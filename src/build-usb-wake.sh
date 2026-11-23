#!/bin/bash
# build-usb-wake.sh -- Sleep Assistant (the Apple-stack sleep experiment: "FW800 USB Wake" until 2026-11-12, "FW800 Sleep
# Assistant" until uwake12, 2026-11-13), in two toolchains (NECQuiet's recipe):
#   1. PPC CFM fragment  usb_wake_resident.pef   (cmake: MakePEF + main->DoDriverIO)
#   2. 68K INIT          usb_wake_init.flt       (m68k-apple-macos-gcc -Wl,--mac-flat)
#   3. Rez packs them    FW800USBWake.bin        (type INIT, creator RSED; + the custom icon family)
#   4. the deliverable   SleepAssistant.bin      (named "Sleep Assistant" inside, hasCustomIcon set, CRC recomputed:
#                        scripts/set-custom-icon.py), checked by scripts/check-package.py; and a fork-ful "Sleep
#                        Assistant" with the same Finder flag, for the QEMU scripts (ditto into an image)
# UWAKE_MODE=A|B (default B) picks the mode; the hardware build goes in build-<mode>/.
# UWAKE_QTEST=1 BD=<a dir outside build-*/> builds the QEMU self-test variant (QEMU's OHCI stands in for the NEC).
# UWAKE_PAINT=1 (uwake22) builds the DEV variant, which paints the status row, in build-<mode>-dev/ (never ship it); the
# default (0) is the ship build, which paints nothing. A mode A ship build is checked as a RELEASE: Get Info shows the
# plain version (kUwVer) and the code must not be a dev build's.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
RB="$HOME/Retro68-build"
M68KGCC="$RB/toolchain-m68k/bin/m68k-apple-macos-gcc"
PPCBIN="$RB/toolchain/bin"
REZ="$PPCBIN/Rez"
RINC="$RB/toolchain-m68k/m68k-apple-macos/RIncludes"
LIBRETRO_R="$(dirname "$(find "$HOME/Retro68" "$RB" -name Retro68.r 2>/dev/null | head -1)")"
TC="$RB/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"
MODE="${UWAKE_MODE:-B}"
case "$MODE" in A|B) ;; *) echo "FATAL: UWAKE_MODE must be A or B"; exit 1;; esac
PAINT="${UWAKE_PAINT:-0}"
case "$PAINT" in 0|1) ;; *) echo "FATAL: UWAKE_PAINT must be 0 or 1"; exit 1;; esac
if [ "$PAINT" = 1 ]; then BD="${BD:-$HERE/build-$MODE-dev}"; else BD="${BD:-$HERE/build-$MODE}"; fi
if [ "$PAINT" = 1 ] && [ "$BD" = "$HERE/build-$MODE" ]; then
    echo "FATAL: the DEV (status row) build must not go in $BD, the ship build's directory"; exit 1
fi
mkdir -p "$BD"
case "$BD" in "$HERE"/build*) if [ -n "${UWAKE_QTEST:-}" ]; then
    echo "FATAL: the QEMU self-test build (UWAKE_QTEST) must not go in $HERE/build*"; exit 1; fi;; esac

echo "=== 1. usb_wake_resident.pef (PPC CFM fragment), mode $MODE${UWAKE_QTEST:+ (UWAKE_QTEST)}$([ "$PAINT" = 1 ] && echo ' (DEV: paints the status row)') ==="
cmake -S "$HERE" -B "$BD" -DCMAKE_TOOLCHAIN_FILE="$TC" -DUWAKE_MODE="$MODE" -DUWAKE_PAINT="$PAINT" ${UWAKE_QTEST:+-DUWAKE_QTEST=1} \
    ${UWAKE_QTEST_NOSYS:+-DUWAKE_QTEST_NOSYS=1} ${UWAKE_QTEST_EDVM:+-DUWAKE_QTEST_EDVM=1} \
    ${UWAKE_QTEST_STALE:+-DUWAKE_QTEST_STALE=1} ${UWAKE_QTEST_RCV:+-DUWAKE_QTEST_RCV=1} \
    ${UWAKE_QTEST_KEEP:+-DUWAKE_QTEST_KEEP=1} ${UWAKE_QTEST_DISARM:+-DUWAKE_QTEST_DISARM=1} \
    ${UWAKE_QTEST_NOARM:+-DUWAKE_QTEST_NOARM=1} ${UWAKE_QTEST_READVM:+-DUWAKE_QTEST_READVM=1} \
    ${UWAKE_QTEST_CFLOST:+-DUWAKE_QTEST_CFLOST=1} ${UWAKE_QTEST_PRESET:+-DUWAKE_QTEST_PRESET=1} \
    ${UWAKE_QTEST_PUMP:+-DUWAKE_QTEST_PUMP=1} ${UWAKE_QTEST_NOQUIET:+-DUWAKE_QTEST_NOQUIET=1} >/dev/null
