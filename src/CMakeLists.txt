cmake_minimum_required(VERSION 3.13)
project(FW800USBWake C)

# FW800 USB Wake (uwake1): the experiment that asks whether APPLE'S OWN USB stack can survive the FW800's deep sleep
# (see ../README.md). Builds ONLY the resident PowerPC fragment; the 68K INIT and the Rez packing are done by
# build-usb-wake.sh (the m68k toolchain cannot link PPC). The fragment is a native driver: main == DoDriverIO (patched
# after MakePEF), exporting DoDriverIO + TheDriverDescription + InstallMe (NECQuiet's vehicle).
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
#   cmake --build build --target usb_wake_pef
# QEMU self-test build (QEMU's OHCI stands in for the NEC; a simulated cycle runs 90 s after boot): -DUWAKE_QTEST=1

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_compile_options(-Wno-multichar -Wall)

add_library(FW800USBWakeResident SHARED usb_wake_resident.c)
set_target_properties(FW800USBWakeResident PROPERTIES
    LINK_FLAGS "-Wl,-bE:${CMAKE_CURRENT_SOURCE_DIR}/usb_wake_resident.exp"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/usb_wake_resident.exp")
target_link_libraries(FW800USBWakeResident NameRegistryLib DriverServicesLib PCILib DriverLoaderLib InterfaceLib)
# The mode: B = Apple's PowerBook mode (the USB Expert removes USB at the sleep demand and reloads it at the wake),
#           A = keep Apple's stack (restore the controllers at 250, interrupts on at 350: v7, corrected).
set(UWAKE_MODE "B" CACHE STRING "A or B")
if(UWAKE_MODE STREQUAL "A")
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_MODE_A=1)
elseif(UWAKE_MODE STREQUAL "B")
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_MODE_B=1)
else()
    message(FATAL_ERROR "UWAKE_MODE must be A or B")
endif()
if(UWAKE_QTEST)
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST=1)
endif()
# uwake22 (1.0): the DEV build's status row (12 x 12 cells painted at the bottom-left of the screen). OFF = the ship build,
# which paints nothing: the screen code is compiled out. build-usb-wake.sh passes it explicitly on every configure, so a
# cache left by an earlier dev configure cannot carry it into a ship build.
option(UWAKE_PAINT "DEV build: paint the status row (never ship)" OFF)
if(UWAKE_PAINT)
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_PAINT=1)
endif()
if(UWAKE_QTEST_EDVM)        # QEMU only: read the control EDs although QEMU's image runs with VM on
    if(NOT UWAKE_QTEST)
        message(FATAL_ERROR "UWAKE_QTEST_EDVM is a QEMU build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_EDVM=1)
endif()
if(UWAKE_QTEST_NOSYS)       # the QEMU control: mode B's reload WITHOUT system mode (never for hardware)
    if(NOT UWAKE_QTEST)
        message(FATAL_ERROR "UWAKE_QTEST_NOSYS is a QEMU control build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_NOSYS=1)
endif()
if(UWAKE_QTEST_STALE)       # QEMU only, fault injection: the startup memory map made WRONG on purpose (the map check
    if(NOT UWAKE_QTEST)     # must find it, drop it and take it again)
        message(FATAL_ERROR "UWAKE_QTEST_STALE is a QEMU build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_STALE=1)
endif()
if(UWAKE_QTEST_RCV)         # QEMU only: the stick's healthy pipe reported dead on purpose, so fn1's failed-add recovery
    if(NOT UWAKE_QTEST)     # (the root-hub power off and on) runs where QEMU can show it
        message(FATAL_ERROR "UWAKE_QTEST_RCV is a QEMU build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_RCV=1)
endif()
if(UWAKE_QTEST_KEEP)        # QEMU only (mode A): the simulated cycle leaves out the power loss (the NEC kept its state,
    if(NOT UWAKE_QTEST)     # as armed on the FW800 it should): the restore must leave it alone, Apple's level 0 resumes it
        message(FATAL_ERROR "UWAKE_QTEST_KEEP is a QEMU build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_KEEP=1)
endif()
if(UWAKE_QTEST_DISARM)      # QEMU only (mode A, uwake11): the first survey clears the remote wakeup Apple's stack set on
    if(NOT UWAKE_QTEST OR NOT UWAKE_MODE STREQUAL "A")   # the keyboard (and hub), so OUR SET_FEATURE is what runs
        message(FATAL_ERROR "UWAKE_QTEST_DISARM is a QEMU mode A build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_DISARM=1)
endif()
if(UWAKE_QTEST_READVM)      # QEMU only, fault injection (uwake16): the activity hook reads the stack although VM is on
    if(NOT UWAKE_QTEST)     # (q16K2, q16K3: it FROZE QEMU ~30 s after the simulated wake; q16N1 without it did not)
        message(FATAL_ERROR "UWAKE_QTEST_READVM is a QEMU build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_READVM=1)
endif()
if(UWAKE_QTEST_NOARM)       # QEMU only (mode A, uwake11): the control: the survey arms nothing (with DISARM: a key must
    if(NOT UWAKE_QTEST OR NOT UWAKE_MODE STREQUAL "A")   # then signal NO remote wakeup)
        message(FATAL_ERROR "UWAKE_QTEST_NOARM is a QEMU mode A build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_NOARM=1)
endif()

if(UWAKE_QTEST_CFLOST)      # QEMU only (mode A, uwake20): the simulated sleep also clears the EHCI's CONFIGFLAG (its
    if(NOT UWAKE_QTEST OR NOT UWAKE_MODE STREQUAL "A")   # auxiliary power lost): the wake must leave it halted
        message(FATAL_ERROR "UWAKE_QTEST_CFLOST is a QEMU mode A build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_CFLOST=1)
endif()
if(UWAKE_QTEST_PRESET)      # QEMU only (mode A, uwake20): the simulated sleep resets the stick's EHCI port (its device no
    if(NOT UWAKE_QTEST OR NOT UWAKE_MODE STREQUAL "A")   # longer suspended at the wake): the wake must not resume it
        message(FATAL_ERROR "UWAKE_QTEST_PRESET is a QEMU mode A build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_PRESET=1)
endif()
if(UWAKE_QTEST_PUMP)        # QEMU only (uwake21): a stand-in for the USB 2.0 driver's keepalive, a silent request every
    if(NOT UWAKE_QTEST)     # 2 s from the heartbeat in the quiet window, plus one marked request (the control)
        message(FATAL_ERROR "UWAKE_QTEST_PUMP is a QEMU build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_PUMP=1)
endif()
if(UWAKE_QTEST_NOQUIET)     # QEMU only (uwake21): the control: a silent request counts as activity again, as in Mac OS
    if(NOT UWAKE_QTEST)
        message(FATAL_ERROR "UWAKE_QTEST_NOQUIET is a QEMU control build only")
    endif()
    target_compile_definitions(FW800USBWakeResident PRIVATE UWAKE_QTEST_NOQUIET=1)
endif()

add_custom_command(OUTPUT usb_wake_resident.pef
    COMMAND ${MAKE_PEF} libFW800USBWakeResident.so -o usb_wake_resident.pef
    COMMAND /usr/bin/env python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch-pef-main.py usb_wake_resident.pef DoDriverIO
    DEPENDS FW800USBWakeResident ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch-pef-main.py
    COMMENT "MakePEF + main->DoDriverIO -> usb_wake_resident.pef")
add_custom_target(usb_wake_pef ALL DEPENDS usb_wake_resident.pef)
