/*
 * usb_wake_init.c -- FW800 USB Wake, part 1 of 2: the thin 68K INIT.
 *
 * FW800 USB Wake (uwake1, 2026-11-12) is the experiment that decides whether APPLE'S OWN USB stack can survive the
 * FW800's deep sleep (see the README). It runs on the stock / base Mac OS ROM, where Apple's
 * OHCI driver owns the NEC. This INIT only loads the resident PowerPC fragment (usb_wake_resident.pef, embedded as a
 * 'PPC ' (128) resource) and calls its InstallMe through the Mixed Mode Manager; InstallMe makes the fragment a
 * resident native driver (InstallDriverFromMemory), and the resident copy installs the NEC power handlers and the
 * recorder. The vehicle is NECQuiet's (pm-necquiet/necquiet_init.c), proven on this machine in October.
 *
 * Defensive throughout: any failure returns cleanly so the boot proceeds. Recovery from a bad INIT on the base ROM:
 * hold Shift at startup (Apple's stack gives the keyboard early there), or remove the extension from Tiger.
 */
#include <OSUtils.h>
#include <MacMemory.h>
#include <Resources.h>
#include <Gestalt.h>
#include <CodeFragments.h>
#include <MixedMode.h>
#include <Files.h>
#include "Retro68Runtime.h"

enum { uppInstallMeProcInfo = kCStackBased };   /* void InstallMe(void) */

void _start(void)
{
    long cfmAttr = 0;
    Handle h = NULL;
    CFragConnectionID connID = (CFragConnectionID)0;
    Ptr mainAddr; Str255 errName;
    Ptr symAddr; CFragSymbolClass symClass;
    UniversalProcPtr upp;
    OSErr err;
    Boolean haveConn = false;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    if (Gestalt(gestaltCFMAttr, &cfmAttr) != noErr) goto done;
    if (!(cfmAttr & (1L << gestaltCFMPresent))) goto done;

    h = Get1Resource('PPC ', 128);
    if (h == NULL) goto done;
    HLock(h);

    /* Transient load just to call InstallMe; the RESIDENT copy is created by InstallDriverFromMemory inside
     * InstallMe (NewPtrSys-copied there), owned by the Device Manager, so this connection can be dropped. */
    err = GetMemFragment(*h, GetHandleSize(h), "\pFW800USBWakeLoader", kPrivateCFragCopy,
                         &connID, &mainAddr, errName);
    if (err != noErr) goto done;
    haveConn = true;

    err = FindSymbol(connID, "\pInstallMe", &symAddr, &symClass);
    if (err != noErr) goto done;

    /* MUST be NewRoutineDescriptorTrap: plain NewRoutineDescriptor is a 68K no-op passthrough macro, so JSRing its
     * result would execute PPC data as 68K (the r2b1 boot crash). */
    upp = NewRoutineDescriptorTrap((ProcPtr)symAddr, uppInstallMeProcInfo, kPowerPCISA);
    if (upp == NULL) goto done;
    (*(void (*)(void))upp)();
    DisposeRoutineDescriptorTrap(upp);

done:
    if (haveConn) (void)CloseConnection(&connID);
    if (h) HUnlock(h);
    Retro68FreeGlobals();
}