# uwake11: a failed compile used to fall through here (the pipe into grep hid cmake's status) and package the PREVIOUS
# build's PEF under the new tag. The old PEF is removed first and cmake's own status decides.
rm -f "${BD:?}/usb_wake_resident.pef"
if ! cmake --build "$BD" --target usb_wake_pef > "$BD/build.log" 2>&1; then
    grep -i "warning\|error" "$BD/build.log"; echo "FATAL: the PEF did not build (see $BD/build.log)"; exit 1
fi
grep -i "warning" "$BD/build.log" || true
[ -f "$BD/usb_wake_resident.pef" ] || { echo "FATAL: usb_wake_resident.pef not built"; exit 1; }
ls -l "$BD/usb_wake_resident.pef"

echo "=== 2. usb_wake_init.flt (68K INIT code resource) ==="
"$M68KGCC" -Wno-multichar -O2 -Wl,--mac-flat "$HERE/usb_wake_init.c" -o "$BD/usb_wake_init.flt"
ls -l "$BD/usb_wake_init.flt"

echo "=== 3. Rez -> FW800USBWake.bin (type INIT), mode $MODE ==="
cp "$HERE/usb_wake.r" "$BD/pack.r"
REZDEFS=(-D "UWAKE_MODE_$MODE"); [ "$PAINT" = 1 ] && REZDEFS+=(-D UWAKE_PAINT)
( cd "$BD" && "$REZ" -I"$LIBRETRO_R" -I"$RINC" -I"$HERE" "${REZDEFS[@]}" pack.r \
    -o FW800USBWake.bin --cc FW800USBWake.dsk --cc FW800USBWake -t INIT -c RSED )
ls -l "$BD/FW800USBWake.bin"

# uwake12: "Sleep Assistant" (user, 2026-11-13: not FW800-specific; "FW800 Sleep Assistant" from uwake6 to uwake11)
echo "=== 4. SleepAssistant.bin: its Mac name, the custom icon flag, the MacBinary CRC; checked ==="
rm -f "${BD:?}/SleepAssistant.bin" "${BD:?}/FW800SleepAssistant.bin"
python3 -I "$HERE/scripts/set-custom-icon.py" "$BD/FW800USBWake.bin" "$BD/SleepAssistant.bin" \
    --name "Sleep Assistant"
# uwake19: the expected tag comes from the SOURCE (kUwTag for this mode), not from a number typed here: the line said
# "uwake18$MODE", which was already wrong for mode B (uwake17B) and would have needed a hand edit every build.
TAG="$(command grep -o "kUwTag *\"uwake[0-9]*$MODE\"" "$HERE/usb_wake_resident.c" | command grep -o "uwake[0-9]*$MODE")"
[ -n "$TAG" ] || { echo "FAIL: no kUwTag for mode $MODE in usb_wake_resident.c" >&2; exit 1; }
# uwake22: a mode A ship build (no status row) is a RELEASE: Get Info must show the plain version, the kUwVer defined
# beside this mode's kUwTag. A dev build (and mode B) keeps the tag in vers 1, as before.
REL=""
if [ "$MODE" = A ] && [ "$PAINT" = 0 ]; then
    REL="$(command grep -B1 "kUwTag *\"$TAG\"" "$HERE/usb_wake_resident.c" | command grep -o 'kUwVer *"[^"]*"' \
           | command grep -o '"[^"]*"' | tr -d '"')"
    [ -n "$REL" ] || { echo "FAIL: no kUwVer beside kUwTag $TAG in usb_wake_resident.c" >&2; exit 1; }
fi
python3 -I "$HERE/scripts/check-package.py" "$BD/SleepAssistant.bin" "Sleep Assistant" "$TAG" ${REL:+"$REL"}
# The fork-ful copy for the QEMU scripts: Rez's --cc copy, renamed, with hasCustomIcon set in its FinderInfo (bytes
# 8-9 = fdFlags) so the Finder in QEMU draws the custom icon too.
rm -rf "${BD:?}/Sleep Assistant" "${BD:?}/FW800 Sleep Assistant"
cp -p "$BD/FW800USBWake" "$BD/Sleep Assistant"
FI="$(xattr -px com.apple.FinderInfo "$BD/Sleep Assistant" | tr -d ' \n')"
FI="$(python3 -I -c 'import sys; h=bytearray.fromhex(sys.argv[1]); h[8]|=0x04; print(h.hex())' "$FI")"
xattr -wx com.apple.FinderInfo "$FI" "$BD/Sleep Assistant"
echo "=== done. SleepAssistant.bin md5: $(md5 -q "$BD/SleepAssistant.bin") ==="
