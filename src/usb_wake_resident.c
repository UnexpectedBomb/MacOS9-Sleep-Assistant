/*
 * usb_wake_resident.c -- Sleep Assistant (the "FW800 USB Wake" experiment, uwake1; "FW800 Sleep Assistant" from
 * 2026-11-12, "Sleep Assistant" from uwake12, 2026-11-13, both at the user's request: the file, its log "Sleep Assistant
 * Log"; the custom icon is the Energy Saver control strip module's; internal names stay FW800USBWake),
 * part 2 of 2: the resident PowerPC fragment.
 *
 * THE QUESTION (see the README): can APPLE'S OWN USB stack survive the FW800's deep sleep?
 * In sleep the on-board NEC (two OHCI functions + an EHCI, one shared interrupt) loses power: PCI config and every OHCI
 * register reset. Apple's OHCI driver treats the NEC as a controller that keeps power (its quirk 0x80: no PCI power
 * management; at wake it only moves HcControl back to Operational), so the stock wake dies in it. Two ways to answer,
 * one per build (-DUWAKE_MODE_A or -DUWAKE_MODE_B):
 *
 * MODE B, "Apple's PowerBook mode" (the first experiment). The USB Expert carries Apple's own sleep procedure for
 *   machines whose USB loses power in sleep (Expert code 0xC870; installed only on PowerBook1,1, or with no Native Power
 *   Manager): at the sleep demand it removes every USB bus and finalizes every UIM while the controllers still have
 *   power; at the wake it reloads every UIM (UIMInitialize: a new HCCA, lists, FmInterval, ports) and re-enumerates
 *   every device from scratch. This build points the Expert's sleep-queue record (gUSBGlobals+0x90) at a thin wrapper
 *   of ours around that procedure; at the wake the wrapper first restores each NEC function's PCI config (bus master
 *   off: the UIM turns it on), then lets Apple reload, in "system mode" so the reloaded UIMs belong to the system and
 *   not to whichever application asked to sleep. No device power handler; nothing of ours in the wake device loop.
 *
 * MODE A, "keep Apple's stack" (the fallback). NECQuiet v7's (run 56) restore at 250 + enable at 350, corrected by a
 *   reading of Apple's driver (2026-11-12): no blanket acknowledge (it discards a done queue and the root hub's change
 *   notice), every OHCI function must decode before any interrupt is enabled (Apple's ISR claims an all-ones read
 *   forever), Resume Detected cleared, UE held off for the first minutes, the interrupt enables Apple left at sleep
 *   restored with MIE last, HcFmNumber put back.
 *
 * Both: a recorder (DriverServices SetInterruptTimer, secondary interrupt level, MMIO READS and memory only) every 8 ms
 * for 60 s (and again for 30 s after any port change: a replug), then every 50 ms to 360 s (QEMU builds: 180 s): each
 * OHCI function's HcControl, HcInterruptStatus, HcInterruptEnable, HcDoneHead (uwake5: with every retired TD's CC,
 * direction and buffer), list registers, frame counter and port status; the cursor and the keyboard as the system sees
 * them (low memory); a File Manager call held at the head; whether Ticks advance; the wall clock's first ticks (to line
 * the log up with a network capture). Changes go to a RAM ring; in a DEV build only (UWAKE_PAINT, uwake22) a status row
 * is painted at the bottom-left of the screen (it survives a hang); the ring is written to "Sleep Assistant Log" (System
 * Folder) by a task-level Notification Manager pass once the wake settles. A ship build paints nothing.
 *
 * ⚠ LEVELS. Mode A's power handlers run in the sleep/wake device loop (interrupts off on the real wake); mode B's
 * wrapper runs in the Power Manager's sleep queue (task level, inside Sleep: no File Manager, no allocation, no screen);
 * the recorder at secondary interrupt level. The File Manager, the Toolbox and allocation happen ONLY at install (task
 * level) and in the NM pass. audit-levels.py walks the call graph.
 */
#include <MacTypes.h>
#include <MacMemory.h>
#include <Resources.h>
#include <OSUtils.h>
#include <Devices.h>
#include <DriverServices.h>
#include <DriverFamilyMatching.h>
#include <NameRegistry.h>
#include <CodeFragments.h>
#include <MixedMode.h>
#include <PCI.h>
#include <Timer.h>
#include <Quickdraw.h>
#include <Notification.h>
#include <Files.h>
#include <Folders.h>
#include <Gestalt.h>
#include <Errors.h>
#include <Patches.h>             /* the put-away hook: GetToolboxTrapAddress(_AliasDispatch), for ReleaseFolder */
#include <Processes.h>           /* uwake13: Web Sharing's process, as NPM's FindTypeCreatorProcess looks for it */
#include <USB.h>                 /* uwake11: USBPB and the request constants (USBDeviceRequest itself via FindSymbol) */
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>              /* the put-away hook: offsetof, for its compile-time layout checks */
#include <string.h>
#include "map_scan.h"            /* uwake8: the memory map's scan, shared with the host test scripts/test-map-scan.c */

#if defined(UWAKE_MODE_A) && defined(UWAKE_MODE_B)
#error "one mode per build"
#endif
#if !defined(UWAKE_MODE_A) && !defined(UWAKE_MODE_B)
#error "build with -DUWAKE_MODE_A or -DUWAKE_MODE_B"
#endif

#ifdef UWAKE_MODE_B
#define kUwVer   "1.0d1"
#define kUwTag   "uwake17B"
#define kUwMode  "B, Apple's PowerBook mode: the USB Expert removes USB at the sleep demand and reloads it at the wake"
#else
/* ★ 1.0 (user, 2026-11-17: "Yes, go ahead with 1.0. Make sure to remove the blinking square beacons in the shipping
   version as well."): uwake22A is uwake21A's code with the status row compiled out (UWAKE_PAINT, below). Get Info shows
   "1.0" only (usb_wake.r, as AirPort's releases do); the build stays visible in the log's banner. */
#define kUwVer   "1.0"
#define kUwTag   "uwake22A"
#define kUwMode  "A, keep Apple's stack alive, as on KeyLargo: at the demand (250, after Apple's suspend) the NEC armed to keep its state (PME, D3hot), with remote wakeup on the keyboard's function (uwake11); at the wake D0 first, a restore only if it was lost, interrupts at 350; servers put away before every chosen or idle sleep (uwake12), except an idle sleep Mac OS refuses for sharing (uwake13); a visible EHCI that no driver runs no longer stops it (uwake19); a RUNNING EHCI is suspended and halted at the demand and resumed at the wake (uwake20); a silent Notification Manager request (no mark, icon, sound or text) no longer counts as activity (uwake21)"
#endif

/* ★ uwake22: the status row (12 x 12 cells at the bottom-left, "the screen row" below) is a DEVELOPMENT diagnostic. The
   user's standing rule (2026-08-13, 2026-11-09): "We don't want anything painted onto the desktop for a ship build", and
   a separate dev build keeps the paint "for our troubleshooting/dev purposes only". So the screen code is COMPILED OUT
   of a ship build, not switched off at run time: no GetMainDevice import, nothing that can find the screen
   (scripts/pef-screen-audit.py checks the binary). The dev build: cmake -DUWAKE_PAINT=1 (build-usb-wake.sh UWAKE_PAINT=1,
   its own build directory). One source tree; the banner names the variant. */
#ifdef UWAKE_PAINT
#define kUwVariant ", DEV BUILD: paints the status row"
#else
#define kUwVariant ""
#endif

#ifndef noErr
#define noErr 0L
#endif
#define kImmediateIOCommandKind 0x00000004UL

/* ---- PM messages (as NativePowerMgrLib numbers them) ---------------------- */
#define kPMSleepRequest      1UL
#define kPMSleepDemand       2UL
#define kPMSleepWakeUp       3UL
#define kPMSleepRevoke       4UL
#define kPMDozeWakeUp        8UL
#define kPMSuspendDemand     13UL
#define kPMSuspendWakeUp     15UL
#define kPMWakeToDoze        19UL
#define kPMDozeToFullWakeUp  20UL
#define kPMMessageNotHandled (-13009)

/* ---- OHCI 1.0a operational registers (little-endian, at AAPL,address) ------ */
#define kHcRevision        0x00UL
#define kHcControl         0x04UL
#define kHcCommandStatus   0x08UL
#define kHcInterruptStatus 0x0CUL
#define kHcInterruptEnable 0x10UL
#define kHcInterruptDisable 0x14UL
#define kHcHCCA            0x18UL
#define kHcControlHeadED   0x20UL
#define kHcControlCurrentED 0x24UL
#define kHcBulkHeadED      0x28UL
#define kHcBulkCurrentED   0x2CUL
#define kHcDoneHead        0x30UL
#define kHcFmInterval      0x34UL
#define kHcFmNumber        0x3CUL
#define kHcPeriodicStart   0x40UL
#define kHcLSThreshold     0x44UL
#define kHcRhDescriptorA   0x48UL
#define kHcRhDescriptorB   0x4CUL
#define kHcRhStatus        0x50UL
#define kHcRhPortStatus1   0x54UL
#define kMaxPorts          15
#define kIntUE             0x00000010UL
#define kIntMIE            0x80000000UL

#define kLowTime     (*(volatile UInt32 *)0x020CUL)      /* Time: the wall clock, seconds since 1904 */
#define kLowTicks    (*(volatile UInt32 *)0x016AUL)
#define kLowFSQHead  (*(volatile UInt32 *)0x0362UL)      /* FSQHdr (0x360) qHead: the File Manager call in progress */
#define kLowRawMouse (*(volatile UInt32 *)0x082CUL)      /* RawMouse: where the mouse has put the cursor (v, h) */
#define kLowKeyMap   ((volatile UInt32 *)0x0174UL)        /* KeyMap: 128 bits, one per key held down */

/* ============================================================== the log (task level ONLY) */
static char  gLog[48 * 1024];             /* text not yet written */
static long  gLogLen;
static short gLogVRef; static long gLogDir; static Boolean gLogReady;

/* TASK LEVEL: append the buffer to the file. The file is NEVER truncated: each boot appends a dated section, so the
   report of a wake that left no keyboard (written ~3 min after it, before the operator power-cycles) survives the
   restart. Read from the LAST banner; delete the file before a test to start clean. */
static void LogFlush(void)
{
    FSSpec sp; short ref; long n, eof; OSErr e;
    if (gLogLen == 0) return;
    if (!gLogReady) {
        if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &gLogVRef, &gLogDir) != noErr) return;
        gLogReady = true;
    }
    e = FSMakeFSSpec(gLogVRef, gLogDir, "\pSleep Assistant Log", &sp);    /* uwake12: "Sleep Assistant" (user, 2026-11-13) */
    if (e == fnfErr) e = FSpCreate(&sp, 'ttxt', 'TEXT', smSystemScript);
    if (e != noErr && e != dupFNErr) return;
    if (FSpOpenDF(&sp, fsRdWrPerm, &ref) != noErr) return;
    if (GetEOF(ref, &eof) != noErr) eof = 0;
    n = gLogLen;
    if (SetFPos(ref, fsFromStart, eof) == noErr && FSWrite(ref, &n, gLog) == noErr) gLogLen = 0;
    FSClose(ref);
    (void)FlushVol(NULL, gLogVRef);
}

static void Lg(const char *fmt, ...)   /* TASK LEVEL ONLY (it may write the file when the buffer fills) */
{
    char b[300]; va_list ap; int n;
    va_start(ap, fmt); n = vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    if (n < 0) return;
    /* A cut line holds sizeof b - 3 characters, then vsnprintf's NUL (uwake7 QEMU snh15: clamping to sizeof b - 2 wrote
       that NUL into the log). The CR goes over the NUL. */
    if (n > (int)sizeof b - 3) n = (int)sizeof b - 3;
    b[n++] = '\r';
    if (gLogLen + n > (long)sizeof gLog) LogFlush();
    if (gLogLen + n > (long)sizeof gLog) return;
    memcpy(gLog + gLogLen, b, (size_t)n); gLogLen += n;
}

static void LgStamp(char *b, int max)  /* TASK LEVEL: "2026-11-12 15:42:07" */
{
    unsigned long secs = 0; DateTimeRec dt;
    GetDateTime(&secs); SecondsToDate(secs, &dt);
    snprintf(b, (size_t)max, "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
}

/* ============================================================== the NEC functions (NECQuiet's, verbatim) */
#define kMaxFns 8
typedef struct {
    RegEntryID    id;
    unsigned long vid, did, cls, reg0;
    Boolean       nec;
    unsigned long pre[64];        /* golden config (BootROM-set), the restore source */
    Boolean       preOK;
} UsbFn;
static UsbFn gFn[kMaxFns];
static short gFnN;

typedef struct {
    Boolean       ok;
    unsigned long rev, ctl, cmdSt, intSt, intEn, hcca, ctlHead, bulkHead,
                  fmInterval, fmNumber, perStart, lsThresh, rhA, rhB, rhSt;
    short         ndp;
    unsigned long port[kMaxPorts];
} OhciRegs;

/* uwake19: the NEC's EHCI (fn2), which the USB 2.0 ROM shows and the stock firmware hides (fn0's E4 bit 0). EHCI 1.0:
   capability registers at BAR0 (CAPLENGTH byte 0, HCIVERSION bytes 2-3, HCSPARAMS +4, N_PORTS its low nibble),
   operational registers at BAR0 + CAPLENGTH (USBCMD +0, bit 0 Run/Stop; USBSTS +4, bit 12 HCHalted; CONFIGFLAG +0x40,
   bit 0 = the ports are routed to the EHCI; PORTSC +0x44 + 4n, bit 13 PORT_OWNER = the companion OHCI owns the port,
   bit 12 power, bit 2 enabled, bit 0 a device connected). uwake19 only read them; uwake20 writes them, only for an EHCI
   it found RUNNING at the sleep demand (EhciDemand), and the wake undoes exactly that (EhciWake). */
#define kEhUsbCmd   0x00UL
#define kEhUsbSts   0x04UL
#define kEhUsbIntr  0x08UL
#define kEhFrIndex  0x0CUL
#define kEhCtrlDs   0x10UL                       /* CTRLDSSEGMENT: the high address word (our driver writes 0) */
#define kEhPerBase  0x14UL                       /* PERIODICLISTBASE: the frame list, 4 KB aligned */
#define kEhAsync    0x18UL                       /* ASYNCLISTADDR: the next queue head of the async ring, 32-byte aligned */
#define kEhCfgFlag  0x40UL
#define kEhPortSc   0x44UL
#define kEhCmdRS    0x00000001UL
#define kEhCmdHCRst 0x00000002UL
#define kEhCmdPSE   0x00000010UL
#define kEhCmdASE   0x00000020UL
#define kEhCmdIAAD  0x00000040UL                 /* the async-advance doorbell: never written back (it would ring) */
#define kEhCmdLHCR  0x00000080UL
#define kEhCmdKeep  (~(kEhCmdHCRst | kEhCmdIAAD | kEhCmdLHCR))
#define kEhStsHalt  0x00001000UL
#define kEhStsPSS   0x00004000UL
#define kEhStsASS   0x00008000UL
#define kEhPsCCS    0x00000001UL
#define kEhPsCSC    0x00000002UL                 /* RW1C */
#define kEhPsPE     0x00000004UL
#define kEhPsPEC    0x00000008UL                 /* RW1C */
#define kEhPsOCC    0x00000020UL                 /* RW1C */
#define kEhPsFPR    0x00000040UL                 /* Force Port Resume */
#define kEhPsSusp   0x00000080UL
#define kEhPsPR     0x00000100UL
#define kEhPsPP     0x00001000UL
#define kEhPsOwner  0x00002000UL
#define kEhPsWake   0x00700000UL                 /* WKCNNT_E | WKDSCNNT_E | WKOC_E */
#define kEhPsRW1C   (kEhPsCSC | kEhPsPEC | kEhPsOCC)
#define kMaxEhPorts 8
typedef struct {
    Boolean       ok;                            /* read, and the capability registers made sense */
    unsigned long cap, hcs, cmd, sts, cfg;       /* CAPLENGTH|HCIVERSION, HCSPARAMS, USBCMD, USBSTS, CONFIGFLAG */
    short         n;                             /* ports read (N_PORTS, at most kMaxEhPorts) */
    unsigned long port[kMaxEhPorts];
} EhciRegs;

enum { kOutNone = 0, kOutRestored, kOutIntact, kOutAbsent, kOutVerifyFail };
enum { kOhciNone = 0, kOhciRestored, kOhciIntact, kOhciNoSnap, kOhciNotAnswering, kOhciVerifyFail };
enum { kEnNone = 0, kEnOn, kEnWithheldDecode, kEnWithheldUE, kEnNotOurs };

/* the recorder's per-function memory */
typedef struct {
    Boolean       live;           /* sample this function (an OHCI whose config decodes after the restore) */
    unsigned long ctl, ist, ien, done, fm;
    unsigned long port[4];
    short         fmSame;         /* passes the frame counter has not moved */
    Boolean       fmStuck, ueSeen;
    unsigned long istN, doneN;    /* changes seen; only the first kChurnLog of each go to the ring */
} RecFn;
#define kChurnLog 300

#define kMaxNec 4
typedef struct {
    short          fi;
    Boolean        ohci;
    unsigned short pmOff;
    unsigned long  base;
    Boolean        baseOK;
    OhciRegs       launch;
    OhciRegs       req;                          /* the sleep request (still D0): the restore source */
    Boolean        slpOK; short slpBy;           /* mode A: the sleep demand, after Apple's level 1 (HCFS Suspend) */
    unsigned long  slpCtl, slpIen, slpIst;
    unsigned long  hcdSlp, hcdWake, hcdEn;       /* HCCA+0x84 in RAM (a done batch nobody consumed); ~0 = not read */
    unsigned long  dCmd, dBar0, dPMCSR;
    unsigned long  wId, wCmd, wBar0, wPMCSR, wE0, wE4, wCL;
    short          outcome;
    unsigned long  cfgWrites;
    unsigned long  aCmd, aBar0, aPMCSR, aE4;
    unsigned long  oRev, oCtl, oHCCA;
    short          ohciOutcome;
    Boolean        usedReq;
    short          hcrPolls;
    unsigned long  mmioWrites;
    unsigned long  fmWrote, fmBack;
    OhciRegs       after;
    unsigned long  enRev, enIen, enCtl, enIst, enMask;   /* mode A: what the enable found (Apple's level 0 had run) */
    short          enOutcome;
    Boolean        enForcedOp, ueBack;
    unsigned long  armPm0, armPm1, wkPm;         /* uwake10 (mode A): PMCSR at the demand before / after our arming, and
                                                    at the wake before our D0 write (0xFFFF = not read) */
    short          armWhy;                       /* 0 armed; 1 no PCI PM capability; 2 Apple had not suspended it; 9 not
                                                    reached (no demand seen) */
    /* uwake11 (mode A): keyboard wake. 0xFFFFFFFF = not read */
    short          kwWhy;                        /* at the demand: 0 remote wakeup SET; 1 no armed keyboard on this
                                                    function; 2 Apple had not suspended it; 3 no survey had finished;
                                                    4 the write did not take; 9 not reached */
    unsigned long  kwCtl0, kwCtl1, kwRh0, kwRh1, kwIst0;   /* HcControl, HcRhStatus at the demand before / after our
                                                    writes; InterruptStatus before */
    unsigned long  kwP0[4];                      /* the root ports at the demand */
    short          kwTries;                      /* HcControl writes until RWC|RWE read back (NEC errata: up to 10) */
    Boolean        kwRdClr, kwDrweOff;           /* we cleared a stale RD; we cleared DRWE (put back at the enable) */
    unsigned long  kwWkCtl, kwWkIst, kwWkRh;     /* at the wake, after our restore, before Apple's level 0 */
    unsigned long  kwWkP[4];
    unsigned long  kwEnRh;                       /* HcRhStatus after DRWE was put back at the enable */
    unsigned long  kwQMie; long kwQRdMs;         /* QEMU self-test: MIE masked for the hold; RD seen at (-1 never) */
    /* uwake12: this wake's sleep, kept at the wake for its report. The next sleep's request replaces req and clears
       slpOK, and a report written after it (the next sleep began within the 6 minutes) printed that request's values
       (uwake11A on the FW800, report 2 of log d9e6b6ef). */
    OhciRegs       wReq;
    Boolean        wSlpOK; short wSlpBy;
    unsigned long  wSlpCtl, wSlpIen, wSlpIst;
    RecFn          rec;
    /* uwake19: the EHCI function. ehBase is AAPL,address when it agrees with BAR0 (uwake20: whether or not the boot
       config decodes, so a driver that turns decode on after boot is still seen at the demand), else 0. */
    Boolean        ehci;
    unsigned long  ehBase;
    EhciRegs       ehBoot, ehDem, ehWake;        /* at boot, at the sleep demand, after the wake's config restore */
    /* uwake20 (mode A): a RUNNING EHCI carried through the sleep (EhciDemand, EhciWake, EhciEnable). */
    short          ehWhy;                        /* the demand: 0 stopped and in D3hot; 1 not running, nothing to stop;
                                                    2 not readable; 3 stopped but HCHalted never read 1 (D3hot anyway);
                                                    9 not reached */
    Boolean        ehSusp;                       /* we stopped it at this sleep's demand (the wake resumes only that) */
    unsigned long  ehCmd, ehIntr, ehFrIdx, ehSeg, ehPer, ehAsync;   /* at the demand, before our first write */
    unsigned long  ehStsQ, ehStsH, ehAsyncH;     /* USBSTS once the schedules stopped, once halted; ASYNCLISTADDR halted */
    UInt32         ehQMs, ehHMs;                 /* how long the schedules took to stop, the halt took */
    UInt8          ehSuspMask;                   /* the ports we suspended (bit n = port n + 1) */
    unsigned long  ehPortS[kMaxEhPorts];         /* PORTSC after our suspend writes */
    unsigned long  ehPm0, ehPm1;                 /* PMCSR before / after our D3hot write (0xFFFF = not written) */
    short          ewWhy;                        /* the wake: 0 resumed; 1 we had not stopped it; 2 not readable after the
                                                    restore; 3 CONFIGFLAG 0 (left halted); 4 the list bases did not read
                                                    back (left halted); 5 Run did not take; 9 not reached */
    unsigned long  ewPm, ewIntr, ewFrIdx, ewPer, ewAsync;   /* at the wake, before our writes: what the core well kept */
    unsigned long  ewPerB, ewAsyncB, ewStsRs;    /* the bases read back; USBSTS after Run */
    UInt8          ewResMask, ewGoneMask;        /* ports resumed; ports we suspended whose device did not stay suspended */
    UInt32         ewRsMs, ewFprMs, ewSchMs;     /* Run, the end of the resume, the schedules: how long each took */
    unsigned long  ewPortF[kMaxEhPorts];         /* PORTSC once resumed */
    unsigned long  ewCmd, ewSts;                 /* once the schedules are back */
    Boolean        ewRun;                        /* Run took: the enable (350) puts the interrupt mask back */
    unsigned long  eeIntr;                       /* at the enable: USBINTR read back after our write (0xFFFFFFFF = none) */
} NecFn;
static NecFn   gNec[kMaxNec];
static short   gNecN;
static Boolean gNecReady;
static UInt32  gLRam;                   /* Gestalt 'lram': a plausible-pointer bound */
static Boolean gRamReadable;            /* VM off: the HCCA's physical address is a usable pointer */

static unsigned long Swap32(unsigned long v)
{
    return ((v & 0x000000FFUL) << 24) | ((v & 0x0000FF00UL) << 8) |
           ((v & 0x00FF0000UL) >> 8)  | ((v & 0xFF000000UL) >> 24);
}
static unsigned long OhciRd(unsigned long base, unsigned long off)
{
    unsigned long v = *(volatile unsigned long *)(base + off);
    __asm__ volatile ("eieio" ::: "memory");
    return Swap32(v);
}
#ifdef UWAKE_MODE_A                     /* mode B leaves the OHCI registers to Apple's UIM, except uwake8's failed-add
                                           recovery (RcvPower: fn1's root-hub power, off and on) */
static void OhciWr(NecFn *n, unsigned long off, unsigned long v)
{
    *(volatile unsigned long *)(n->base + off) = Swap32(v);
    __asm__ volatile ("eieio" ::: "memory");
    n->mmioWrites++;
}
#endif
static unsigned long CfgRd32(UsbFn *f, unsigned short off)
{
    UInt32 v = 0xFFFFFFFFUL;
    if (ExpMgrConfigReadLong(&f->id, (LogicalAddress)(unsigned long)off, &v) != noErr) v = 0xFFFFFFFFUL;
    return (unsigned long)v;
}
static unsigned long CfgRd16(UsbFn *f, unsigned short off)
{
    UInt16 v = 0xFFFF;
    if (ExpMgrConfigReadWord(&f->id, (LogicalAddress)(unsigned long)off, &v) != noErr) v = 0xFFFF;
    return (unsigned long)v;
}
static void CfgWr32(NecFn *n, unsigned short off, unsigned long v)
{ n->cfgWrites++; (void)ExpMgrConfigWriteLong(&gFn[n->fi].id, (LogicalAddress)(unsigned long)off, (UInt32)v); }
static void CfgWr16(NecFn *n, unsigned short off, unsigned long v)
{ n->cfgWrites++; (void)ExpMgrConfigWriteWord(&gFn[n->fi].id, (LogicalAddress)(unsigned long)off, (UInt16)v); }
static void CfgWr8(NecFn *n, unsigned short off, unsigned long v)
{ n->cfgWrites++; (void)ExpMgrConfigWriteByte(&gFn[n->fi].id, (LogicalAddress)(unsigned long)off, (UInt8)v); }

static unsigned long CfgB(const unsigned long *c, unsigned short off)
{ return (c[off >> 2] >> ((off & 3) * 8)) & 0xFFUL; }
static unsigned long CfgW(const unsigned long *c, unsigned short off)
{ return (c[off >> 2] >> ((off & 2) * 8)) & 0xFFFFUL; }

static unsigned short PMCapOff(const unsigned long *c)
{
    unsigned short ptr; short guard;
    if (!(CfgW(c, 0x06) & 0x10)) return 0;
    ptr = (unsigned short)(CfgB(c, 0x34) & 0xFC);
    for (guard = 0; ptr >= 0x40 && ptr < 0x100 && guard < 16; guard++) {
        if (CfgB(c, ptr) == 0x01) return ptr;
        ptr = (unsigned short)(CfgB(c, (unsigned short)(ptr + 1)) & 0xFC);
    }
    return 0;
}

static void DelayMs(long ms)
{
    AbsoluteTime t = DurationToAbsolute((Duration)ms);
    (void)DelayForHardware(t);
}
/* milliseconds since t0. Positive Durations are milliseconds, NEGATIVE ones MICROSECONDS (what a short delta comes
   back as; uwake1's first QEMU run read every delta as 0. Prior art: pm_driverlist.c DeltaMs, Apple's USB DDK samples) */
static UInt32 MsSince(AbsoluteTime t0)
{
    Duration d = AbsoluteDeltaToDuration(UpTime(), t0);
    return (d >= 0) ? (UInt32)d : (UInt32)(-d) / 1000UL;
}

static Boolean NecVendor(unsigned long vid, unsigned long cls, Boolean clsOK)
{
#ifdef UWAKE_QTEST
    if (vid == 0x106BUL && clsOK && (cls & 0xFFFFFFUL) == 0x0C0310UL) return true;   /* QEMU's OHCI stands in */
    if (vid == 0x8086UL && clsOK && (cls & 0xFFFFFFUL) == 0x0C0320UL) return true;   /* uwake19: QEMU's EHCI (usb-ehci)
                                                                                         stands in for the NEC's fn2 */
#else
    (void)cls; (void)clsOK;
#endif
    return vid == 0x1033UL;
}

static Boolean CfgDecodes(NecFn *n)
{
    UsbFn *f = &gFn[n->fi];
    if (CfgRd32(f, 0x10) != f->pre[4]) return false;
    if (!(CfgRd16(f, 0x04) & 0x0002)) return false;
    if (n->pmOff && (CfgRd16(f, (unsigned short)(n->pmOff + 4)) & 3UL) != 0) return false;
    return true;
}

static Boolean OhciSane(const OhciRegs *r)
{
    unsigned long fi = r->fmInterval & 0x3FFFUL;
    return (r->rev & 0xFFUL) == 0x10UL &&
           r->hcca != 0 && (r->hcca & 0xFFUL) == 0 &&
           (r->ctlHead & 0xFUL) == 0 && (r->bulkHead & 0xFUL) == 0 &&
           fi >= 0x2E00UL && fi <= 0x2FFFUL &&
           ((r->fmInterval >> 16) & 0x7FFFUL) != 0 &&
           r->ndp >= 1 && r->ndp <= kMaxPorts;
}

/* uwake19: the EHCI's registers, READS ONLY. The caller has checked that the function decodes (CfgDecodes): an MMIO read
   of a function whose BAR or memory decode is gone is exactly what this file never does. Nothing past the capability
   block is read unless it says EHCI 1.00 with a plausible CAPLENGTH. */
static void ReadEhci(const NecFn *n, EhciRegs *r)
{
    unsigned long op; short i;
    memset(r, 0, sizeof *r);
    if (n->ehBase == 0) return;
    r->cap = OhciRd(n->ehBase, 0x00);
    if ((r->cap >> 16) != 0x0100UL || (r->cap & 0xFFUL) < 0x0CUL || (r->cap & 0xFFUL) > 0x40UL) return;
    op = n->ehBase + (r->cap & 0xFFUL);
    r->hcs = OhciRd(n->ehBase, 0x04);
    r->cmd = OhciRd(op, 0x00); r->sts = OhciRd(op, 0x04); r->cfg = OhciRd(op, 0x40);
    r->n = (short)(r->hcs & 0xFUL); if (r->n > kMaxEhPorts) r->n = kMaxEhPorts;
    for (i = 0; i < r->n; i++) r->port[i] = OhciRd(op, 0x44UL + 4UL * (unsigned long)i);
    r->ok = true;
}
/* uwake19: a driver runs this EHCI -- its schedule is running, or it has taken the ports from the companions */
static Boolean EhciRunning(const EhciRegs *r) { return r->ok && ((r->cmd & 1UL) || (r->cfg & 1UL)); }
/* uwake19: CONFIGFLAG 0 routes every port to the companion OHCIs, and PORT_OWNER then reads 1 on every port */
static Boolean EhciPortsCompanion(const EhciRegs *r)
{
    short i;
    if (!r->ok || (r->cfg & 1UL)) return false;
    for (i = 0; i < r->n; i++) if (!(r->port[i] & 0x2000UL)) return false;
    return true;
}
static void LogEhci(const char *what, const EhciRegs *r)   /* TASK LEVEL */
{
    char pb[kMaxEhPorts * 10 + 4]; short i; int k = 0;
    if (!r->ok) {
        Lg("    EHCI %s: %s", what, r->cap ? "NOT an EHCI 1.00 capability block (nothing more read)" :
           "not read (its BAR or memory decode was not as at boot)");
        return;
    }
    pb[0] = 0;
    for (i = 0; i < r->n; i++) k += snprintf(pb + k, sizeof pb - (size_t)k, " %08lX", r->port[i]);
    Lg("    EHCI %s: CAPLENGTH %02lX HCIVERSION %04lX HCSPARAMS %08lX (%d ports); USBCMD %08lX (%s); USBSTS %08lX (%s); "
       "CONFIGFLAG %08lX (%s)", what, r->cap & 0xFFUL, r->cap >> 16, r->hcs, (int)r->n, r->cmd,
       (r->cmd & 1UL) ? "RUNNING" : "stopped", r->sts, (r->sts & 0x1000UL) ? "halted" : "NOT halted", r->cfg,
       (r->cfg & 1UL) ? "the ports are routed to the EHCI" : "the ports are the companions'");
    Lg("      PORTSC%s (bit 13 = the companion owns it, 12 power, 7 suspended, 6 resuming, 2 enabled, 0 a device): %s", pb,
       EhciPortsCompanion(r) ? "PORT_OWNER set on every port" :
       (r->cfg & 1UL) ? "ports routed to the EHCI" : "PORT_OWNER clear on a port although CONFIGFLAG is 0");
}
static void EhPorts(UInt8 m, char *b, size_t max)            /* TASK LEVEL: "1 and 3" (port numbers from 1), "none" */
{
    short i; int k = 0, c = 0;
    b[0] = 0;
    for (i = 0; i < kMaxEhPorts; i++) if (m & (1U << i)) {
        k += snprintf(b + k, max - (size_t)k, "%s%d", c ? ", " : "", (int)i + 1); c++;
        if ((size_t)k >= max) break;
    }
    if (c == 0) snprintf(b, max, "none");
}
static void EhPortLine(const char *what, const unsigned long *p, short n)   /* TASK LEVEL */
{
    char pb[kMaxEhPorts * 10 + 4]; short i; int k = 0;
    pb[0] = 0;
    for (i = 0; i < n && i < kMaxEhPorts; i++) k += snprintf(pb + k, sizeof pb - (size_t)k, " %08lX", p[i]);
    Lg("      PORTSC %s:%s", what, pb);
}
/* uwake20, TASK LEVEL: the EHCI that EhciDemand stopped, through this sleep and EhciWake's resume, with the verdict */
static void LogEhciCycle(const NecFn *n)
{
    char m1[40], m2[40]; Boolean kept;
    LogEhci("at the sleep demand, before our writes", &n->ehDem);
    Lg("    uwake20 at the demand: RUNNING, so stopped: USBCMD %08lX, USBINTR %08lX, FRINDEX %04lX, CTRLDSSEGMENT %08lX, "
       "PERIODICLISTBASE %08lX, ASYNCLISTADDR %08lX (running)", n->ehCmd, n->ehIntr, n->ehFrIdx & 0x3FFFUL, n->ehSeg,
       n->ehPer, n->ehAsync);
    EhPorts(n->ehSuspMask, m1, sizeof m1);
    Lg("      interrupts masked; schedules off in %lu ms (USBSTS %08lX); ports suspended: %s; halted in %lu ms "
       "(USBSTS %08lX, %s); ASYNCLISTADDR halted %08lX (the wake restarts the ring there)", (unsigned long)n->ehQMs,
       n->ehStsQ, m1, (unsigned long)n->ehHMs, n->ehStsH, (n->ehStsH & kEhStsHalt) ? "HCHalted" : "!! NOT halted in 20 ms",
       n->ehAsyncH);
    EhPortLine("after the suspend writes", n->ehPortS, n->ehDem.n);
    if (n->ehPm0 != 0xFFFFUL) Lg("      PMCSR %04lX -> %04lX (D3hot, PME_Enable 0: the EHCI is no wake source)", n->ehPm0, n->ehPm1);
    else Lg("      no PCI power management capability: left in D0");
    if (n->ewPm != 0xFFFFUL) Lg("    uwake20 at the wake, before our D0 write: PMCSR %04lX = D%lu", n->ewPm, n->ewPm & 3UL);
    LogEhci("after the wake, its PCI config restored, before any write", &n->ehWake);
    if (n->ewWhy == 2) { Lg("    uwake20 VERDICT: !! the EHCI could not be read after the wake's config restore: left as found"); return; }
    if (n->ewWhy == 1 || n->ewWhy == 9) { Lg("    uwake20 VERDICT: !! no wake step ran for the EHCI we stopped"); return; }
    kept = (n->ewPer & ~0xFFFUL) == (n->ehPer & ~0xFFFUL) && (n->ewAsync & ~0x1FUL) == (n->ehAsyncH & ~0x1FUL);
    Lg("    uwake20 the core-well registers as the wake found them: USBINTR %08lX (0 at the sleep), FRINDEX %04lX, "
       "PERIODICLISTBASE %08lX, ASYNCLISTADDR %08lX: %s", n->ewIntr, n->ewFrIdx & 0x3FFFUL, n->ewPer, n->ewAsync,
       kept ? "KEPT (the NEC kept its core power too, as its OHCIs do)" : "LOST (reset values): written back");
    EhPorts(n->ewGoneMask, m2, sizeof m2);
    if (n->ewWhy == 3) {
        Lg("    uwake20 VERDICT: !! CONFIGFLAG read 0 at the wake: the EHCI's auxiliary power was NOT kept through the "
           "sleep (its routing reset: every port back to the OHCIs); left halted. Ports we suspended that lost their "
           "device: %s", m2);
        return;
    }
    if (n->ewWhy == 4) {
        Lg("    uwake20 VERDICT: !! the list bases did not read back as written (PERIODICLISTBASE %08lX for %08lX, "
           "ASYNCLISTADDR %08lX for %08lX): left halted", n->ewPerB, n->ehPer, n->ewAsyncB, n->ehAsyncH);
        return;
    }
    if (n->ewWhy == 5) {
        Lg("    uwake20 VERDICT: !! Run did not take in 20 ms (USBSTS %08lX): left so", n->ewStsRs);
        return;
    }
    EhPorts(n->ewResMask, m1, sizeof m1);
    Lg("    uwake20 resume: the bases read back %08lX / %08lX before Run; Run in %lu ms (USBSTS %08lX); ports resumed "
       "(resume signalling 40 ms): %s, ended in %lu ms; schedules back in %lu ms: USBCMD %08lX, USBSTS %08lX",
       n->ewPerB, n->ewAsyncB, (unsigned long)n->ewRsMs, n->ewStsRs, m1, (unsigned long)n->ewFprMs,
       (unsigned long)n->ewSchMs, n->ewCmd, n->ewSts);
    EhPortLine("after the resume", n->ewPortF, n->ehWake.n);
    if (n->eeIntr != 0xFFFFFFFFUL) Lg("    uwake20 at the enable (350): USBINTR %08lX put back, read %08lX", n->ehIntr, n->eeIntr);
    else Lg("    uwake20 at the enable (350): !! the interrupt mask was not put back (the EHCI did not decode there)");
    if (n->ewGoneMask)
        Lg("    uwake20 VERDICT: !! the EHCI kept its routing (CONFIGFLAG 1) and runs again, but the device on port(s) %s "
           "did not stay suspended (it lost power, or was reset or removed; PORTSC at the wake above). Our driver sees "
           "whatever that port reads now; port(s) resumed: %s", m2, m1);
    else if (n->ewResMask)
        Lg("    uwake20 VERDICT: the EHCI KEPT its routing and its device(s) through the sleep: CONFIGFLAG 1, port(s) %s "
           "connected, enabled and suspended at the wake, resumed in place (resume signalling, no reset), the schedules "
           "running again. A USB 2.0 disk stays mounted, as the OHCIs' do", m1);
    else
        Lg("    uwake20 VERDICT: the EHCI kept its routing (CONFIGFLAG 1) and runs again; no enabled device was on a port "
           "it owned at the demand");
}

#ifdef UWAKE_MODE_A
/* The sleep demand's reads, ONE copy for the power handler and the QEMU self-test (uwake19: the self-test's simulated
   cycle had its own OHCI-only loops, so its first run never exercised the EHCI reads the hardware will run). Every NEC
   function's PCI config; the EHCI's registers while it still decodes. Reads only. */
static void DemandReads(void)
{
    short i;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; UsbFn *f = &gFn[n->fi];
        n->dCmd = CfgRd16(f, 0x04); n->dBar0 = CfgRd32(f, 0x10);
        n->dPMCSR = n->pmOff ? CfgRd16(f, (unsigned short)(n->pmOff + 4)) : 0xFFFFUL;
        if (n->ehci) { if (n->ehBase && CfgDecodes(n)) ReadEhci(n, &n->ehDem); else memset(&n->ehDem, 0, sizeof n->ehDem); }
    }
}
static void EhWr(NecFn *n, unsigned long a, unsigned long v)   /* uwake20: an EHCI register (little-endian, as the OHCI's) */
{
    *(volatile unsigned long *)a = Swap32(v);
    __asm__ volatile ("eieio" ::: "memory");
    n->mmioWrites++;
}
/* uwake20: read an EHCI register until (v & mask) == want or limitMs pass; the last value read, *ms the time taken */
static unsigned long EhWait(unsigned long a, unsigned long mask, unsigned long want, UInt32 limitMs, UInt32 *ms)
{
    AbsoluteTime t0 = UpTime(); unsigned long v;
    for (;;) {
        v = OhciRd(a, 0);
        if ((v & mask) == want || MsSince(t0) >= limitMs) break;
    }
    *ms = MsSince(t0);
    return v;
}

/* uwake20 (mode A): carry a RUNNING EHCI through the deep sleep: EHCI 1.0's bus suspend, in the order Linux's
   ehci_bus_suspend and ehci_suspend use (drivers/usb/host/ehci-hub.c, ehci-hcd.c): interrupts masked, both schedules
   stopped, every enabled port the EHCI owns SUSPENDED (its device keeps its address and configuration, as the OHCIs'
   devices do under Apple's level 1), a microframe's wait, the controller halted; then PCI D3hot with PME off
   (sleepehci1's state, 2026-11-08: CONFIGFLAG read 1 after that wake).
   Why the suspend: Path A's wakes (wake1b, 2026-11-09) found the flash's port still connected and enabled (PORTSC 0x1005,
   no change bit), yet the flash had RESET, because they ran the controller again on a port that was never suspended: an
   idle bus puts the device to sleep by itself, and high-speed traffic with no resume signalling first reads to it as a
   reset. Here the wake (EhciWake) resumes the port with resume signalling, as the specification asks, before anything is
   sent to it. Our USB 2.0 driver is not told: interrupts masked and the controller halted, its 8 ms timer finds a halted
   controller and no work, and the wake puts everything back before that timer can run again.
   At our 250 at the demand (after DemandReads, which read it while it decoded; before ArmNec). MMIO and config only;
   each wait at most 20 ms. Only an EHCI found RUNNING (Run/Stop 1) is touched: a stopped one is left as uwake19 left it. */
static void EhciDemand(NecFn *n)
{
    UsbFn *f = &gFn[n->fi]; unsigned long op, base; short i; unsigned short pm;
    if (!n->ehci || n->ehSusp) return;           /* (ehSusp: a second demand message for the same sleep) */
    n->ehWhy = 2; n->ehSuspMask = 0; n->ehPm0 = n->ehPm1 = 0xFFFFUL;
    n->ehCmd = n->ehIntr = n->ehFrIdx = n->ehSeg = n->ehPer = n->ehAsync = 0xFFFFFFFFUL;
    n->ehStsQ = n->ehStsH = n->ehAsyncH = 0xFFFFFFFFUL; n->ehQMs = n->ehHMs = 0;
    for (i = 0; i < kMaxEhPorts; i++) n->ehPortS[i] = 0xFFFFFFFFUL;
    if (n->ehBase == 0 || !n->ehDem.ok) return;  /* not readable at the demand (DemandReads) */
    if (!(n->ehDem.cmd & kEhCmdRS)) { n->ehWhy = 1; return; }
    op = n->ehBase + (n->ehDem.cap & 0xFFUL);
    n->ehCmd = OhciRd(op, kEhUsbCmd); n->ehIntr = OhciRd(op, kEhUsbIntr); n->ehFrIdx = OhciRd(op, kEhFrIndex);
    n->ehSeg = OhciRd(op, kEhCtrlDs); n->ehPer = OhciRd(op, kEhPerBase); n->ehAsync = OhciRd(op, kEhAsync);
    base = n->ehCmd & kEhCmdKeep;
    EhWr(n, op + kEhUsbIntr, 0);                                          /* 1. no interrupt from what follows */
    EhWr(n, op + kEhUsbCmd, base & ~(kEhCmdASE | kEhCmdPSE));              /* 2. both schedules off */
    n->ehStsQ = EhWait(op + kEhUsbSts, kEhStsASS | kEhStsPSS, 0, 20, &n->ehQMs);
    for (i = 0; i < n->ehDem.n; i++) {                                     /* 3. suspend each enabled port it owns */
        unsigned long a = op + kEhPortSc + 4UL * (unsigned long)i, p = OhciRd(a, 0), w;
        if (p & kEhPsOwner) continue;                                      /* the companion's (writes ignored there) */
        w = p & ~(kEhPsRW1C | kEhPsWake);                                  /* no change bit cleared, no wake source */
        if ((p & (kEhPsPE | kEhPsSusp | kEhPsPR)) == kEhPsPE) { w |= kEhPsSusp; n->ehSuspMask |= (UInt8)(1U << i); }
        if (w != (p & ~kEhPsRW1C)) EhWr(n, a, w);
    }
    DelayMs(1);                                                            /* Linux: >= 1 microframe before the halt */
    for (i = 0; i < n->ehDem.n; i++) n->ehPortS[i] = OhciRd(op, kEhPortSc + 4UL * (unsigned long)i);
    EhWr(n, op + kEhUsbCmd, base & ~(kEhCmdASE | kEhCmdPSE | kEhCmdRS));   /* 4. halt */
    n->ehStsH = EhWait(op + kEhUsbSts, kEhStsHalt, kEhStsHalt, 20, &n->ehHMs);
    n->ehAsyncH = OhciRd(op, kEhAsync);                                    /* where the wake restarts the async ring */
    EhWr(n, op + kEhUsbIntr, 0);                                          /* (in case the driver's path unmasked it) */
    n->ehSusp = true;
    n->ehWhy = (n->ehStsH & kEhStsHalt) ? 0 : 3;
    if (n->pmOff) {                                                        /* 5. D3hot, PME off; no MMIO after this */
        pm = (unsigned short)(n->pmOff + 4);
        n->ehPm0 = CfgRd16(f, pm);
        CfgWr16(n, pm, 0x8003UL);                /* PME_Status (W1C) | D3hot. PME_Enable 0: the EHCI never wakes the Mac */
        n->ehPm1 = CfgRd16(f, pm);
    }
}

/* uwake20: the wake, in our 250 after WakeDisarm (D0) and RestoreConfig (the command register). uwake19's reads, then,
   for an EHCI that EhciDemand stopped, Linux's ehci_resume and ehci_bus_resume: CONFIGFLAG still 1 means the auxiliary
   power was kept (Linux's own test: "If CF is still set ... we maintained suspend power"), so the ports kept their
   routing and their state. The registers of the core power well (EHCI 1.0 2.3: all but CONFIGFLAG and PORTSC) are
   written back whether or not they survived, while it is halted, and read back before Run (wake1d: a posted write that
   had not landed when Run was set left the controller on a stale async ring). Then Run with the schedules off; 8 ms
   (Linux, Bugzilla 8190: port status settles after Run); resume signalling on each port we suspended that still holds
   its device, suspended (Force Port Resume for 40 ms: USB 2.0 7.1.7.7 asks for at least 20, Linux uses 40), then ended;
   10 ms of resume recovery (TRSMRCY) before any transfer; the schedules back as the driver had them. The interrupt mask
   goes back at the enable (350). A port whose device did not stay suspended (it lost power, or left) is not touched:
   the driver sees what it sees. CONFIGFLAG 0 (the routing was lost): left halted, every port is then the OHCIs'.
   Interrupts are off in the wake's device loop, so our driver's timer and interrupt cannot run in between. */
static void EhciWake(NecFn *n)
{
    unsigned long op, v, base; short i; UInt8 res = 0, gone = 0;
    if (!n->ehci) return;
    if (n->ehBase && CfgDecodes(n)) ReadEhci(n, &n->ehWake); else memset(&n->ehWake, 0, sizeof n->ehWake);
    n->ewRun = false; n->eeIntr = 0xFFFFFFFFUL; n->ewResMask = n->ewGoneMask = 0;
    n->ewIntr = n->ewFrIdx = n->ewPer = n->ewAsync = n->ewPerB = n->ewAsyncB = n->ewStsRs = 0xFFFFFFFFUL;
    n->ewCmd = n->ewSts = 0xFFFFFFFFUL; n->ewRsMs = n->ewFprMs = n->ewSchMs = 0;
    for (i = 0; i < kMaxEhPorts; i++) n->ewPortF[i] = 0xFFFFFFFFUL;
    n->ewPm = n->wkPm;                                                     /* the PMCSR WakeDisarm found */
    if (!n->ehSusp) { n->ewWhy = 1; return; }
    n->ehSusp = false;                                                     /* one resume per stop */
    if (!n->ehWake.ok) { n->ewWhy = 2; return; }
    op = n->ehBase + (n->ehWake.cap & 0xFFUL);
    n->ewIntr = OhciRd(op, kEhUsbIntr); n->ewFrIdx = OhciRd(op, kEhFrIndex);
    n->ewPer = OhciRd(op, kEhPerBase); n->ewAsync = OhciRd(op, kEhAsync);
    for (i = 0; i < n->ehWake.n; i++) {
        if (!(n->ehSuspMask & (1U << i))) continue;
        v = n->ehWake.port[i];
        if ((v & (kEhPsCCS | kEhPsPE | kEhPsSusp | kEhPsOwner | kEhPsCSC | kEhPsPEC)) == (kEhPsCCS | kEhPsPE | kEhPsSusp))
            res |= (UInt8)(1U << i);
        else gone |= (UInt8)(1U << i);
    }
    n->ewGoneMask = gone;
    if (!(n->ehWake.cfg & 1UL)) { n->ewWhy = 3; return; }
    base = n->ehCmd & kEhCmdKeep;
    EhWr(n, op + kEhCtrlDs, 0);                  /* 32-bit, as our driver writes it (reads 0, ignores writes, if unused) */
    EhWr(n, op + kEhPerBase, n->ehPer);
    EhWr(n, op + kEhAsync, n->ehAsyncH);
    n->ewPerB = OhciRd(op, kEhPerBase); n->ewAsyncB = OhciRd(op, kEhAsync);
    if ((n->ewPerB & ~0xFFFUL) != (n->ehPer & ~0xFFFUL) || (n->ewAsyncB & ~0x1FUL) != (n->ehAsyncH & ~0x1FUL)) {
        n->ewWhy = 4; return;
    }
    EhWr(n, op + kEhUsbCmd, (base & ~(kEhCmdASE | kEhCmdPSE)) | kEhCmdRS);   /* Run, the schedules still off */
    n->ewStsRs = EhWait(op + kEhUsbSts, kEhStsHalt, 0, 20, &n->ewRsMs);
    if (n->ewStsRs & kEhStsHalt) { n->ewWhy = 5; return; }
    n->ewRun = true;
    DelayMs(8);
    if (res) {
        AbsoluteTime t0; Boolean busy;
        for (i = 0; i < n->ehWake.n; i++) if (res & (1U << i)) {
            unsigned long a = op + kEhPortSc + 4UL * (unsigned long)i;
            EhWr(n, a, (OhciRd(a, 0) & ~(kEhPsRW1C | kEhPsWake)) | kEhPsFPR);
        }
        DelayMs(40);
        for (i = 0; i < n->ehWake.n; i++) if (res & (1U << i)) {
            unsigned long a = op + kEhPortSc + 4UL * (unsigned long)i;
            EhWr(n, a, OhciRd(a, 0) & ~(kEhPsRW1C | kEhPsWake | kEhPsFPR | kEhPsSusp));
        }
        t0 = UpTime();                           /* the controller ends the resume (an EOP), then clears Suspend */
        do {
            busy = false;
            for (i = 0; i < n->ehWake.n; i++)
                if ((res & (1U << i)) && (OhciRd(op, kEhPortSc + 4UL * (unsigned long)i) & (kEhPsFPR | kEhPsSusp))) busy = true;
        } while (busy && MsSince(t0) < 20);
        n->ewFprMs = MsSince(t0);
        DelayMs(10);                             /* TRSMRCY: the device's recovery before its first transfer */
    }
    n->ewResMask = res;
    for (i = 0; i < n->ehWake.n; i++) n->ewPortF[i] = OhciRd(op, kEhPortSc + 4UL * (unsigned long)i);
    EhWr(n, op + kEhUsbCmd, base);               /* the schedules back, as the driver had them */
    n->ewSts = EhWait(op + kEhUsbSts, kEhStsASS | kEhStsPSS,
                      ((base & kEhCmdASE) ? kEhStsASS : 0UL) | ((base & kEhCmdPSE) ? kEhStsPSS : 0UL), 20, &n->ewSchMs);
    n->ewCmd = OhciRd(op, kEhUsbCmd);
    n->ewWhy = 0;
}

/* uwake20: the enable (350: Apple's level 0 has run): our driver's interrupt mask from the demand, put back last, as
   the OHCIs' are (EnableA). Only where Run took at the wake. */
static void EhciEnable(void)
{
    short i;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; unsigned long op;
        if (!n->ehci || !n->ewRun) continue;
        n->ewRun = false;
        if (n->ehBase == 0 || !n->ehWake.ok || !CfgDecodes(n)) continue;
        op = n->ehBase + (n->ehWake.cap & 0xFFUL);
        EhWr(n, op + kEhUsbIntr, n->ehIntr);
        n->eeIntr = OhciRd(op, kEhUsbIntr);
    }
}
#endif

static void ReadOhci(const NecFn *n, OhciRegs *r)
{
    unsigned long b = n->base; short p;
    r->rev = OhciRd(b, kHcRevision);   r->ctl = OhciRd(b, kHcControl);
    r->cmdSt = OhciRd(b, kHcCommandStatus); r->intSt = OhciRd(b, kHcInterruptStatus);
    r->intEn = OhciRd(b, kHcInterruptEnable); r->hcca = OhciRd(b, kHcHCCA);
    r->ctlHead = OhciRd(b, kHcControlHeadED); r->bulkHead = OhciRd(b, kHcBulkHeadED);
    r->fmInterval = OhciRd(b, kHcFmInterval); r->fmNumber = OhciRd(b, kHcFmNumber);
    r->perStart = OhciRd(b, kHcPeriodicStart); r->lsThresh = OhciRd(b, kHcLSThreshold);
    r->rhA = OhciRd(b, kHcRhDescriptorA); r->rhB = OhciRd(b, kHcRhDescriptorB);
    r->rhSt = OhciRd(b, kHcRhStatus);
    r->ndp = (short)(r->rhA & 0xFFUL); if (r->ndp > kMaxPorts) r->ndp = kMaxPorts;
    for (p = 0; p < r->ndp; p++) r->port[p] = OhciRd(b, kHcRhPortStatus1 + 4UL * (unsigned long)p);
    r->ok = OhciSane(r);
}

#ifdef UWAKE_MODE_A
/* HCCA+0x84 = HccaDoneHead, the done batch the HC handed over and the ISR has not consumed yet (Apple's ISR, its
   slot 23 and its level 1 each zero it after reading). RAM, little-endian; only with VM off (physical == logical). */
static UInt32 PhysToLog(UInt32 x, UInt32 len);
static unsigned long HccaDone(unsigned long hcca)
{
    UInt32 l;
    if (!gRamReadable || hcca < 0x1000UL || hcca >= gLRam || (hcca & 0xFFUL) != 0) return 0xFFFFFFFFUL;
    if ((l = PhysToLog(hcca + 0x84UL, 4)) == 0) return 0xFFFFFFFFUL;   /* uwake10: the HCCA is a PHYSICAL address: read
                                                                       through a translation, as everything since uwake6 */
    return Swap32(*(volatile unsigned long *)l);
}
static unsigned long NecHcca(const NecFn *n) { return n->req.ok ? n->req.hcca : n->launch.ok ? n->launch.hcca : 0; }
#endif

/* ---- the wake: restore PCI config (NECQuiet v3/v7, verbatim but for the command register in mode B) ---- */
#ifdef UWAKE_MODE_B
#define kCmdKeep 0xFFFBUL      /* bus master OFF: Apple's UIMInitialize writes PCI command 6 once it has programmed the HC */
#else
#define kCmdKeep 0xFFFFUL
#endif
static void RestoreConfig(NecFn *n)
{
    UsbFn               *f = &gFn[n->fi];
    const unsigned long *g = f->pre;
    short                bar;

    n->outcome = kOutNone; n->cfgWrites = 0;
    n->wId = CfgRd32(f, 0x00); n->wCmd = CfgRd16(f, 0x04); n->wCL = CfgRd32(f, 0x0C);
    n->wBar0 = CfgRd32(f, 0x10);
    n->wPMCSR = n->pmOff ? CfgRd16(f, (unsigned short)(n->pmOff + 4)) : 0xFFFFUL;
    n->wE0 = CfgRd32(f, 0xE0); n->wE4 = CfgRd32(f, 0xE4);
    n->aCmd = n->wCmd; n->aBar0 = n->wBar0; n->aPMCSR = n->wPMCSR; n->aE4 = n->wE4;

    if (n->wId != g[0]) { n->outcome = kOutAbsent; return; }
    if (n->wBar0 == g[4] && (n->wCmd & kCmdKeep & 0x0006UL) == (CfgW(g, 0x04) & kCmdKeep & 0x0006UL)) {
        n->outcome = kOutIntact; return;          /* (mode B: Apple's teardown leaves bus master off: still intact) */
    }
    if (n->pmOff && (n->wPMCSR & 3UL) != 0) { CfgWr16(n, (unsigned short)(n->pmOff + 4), 0x0000UL); DelayMs(10); }
    if (n->wE0 != g[0xE0 >> 2]) CfgWr32(n, 0xE0, g[0xE0 >> 2]);
    if (n->wE4 != g[0xE4 >> 2]) CfgWr32(n, 0xE4, g[0xE4 >> 2]);
    if ((n->wCL & 0xFFFFUL) != (g[0x0C >> 2] & 0xFFFFUL)) {
        CfgWr8(n, 0x0C, CfgB(g, 0x0C)); CfgWr8(n, 0x0D, CfgB(g, 0x0D));
    }
    for (bar = 0; bar < 6; bar++)
        if (g[4 + bar] != 0) CfgWr32(n, (unsigned short)(0x10 + 4 * bar), g[4 + bar]);
    if (CfgB(g, 0x3C) != 0) CfgWr8(n, 0x3C, CfgB(g, 0x3C));
    CfgWr16(n, 0x04, CfgW(g, 0x04) & kCmdKeep);

    n->aCmd = CfgRd16(f, 0x04); n->aBar0 = CfgRd32(f, 0x10);
    n->aPMCSR = n->pmOff ? CfgRd16(f, (unsigned short)(n->pmOff + 4)) : 0xFFFFUL;
    n->aE4 = CfgRd32(f, 0xE4);
    n->outcome = (n->aBar0 == g[4] && (n->aCmd & 0x0006UL) == (CfgW(g, 0x04) & kCmdKeep & 0x0006UL) &&
                  (n->aPMCSR == 0xFFFFUL || (n->aPMCSR & 3UL) == 0)) ? kOutRestored : kOutVerifyFail;
}

/* ============================================================== the recorder (secondary interrupt level) */
typedef struct { UInt32 ms; UInt8 fn; UInt8 kind; UInt16 aux; UInt32 val; } RecEv;
enum { kEvCtl = 1, kEvIst, kEvIen, kEvDone, kEvPort, kEvFmStop, kEvFmRun, kEvFsqSlow, kEvFsqStuck, kEvFsqDone,
       kEvTicksStop, kEvTicksRun, kEvEnable, kEvRestore, kEvPhase, kEvEnd, kEvSleepStop, kEvMouse, kEvKeys,
       kEvReload, kEvUsl, kEvUslRef, kEvEd, kEvTaskFirst, kEvTaskGap,
       kEvTd, kEvTdW0, kEvTdCbp, kEvTdBe, kEvTdBad, kEvReg, kEvEdRaw, kEvEdW, kEvClock, kEvTdUnread, kEvEdUnread,
       kEvMapStale, kEvMapRemap,
       kEvA0Td, kEvA0TdDone, kEvA0Buf, kEvA0End, kEvDead, kEvDeadGone, kEvRcv,         /* uwake8 */
       kEvRecStall };                                                                  /* uwake9 */
#define kRecMax   8000
#ifdef UWAKE_QTEST
#define kRecEndMs 180000UL                /* QEMU: the scripts read the report ~3 min after the simulated wake */
#else
#define kRecEndMs 360000UL                /* hardware: 6 min, so a replug a minute or two after the wake is recorded too */
#endif
#define kFastMs   30000UL                 /* 8 ms sampling again for this long after a port change past 60 s */
#define kTdLog    1200                    /* retired-TD events per wake (4 per TD) */
#define kRegLog   1200                    /* list-register changes per wake */
#define kEdRawLog 240                     /* control-ED changes whose raw words are kept (uwake8: the re-add too) */
#define kA0Log    1600                    /* uwake8: address-0 transfer events per wake (TDs, buffers) */
#define kInputLog 40
#define kUslLog   400                     /* USL add-lock changes per wake (a retry loop changes it every ~1-2 s) */
#define kEdLog    600                     /* control-ED changes per wake */
static RecEv            gRec[kRecMax];
static volatile UInt32  gRecN, gRecLost;
static volatile Boolean gRecOn, gRecDone;
static volatile UInt32  gRecGen, gRecPasses, gRecArmErr;
static TimerID          gRecTimer;
/* uwake9: why did uwake8B's recorder stop at +15.16 s (its timer never came back; task level ran on)? Each pass marks
   where it is (gRecStep: 1 begun, 2/3 around the USL reads, 0x1k/0x2k/0x3k fn k's sampling, 0x4k fn k's address-0
   capture, 4 finished, 5 timer re-armed) and when it began; the Time Manager heartbeat counts its own runs. A
   task-level watchdog (NmPass) that sees no pass for 500 ms logs those and starts a fresh timer chain. */
static volatile UInt32  gRecStep, gRecPassMs, gHbN, gHbAtPass;
static UInt32           gWdPasses, gWdMs, gRearmN;      /* TASK LEVEL: passes last seen, when; restarts this wake */
#ifdef UWAKE_QTEST_RCV
static Boolean          gQtDropped;                     /* QEMU only: the recorder's timer chain dropped once on purpose */
#endif
static AbsoluteTime     gRecT0;
static volatile UInt32  gFsqHead, gFsqSince, gFsqCalls, gFsqSlowN, gTicksLast, gTicksSame;
static volatile Boolean gFsqSlow, gFsqStuck, gTicksStuck;
static volatile UInt32  gMouseLast, gMouseN, gMouse1Ms, gKeyN, gKey1Ms;
static UInt32           gKeyLast[4];
static volatile UInt32  gWakeN, gWakeLogged, gEnableMs;
static UInt32           gBootTicks;               /* Ticks at install */
static Boolean          gInstalled;               /* the handlers are in (TASK LEVEL readers only) */
static volatile UInt8   gCell3;                   /* cell 3's shade, painted by the recorder (never from a power handler or
                                                     the sleep queue: the display may not be powered yet there) */
static volatile Boolean gTaskSeen;                /* the task-level pass has run since this wake's first step */
static volatile UInt32  gTaskLast;                /* when it last ran (ms after that step) */
static volatile UInt32  gFastUntil, gRecPer;      /* 8 ms sampling until this ms (a port change past 60 s); the period */
static volatile UInt32  gClockLast, gClockN;      /* the wall clock's first two ticks after the wake */
static volatile UInt32  gTdN, gRegN, gEdRawN;     /* events of the uwake5 kinds this wake (each capped) */
static UInt32           gSnapLast[2][6], gSnapMs[2];

static UInt32 RecMs(void) { return MsSince(gRecT0); }
/* Any level. The slot is RESERVED atomically: the recorder (secondary interrupt level) and the task-level pass both
   record, and a task-level writer interrupted between reading and bumping the count would share a slot with the
   recorder. gRecN therefore counts reservations and can pass kRecMax; readers clamp it. */
static void Ev(UInt8 fn, UInt8 kind, UInt16 aux, UInt32 val)
{
    UInt32 n = (UInt32)IncrementAtomic((SInt32 *)&gRecN);
    if (n >= kRecMax) { gRecLost++; return; }
    gRec[n].ms = RecMs(); gRec[n].fn = fn; gRec[n].kind = kind; gRec[n].aux = aux; gRec[n].val = val;
}

/* ---- Apple's USBServicesLib: the ONE device-add lock (reference_os9_usl_global_enum_mutex). Offsets are the
   analysed build's (md5 3f2e62b3 = the stock FW800 ROM's nlib -20776, checked); at install they are trusted only
   after 17 signature words and the FNV-1a of the whole 52,112-byte code match, the data section is in RAM, and at
   least one ACTIVE bus holds a version-6 dispatch table (the live instance, not a private copy). Reads only. ---- */
#define kUslCodeLen   52112UL
#define kUslCodeFNV   0x4E1D94F5UL
#define kUslOffAddBus 0x5164UL
#define kUslTOC       0x01ECUL
#define kUslDataLen   77540UL
#define U_LOCK        0x0528UL            /* the add-lock: CompareAndSwap(0,1) in the drainer */
#define U_FORCE       (0x0530UL + 24UL)   /* the add timeout's force flag */
#define U_QPTR        0x361CUL            /* -> the pending-add QHdr (qHead at +2) */
#define U_TOKEN       0x3620UL            /* in-flight add token (the first timeout zeroes it) */
#define U_ADDR        0x3624UL            /* the address its SET_ADDRESS assigns */
#define U_REF         0x3628UL            /* its usbReference */
#define U_START       0x3630UL            /* its start, USB frame number (0 = no add in flight) */
#define U_DEVREC      0xDE38UL            /* device records, 124 bytes each, index = USB address */
#define U_DEVRECLEN   124UL
#define U_BUS1        0x12CA0UL           /* ACTIVE dispatch tables, 16 */
static const struct { UInt32 off, word; } kUslSig[17] = {
    { 0x5164, 0x7C0802A6UL }, { 0x5168, 0xBEC1FFD8UL }, { 0x5174, 0x80E2FF38UL }, { 0x0330, 0x3BC2033CUL },
    { 0x0348, 0x4800C6E5UL }, { 0x0358, 0x39823430UL }, { 0x0544, 0x3942033CUL }, { 0x0564, 0x93CA0000UL },
    { 0x0880, 0x38C2033CUL }, { 0x0894, 0x93C60000UL }, { 0x0A9C, 0x80A2FED4UL }, { 0x0AA8, 0x4D820020UL },
    { 0x0AD4, 0x83E2FEE0UL }, { 0x0B20, 0x801F0018UL }, { 0x0B64, 0x80A2FEB0UL }, { 0x0B84, 0x93DF0018UL },
    { 0x57FC, 0x83E2FF58UL } };
static unsigned long gUslD;              /* the USL's data base, 0 = not verified (no USL readings) */
static UInt32 gUslLock = 0xFFFFFFFFUL, gUslTok, gUslAddr, gUslRef, gUslStart, gUslForce, gUslQ, gUslN;
static UInt8  gUslAddrSeen[32];          /* every address an add was in flight for this wake (for the report) */
static short  gUslAddrN;

static Boolean InRAM(unsigned long a, unsigned long len) { return a >= 0x3000UL && a + len >= a && a + len <= gLRam; }

/* ---- uwake6: how the recorder reads memory whose address came from the HARDWARE (an ED, a TD, their links: PHYSICAL
   addresses). Until uwake5 it read a physical address as if it were the logical one. uwake5B froze the Mac at the wake
   at the moment the flash's first transfer finished (its new TD reader's first read of fn1's memory, by the Pi's
   timing); the fn1 control-list walk has read nonsense on every wake; and QEMU shows the guess wrong outright (VM on:
   logical 00050000 is physical 00054000). InRAM (Gestalt 'lram') only says a number is plausible.
   Now a physical address is read only through a TRANSLATION the system gave. The task level (ValidatePass) asks
   GetPhysical about 64 KB logical blocks near the controllers' pointers and keeps each that maps to ONE physical run;
   the recorder reads physical x at that block's logical address + (x - its physical start). It never calls the
   system: an address no translation covers is not read, and its block is queued (gCand) for the next task-level
   pass. ---- */
#define kWinBytes 0x10000UL
#define kWinMax   256
#define kCandMax  32
static volatile UInt32 gWinL[kWinMax], gWinP[kWinMax], gWinN;      /* translations: logical block -> physical start */
static volatile UInt32 gRej[kWinMax], gRejP[kWinMax], gRejN;       /* logical blocks refused, what they reported */
static volatile SInt32 gRejE[kWinMax];                             /* LockMemory's / GetPhysical's result for them */
static volatile UInt32 gCand[kCandMax], gCandW, gCandR;            /* physical blocks the recorder met: ring */
static volatile UInt32 gUnreadN;                                   /* reads refused (no translation covered them) */
static UInt32          gValLast[2][3];                             /* pointers last searched around (ValidatePass) */
static Boolean BlockIn(const volatile UInt32 *t, UInt32 n, UInt32 b)
{ UInt32 i; for (i = 0; i < n; i++) if (t[i] == b) return true; return false; }
static Boolean BlockKnown(UInt32 lb) { return BlockIn(gWinL, gWinN, lb) || BlockIn(gRej, gRejN, lb); }
/* uwake7: the WHOLE memory map, built once at task level after boot (MapMemory: GetPhysical over every logical
   megabyte below Gestalt 'lram'), as runs (logical start, physical start, length) sorted by physical start. The FW800
   with VM OFF is not logical == physical either (uwake6B, 2026-11-13: logical 00010000-003BFFFF -> physical
   00014000-003C3FFF; logical 013B0000-015BFFFF -> physical 0170D000-0191CFFF), and fn1's lists after the reload sat at
   physical 014B1880, beyond uwake6's 1 MB search; and task level first ran at +15.1 s, after the flash's add, too late
   to search then. With the map the recorder translates from the wake's first pass. uwake6's searched blocks
   (gWinL/gWinP) remain as the fallback for memory mapped after the map was taken.
   ⚠ A map taken at startup is only right while nothing remaps memory. QEMU (VM on) proved it can go stale: in snh11
   (2026-11-13) logical 00BD0000 was physical 00BD4000 at startup and 00E5F000 at the wake, and fn1's "ED" read through
   the startup map was someone else's data. So the map is CHECKED, never trusted: every logical block the recorder
   reads through it is queued (gUsed) and translated again at task level (ValidateBlock -> MapCheck, page by page). One
   page that disagrees and the map is dropped (gMapOff) and taken again (MapMemory, at most kRemapMax times); the
   report lists the pages, so a reading made through a wrong page can be discarded. The FW800 runs VM off, where the
   mapping should not move: the check is how the report proves it, rather than this comment claiming it. */
#define kRunMax 1024
static UInt32          gRunL[kRunMax], gRunP[kRunMax], gRunLen[kRunMax];
static volatile UInt32 gRunN;                                      /* published after the runs are sorted */
static volatile UInt32 gMapOff;                                    /* the map was found wrong: do not read through it */
static volatile UInt32 gUsed[kCandMax], gUsedW, gUsedR;            /* logical blocks read through the map: ring */
#define kRemapMax 2                                                /* times the map is taken again after a wrong page */
#define kDiffMax  8                                                /* blocks with a wrong page, kept for the report */
static UInt32 gMapBlocks, gMapPgSame, gMapPgDiff, gMapPgNone;      /* TASK LEVEL (MapCheck): blocks and 4 KB pages compared */
static UInt32 gMapDiffBlocks;                                      /* blocks with at least one wrong page */
static UInt32 gDiffL[kDiffMax], gDiffM[kDiffMax], gDiffF[kDiffMax], gDiffMs[kDiffMax], gDiffPg[kDiffMax];
static Boolean gDiffUsed[kDiffMax];        /* per block: its first wrong page (logical, the map's, fresh), when, how many
                                              wrong, and whether the recorder had read through it (else: searched) */
static Boolean gValUsed;                                           /* ValidateBlock is checking a block read through the map */
static UInt32 gRemapN;
static UInt32 RunFind(UInt32 x, UInt32 len)             /* binary search by physical start; 0 = not covered */
{
    UInt32 lo = 0, hi = gRunN, i;
    while (lo < hi) { i = (lo + hi) / 2; if (gRunP[i] <= x) lo = i + 1; else hi = i; }
    if (lo == 0) return 0;
    i = lo - 1;
    if (x - gRunP[i] + len <= gRunLen[i]) return gRunL[i] + (x - gRunP[i]);
    return 0;
}
/* ANY level, memory only. Queue a logical block read through the map, for the task level to check (MapCheck). Two
   writers are possible (the recorder, and QEMU's task-level self-check): a race loses or repeats an entry, and a repeat
   costs nothing (ValidateBlock skips a known block). */
static void UsedNote(UInt32 lb)
{
    UInt32 i;
    if (gUsedW - gUsedR >= kCandMax) return;
    for (i = gUsedR; i != gUsedW; i++) if (gUsed[i % kCandMax] == lb) return;
    gUsed[gUsedW % kCandMax] = lb; gUsedW++;
}
/* ANY level, memory only: the logical address where the recorder may read len bytes (EDs and TDs: 16, 16-aligned) of
   PHYSICAL address x, or 0 when no translation covers them (the block is then queued for the task level). A block
   translated after the wake (gWinL/gWinP) answers first: it is the fresher answer. Then the map, while it holds. */
static UInt32 PhysToLog(UInt32 x, UInt32 len)
{
    UInt32 i, n = gWinN, pb, l;
    if (x < 0x3000UL || len == 0 || x + len < x) return 0;
    for (i = 0; i < n; i++) { UInt32 p = gWinP[i]; if (x >= p && x + len <= p + kWinBytes) return gWinL[i] + (x - p); }
    if (!gMapOff && (l = RunFind(x, len)) != 0) { UsedNote(l & ~(kWinBytes - 1UL)); return l; }
    gUnreadN++;
    pb = x & ~(kWinBytes - 1UL);
    if (gCandW - gCandR < kCandMax) {
        for (i = gCandR; i != gCandW; i++) if (gCand[i % kCandMax] == pb) return 0;
        gCand[gCandW % kCandMax] = pb; gCandW++;
    }
    return 0;
}
static Boolean InROM(unsigned long a, unsigned long len) { return a >= 0xFFC00000UL && a + len >= a && a + len <= 0xFFFFFFF0UL; }
static UInt32 RdL(unsigned long a) { return *(volatile UInt32 *)a; }

static void UslSample(void)               /* secondary interrupt level: memory reads only */
{
    UInt32 lock, tok, addr, ref, start, force, q, qh = 0; UInt16 aux;
    if (gUslD == 0) return;
    lock = RdL(gUslD + U_LOCK); tok = RdL(gUslD + U_TOKEN); addr = RdL(gUslD + U_ADDR) & 0xFFUL;
    ref = RdL(gUslD + U_REF); start = RdL(gUslD + U_START); force = RdL(gUslD + U_FORCE);
    q = RdL(gUslD + U_QPTR);
    if (InRAM(q, 6)) qh = *(volatile UInt32 *)(q + 2);
    if (lock == gUslLock && tok == gUslTok && addr == gUslAddr && start == gUslStart && force == gUslForce &&
        (qh != 0) == (gUslQ != 0) && ref == gUslRef) return;
    /* The address being ASSIGNED is in the reference: ref = (counter << 7) + 128 + address. (The address field reads 0
       throughout an add in QEMU: the device's current, default address.) */
    if (start != 0 && ref != 0 && gUslAddrN < (short)sizeof gUslAddrSeen) {
        short i; Boolean seen = false; UInt8 target = (UInt8)(ref & 0x7FUL);
        for (i = 0; i < gUslAddrN; i++) if (gUslAddrSeen[i] == target) seen = true;
        if (!seen) gUslAddrSeen[gUslAddrN++] = target;
    }
    aux = (UInt16)((lock ? 1 : 0) | (qh ? 2 : 0) | (force ? 4 : 0) | (tok ? 8 : 0) | ((addr & 0xFFUL) << 8));
    if (++gUslN <= kUslLog) {
        Ev(0xFF, kEvUsl, aux, start);
        if (ref != gUslRef) Ev(0xFF, kEvUslRef, (UInt16)(ref & 0x7FUL), ref);
    }
    gUslLock = lock; gUslTok = tok; gUslAddr = addr; gUslRef = ref; gUslStart = start; gUslForce = force; gUslQ = qh;
}

static Boolean UslLocate(void)            /* TASK LEVEL (install) */
{
    CFragConnectionID conn = 0; Ptr mainAddr = NULL, sa = NULL; Str255 errName; CFragSymbolClass cls = 0; OSErr e;
    unsigned long tv0, tv1, code, data, h = 0x811C9DC5UL, i; short ok = 0, act = 0, k;
    gUslD = 0;
    e = GetSharedLibrary("\pUSBServicesLib", kPowerPCCFragArch, kReferenceCFrag, &conn, &mainAddr, errName);
    if (e == noErr) e = FindSymbol(conn, "\pUSBAddBus", &sa, &cls);
    if (e != noErr || sa == NULL || !(InRAM((unsigned long)sa, 8) || InROM((unsigned long)sa, 8))) {
        Lg("  USL: USBAddBus not found (%d): no add-lock readings", (int)e); return false;
    }
    tv0 = RdL((unsigned long)sa); tv1 = RdL((unsigned long)sa + 4);
    code = tv0 - kUslOffAddBus; data = tv1 - kUslTOC;
    if (!(InROM(code, kUslCodeLen) || InRAM(code, kUslCodeLen)) || !InRAM(data, kUslDataLen)) {
        Lg("  USL: code %08lX / data %08lX not where expected: no add-lock readings", code, data); return false;
    }
    for (k = 0; k < 17; k++) if (RdL(code + kUslSig[k].off) == kUslSig[k].word) ok++;
    for (i = 0; i < kUslCodeLen; i++) { h ^= *(volatile UInt8 *)(code + i); h *= 0x01000193UL; }
    for (k = 0; k < 16; k++) { unsigned long t = RdL(data + U_BUS1 + 4UL * (unsigned long)k);
                               if (t && InRAM(t, 4) && RdL(t) == 6UL) act++; }
    Lg("  USL: code %08lX, data %08lX; signatures %d/17, code FNV-1a %08lX (want %08lX); live buses %d",
       code, data, ok, h, kUslCodeFNV, act);
    if (ok != 17 || h != kUslCodeFNV || act == 0) { Lg("  USL: not the analysed build (or not live): no add-lock readings"); return false; }
    gUslD = data;
    Lg("  USL: add-lock readings ON (lock %lu, add in flight: start %lu)", RdL(data + U_LOCK), RdL(data + U_START));
    return true;
}
static void StopRecorder(UInt16 why)              /* before a sleep powers the NEC down (memory only; one CPU, so no
                                                     recorder pass can be mid-flight when this runs) */
{
    if (gRecOn) { gRecOn = false; gRecDone = true; Ev(0xFF, kEvSleepStop, why, RecMs()); }
}

#ifdef UWAKE_PAINT
/* ---- the screen row (memory writes only; DEV builds only, uwake22) ---- */
static struct { Boolean ok; unsigned long base; long rowBytes; short depth; short w, h; } gScr;
static void ScrInit(void)                /* TASK LEVEL */
{
    GDHandle gd; PixMapHandle pm;
    gScr.ok = false;
    gd = GetMainDevice();
    if (gd == NULL) return;
    pm = (*gd)->gdPMap;
    if (pm == NULL || *pm == NULL) return;
    gScr.base = (unsigned long)(*pm)->baseAddr;
    gScr.rowBytes = (long)((*pm)->rowBytes & 0x3FFF);
    gScr.depth = (*pm)->pixelSize;
    gScr.w = (short)((*pm)->bounds.right - (*pm)->bounds.left);
    gScr.h = (short)((*pm)->bounds.bottom - (*pm)->bounds.top);
    gScr.ok = (gScr.base >= 0x1000UL && gScr.rowBytes > 0 && gScr.h > 40 && gScr.w >= 400 &&   /* the row ends at x 356 */
               (gScr.depth == 8 || gScr.depth == 16 || gScr.depth == 32));
}
static void ScrCell(short x, short y, short w, short h, unsigned char b)
{
    short yy, xx;
    if (!gScr.ok) return;
    for (yy = 0; yy < h; yy++) {
        unsigned long row = gScr.base + (unsigned long)(y + yy) * (unsigned long)gScr.rowBytes;
        if (gScr.depth == 32) { volatile unsigned long *p = (volatile unsigned long *)row + x;
            unsigned long v = (unsigned long)b * 0x01010101UL; for (xx = 0; xx < w; xx++) p[xx] = v; }
        else if (gScr.depth == 16) { volatile unsigned short *p = (volatile unsigned short *)row + x;
            unsigned short v = (unsigned short)(b * 0x0101); for (xx = 0; xx < w; xx++) p[xx] = v; }
        else { volatile unsigned char *p = (volatile unsigned char *)row + x; for (xx = 0; xx < w; xx++) p[xx] = b; }
    }
}
/* The row: 12x12 cells at the bottom-left, 16 px apart, in five groups 8 px apart (0-3 | 4-9 | 10-15 | 16-17 | 18-19),
 * big enough to read from a phone photo of a frozen screen (on 8-bit screens the shades invert: index 0 is white).
 *   0 recorder pass (blinks)     1 Time Manager heartbeat: black/white, grey/white once our sleep step has run
 *   2 Ticks: white moving, black stopped
 *   3 mode A: grey = the restore ran, white = interrupts back on, black = the enable WITHHELD them
 *     mode B: grey = config restored, inside Apple's reload; white = Apple's reload returned
 *   fn0 at 4..9, fn1 at 10..15:  +0 HCFS (black Reset, dark Resume, white Operational, grey Suspend)  +1 MIE (white on)
 *                                +2 WDH pending (black)  +3 RHSC pending (black)  +4 UE, or another enabled source,
 *                                pending (black)  +5 frame counter: blinks while it moves, black when stopped
 *  16 File Manager: white idle, grey busy, black one call held 2 s+   17 recorder: white 8 ms, grey slow, black done
 *  18 the cursor has moved since the wake (white; black = not yet)   19 a key has gone down since the wake (white) */
#define kRowCells 20
static unsigned char gCellNow[kRowCells];
static Boolean gCellValid[kRowCells];
static void Cell(int i, unsigned char b)
{
    short y, grp;
    if (!gScr.ok || i < 0 || i >= kRowCells) return;
    if (gCellValid[i] && gCellNow[i] == b) return;
    gCellNow[i] = b; gCellValid[i] = true;
    grp = (short)(i < 4 ? 0 : i < 10 ? 1 : i < 16 ? 2 : i < 18 ? 3 : 4);
    y = (short)(gScr.h - 20);
    ScrCell((short)(8 + 16 * i + 8 * grp), y, 12, 12, b);
}
static void CellsRepaint(void) { int i; for (i = 0; i < kRowCells; i++) gCellValid[i] = false; }
#else
/* uwake22, the ship build: no screen row. The recorder's calls stay in the source and compile to nothing. */
static inline void Cell(int i, unsigned char b) { (void)i; (void)b; }
static inline void CellsRepaint(void) { }
#endif

/* ---- uwake14: Mac OS's own activity clocks. NativePowerMgrLib keeps four "last activity" times in its globals
   (PmgrBase, LM 0x0D18; PMGetSetActivityTicks, the same offsets in Mac OS 9.1's library and the FW800's d9e42f48):
   the SYSTEM clock (+0x1A), which the system-sleep timer runs on; KEYBOARD/MOUSE (+0x188), which the display timer runs
   on; NETWORK (+0x18C); DISK (+0x1E). Every NMInstall moves the system clock (usb-wake/NPM-IDLE-TIMER.md): one report
   gives it 10 s back, at most one per 10 s, so a pass re-armed every event-loop turn (uwake1 to uwake13) kept the Mac
   from ever taking an idle sleep. The heartbeat watches the four with plain memory reads (no calls: 68K interrupt
   level), once ClkInstall has checked the offsets against NativePowerMgrLib's own GetLastActivity. The record is one
   4 KB-aligned block that starts with a magic, so a QEMU memory dump can find it. ---- */
enum { kClkSys = 0, kClkUser, kClkNet, kClkDisk, kClkN };
static const UInt16 kClkOff[kClkN] = { 0x1A, 0x188, 0x18C, 0x1E };
#define kClkRingN 64
typedef struct { UInt32 tick, sysBefore, sysAfter; UInt8 moved, quiet, who, act; } ClkEv;   /* 16 bytes; moved: bit 0
                       system, 1 keyboard/mouse, 2 network, 3 disk; who (uwake15): 1 + the poster's slot in the
                       Notification Manager table whose request moved the system clock, 0 = no request did; act
                       (uwake16): 1 + the route (gActSig) of the activity report that moved it, 0 = the hook saw none */
typedef struct {
    UInt32 magic[2];                  /* 'UW14' 'CLK1' */
    UInt32 G;                         /* PmgrBase, set LAST by ClkInstall once the offsets check out (0 = not sampling) */
    UInt32 samples, events;           /* heartbeat samples; events recorded (the ring keeps the last kClkRingN) */
    UInt32 last[kClkN];               /* the four clocks at the last sample */
    UInt32 quiet;                     /* 1 while the task-level pass is stopped */
    UInt32 winSince;                  /* Ticks: the window's counters count from here (the last stop) */
    UInt32 winSysBg, winBgGen, winBgNet, winBgDisk;   /* background reports: the system clock moved and keyboard/mouse
                                                         did not; of them, with no other clock / with network / disk */
    UInt32 winUser, winNet, winDisk;  /* moves of the keyboard/mouse, network and disk clocks */
    UInt32 winFirstBg, winLastBg, winMinGap, winMaxGap, winLastUser;   /* Ticks; gaps between background reports */
    ClkEv  ev[kClkRingN];             /* every move except keyboard/mouse activity (which would flood it) */
} ClkRec;
static ClkRec * volatile gClk;        /* NULL until ClkInstall */

/* ---- uwake15: WHO posts. uwake14A on the FW800 (hardware run, 2026-11-14) went quiet itself, and still the system
   clock moved 52 times in 14 minutes, alone, 11-12 s apart: another poster. A trap patch on _NMInstall ($A05E)
   records every Notification Manager request on the Mac by its owner (response routine + refCon), counts and times
   them, and reads the system clock before and after chaining to the original: a request whose own _IdleUpdate moved
   it is named as the one that did. The patch runs at WHATEVER level the caller is (NMInstall may be called at
   interrupt time: our AirPort driver's timer, the Bluetooth stack's USB completions), so it does memory writes and the
   chained call only; the names are read at task level when the table is logged. */
#define kSpyN 16
typedef struct {
    UInt32 resp, refCon, rec;         /* nmResp, nmRefCon, the NMRec's address: the poster's identity */
    UInt8  str, snd, icon, mark;      /* nmStr / nmSound / nmIcon / nmMark set: a notice the user sees */
    UInt32 n, first, last, minGap, maxGap;   /* this window: requests, Ticks of the first and last, gaps (ticks) */
    UInt32 pushes;                    /* this window: system-clock moves this poster's request made */
    UInt32 quiet;                     /* uwake21, this window: its requests whose activity report was not passed on */
} SpyPoster;
static SpyPoster        gSpy[kSpyN];
static volatile UInt32  gSpyN;                       /* slots taken (may pass kSpyN: read as kSpyN) */
static volatile UInt32  gSpyCalls, gSpyPushes, gSpyFull;   /* this window: requests, moves they made, unrecorded */

/* ---- uwake21: a SILENT request is not activity. The FW800's idle-sleep run on the USB 2.0 setup (2026-11-17,
   hardware run): no idle sleep in 35 min with Energy Saver at 10 min, because the released USB 2.0
   driver (fw1rel ehci_vhub.c task_work_arm) posts a silent request every ~2 s as a keepalive, and Mac OS's _NMInstall
   calls _IdleUpdate for every request (docs/NPM-IDLE-TIMER.md): 164 of the 202 system-clock moves, every 10 s, the whole
   window. A request with no Application-menu mark, no icon, no sound and no string shows the user nothing: drivers use
   it as a way to task level (ours, the USB 2.0 driver's, the Bluetooth stack's). So while the ORIGINAL _NMInstall runs
   for such a request, SpyNMInstall sets gNmQuiet, and ActHook does not pass that request's type-0 report (its
   _IdleUpdate) on to PMIdleUpdate: the clocks and PMIdleUpdate's own 10-s limiter stay exactly as they were. A request
   the user can see keeps Apple's behaviour. Nesting-safe (saved and restored: an interrupt-level NMInstall inside a
   task-level one sets and puts back its own value). The cost of the window: a type-0 report from an interrupt that lands
   INSIDE a silent _NMInstall (a few microseconds) is not passed on either; type-0 reporters repeat (input reports type 1
   besides), so one lost is harmless. Memory only, any level. ---- */
static volatile UInt32  gNmQuiet;                    /* 1 while the original _NMInstall runs for a silent request */
static volatile UInt32  gSpyQuiet;                   /* this window: silent requests whose report was not passed on */
static volatile UInt32  gActQuiet, gActQuietAll;     /* reports not passed on: this window; since boot */
static volatile UInt32  gSpyPushTick, gSpyPushWho;   /* the last move a request made: Ticks, 1 + slot */
static UniversalProcPtr gSpyOrig;                    /* the _NMInstall we replaced */
enum { kSpyProcInfo = kRegisterBased | RESULT_SIZE(SIZE_CODE(sizeof(SInt16))) | REGISTER_RESULT_LOCATION(kRegisterD0)
                    | REGISTER_ROUTINE_PARAMETER(1, kRegisterD1, SIZE_CODE(sizeof(SInt16)))      /* the trap word */
                    | REGISTER_ROUTINE_PARAMETER(2, kRegisterA0, SIZE_CODE(sizeof(long))) };     /* the NMRec */
static UInt32 SpySlot(const NMRec *nm)               /* ANY LEVEL: memory only. Returns 1 + slot, 0 = table full */
{
    UInt32 resp = (UInt32)nm->nmResp, rc = (UInt32)nm->nmRefCon, i, n = gSpyN, now = kLowTicks;
    SpyPoster *p;
    if (n > kSpyN) n = kSpyN;
    for (i = 0; i < n; i++) if (gSpy[i].resp == resp && gSpy[i].refCon == rc) break;
    if (i == n) {
        i = (UInt32)IncrementAtomic((SInt32 *)&gSpyN);              /* claim a slot (its old value) */
        if (i >= kSpyN) { gSpyFull++; return 0; }
        p = &gSpy[i];
        p->rec = (UInt32)nm; p->str = (UInt8)(nm->nmStr != NULL); p->snd = (UInt8)(nm->nmSound != NULL);
        p->icon = (UInt8)(nm->nmIcon != NULL); p->mark = (UInt8)(nm->nmMark != 0);
        p->n = 0; p->pushes = 0; p->quiet = 0; p->minGap = p->maxGap = 0;
        p->refCon = rc; p->resp = resp;                             /* identity last: a search matches it only now */
    }
    p = &gSpy[i];
    if (p->n > 0) {
        UInt32 gap = now - p->last;
        if (p->n == 1 || gap < p->minGap) p->minGap = gap;
        if (gap > p->maxGap) p->maxGap = gap;
    } else p->first = now;
    p->n++; p->last = now;
    return i + 1;
}
static SInt16 SpyNMInstall(UInt16 trapWord, NMRecPtr nm)   /* THE PATCH: ANY LEVEL (the caller's). Memory + the chain */
{
    ClkRec *c = gClk; UInt32 G = c ? c->G : 0, before = 0, who = 0, quiet = 0, saveQ, q0; SInt16 r;
    if (nm != NULL && !((UInt32)nm & 1UL) && (UInt32)nm >= 0x2000UL && (UInt32)nm < gLRam && nm->qType == nmType) {
        gSpyCalls++;
        who = SpySlot(nm);
        /* uwake21: it shows the user nothing (read now, at this request, not from the poster's first one) */
        quiet = (nm->nmMark == 0 && nm->nmIcon == NULL && nm->nmSound == NULL && nm->nmStr == NULL) ? 1UL : 0UL;
    }
    if (G) before = *(volatile UInt32 *)(G + 0x1AUL);
    saveQ = gNmQuiet; q0 = gActQuietAll;
    gNmQuiet = quiet;                                           /* uwake21: ActHook reads it inside the call below */
    r = (SInt16)CallOSTrapUniversalProc(gSpyOrig, kSpyProcInfo, (long)trapWord, (long)nm);
    gNmQuiet = saveQ;
    if (gActQuietAll != q0) {                                   /* its _IdleUpdate stopped at ActHook */
        gSpyQuiet++;
        if (who) gSpy[who - 1].quiet++;
    }
    if (G && *(volatile UInt32 *)(G + 0x1AUL) != before) {      /* this request's _IdleUpdate moved the system clock */
        gSpyPushes++;
        if (who) { gSpy[who - 1].pushes++; gSpyPushTick = kLowTicks; gSpyPushWho = who; }
    }
    return r;
}
static RoutineDescriptor gSpyRD = BUILD_ROUTINE_DESCRIPTOR(kSpyProcInfo, SpyNMInstall);

/* ---- uwake16: WHO reports activity. uwake15A on the FW800 (hardware run, 2026-11-14): in the quiet window every
   system-clock move came from NO Notification Manager request (52 moves, 11-13 s apart, all "?"), from ~4 min after
   boot until the hard disk spun down (+841 s: the disk clock went to 0, as DoHDSpinDown leaves it); the idle sleep came
   600 s after the last effective push, 19 min 29 s after boot. Something reports activity directly. NativePowerMgrLib
   has ONE activity routine, PMIdleUpdate (usb-wake/NPM-IDLE-TIMER.md), and reaches it from every route (the $A285 trap
   through __PowerMgrOpDispatch, UpdateSystemActivity, PowerDispatch selector 1, DelaySystemIdle through PMIdleDelay,
   its own doze and wake paths) through ONE transition vector inside its own TOC (d9e42f48: the glue at +0x19210 loads
   r2+0x24; 13 call sites). ActInstall points that vector's code word at a trampoline that loads OUR TOC and enters
   ActHook, which chains to the original: every report passes through it. A report that moved the system clock is
   recorded by its route: the return address into NPM (which entry), the PowerPC return addresses above it (the back
   chain), and any 68K return address in the stack just after an activity trap ($A285, $A09F, $A09E) or a File Manager
   trap; the first move of each route keeps a 4 KB copy of the stack above r1, for reading offline (written at the
   pass's next stop, not inside the sleep hook). Memory only at the reporter's level (any level: NMInstall reports from
   interrupt time); named at task level. ---- */
#define kActSigN    24                /* routes remembered (cleared at each logged stop of the pass) */
#define kActChainN  24                /* PowerPC return addresses per route (Mixed Mode's switch frames add several) */
#define kAct68kN    8                 /* 68K return addresses per route, after an activity or File Manager trap */
#define kActRawN    6                 /* routes whose first move keeps a raw copy of the stack */
#define kActRawB    4096
#define kActEntN    10                /* entries into NPM, counted on every call (not only moves) */
typedef struct {
    UInt32 sig;                       /* the route's hash; published LAST (0 = the slot is not ready) */
    UInt32 lr, type, sp;              /* the return address into NPM; PMIdleUpdate's type; r1 in ActHook at the first move */
    UInt32 nCh, ch[kActChainN];       /* PowerPC return addresses from the back chain, nearest first (ch[0] = lr) */
    UInt32 n68, a68[kAct68kN];        /* 68K return addresses just after an activity or File Manager trap */
    UInt16 w68[kAct68kN], o68[kAct68kN];   /* that trap word; the slot's offset above r1 */
    UInt32 pushes, first, last, minGap, maxGap;   /* this window (Ticks) */
    UInt32 total;                     /* since boot */
    UInt32 raw;                       /* 1 + index into gActRaw, 0 = none */
    UInt32 rawShown;
} ActSig;
typedef struct { UInt32 lr; UInt32 n[6]; } ActEnt;     /* calls this window, by entry and type (5 = out of range) */
static ActSig          gActSig[kActSigN];
static ActEnt          gActEnt[kActEntN];
static UInt32          gActRaw[kActRawN][kActRawB / 4];
static volatile UInt32 gActSigN, gActEntN, gActRawN;          /* slots claimed (each may pass its bound) */
static volatile UInt32 gActCalls[6], gActPushes, gActFull;    /* this window: calls by type; moves; moves not recorded */
static volatile UInt32 gActUserMoves;                         /* this window: moves by keyboard/mouse reports (no route) */
static volatile UInt32 gActPushTick, gActPushWho;             /* the last move: Ticks, 1 + route */
static UInt32          gActOrig[2];                           /* PMIdleUpdate's own transition vector: code, TOC */
static Boolean         gActReadStack;                         /* VM off (or the QEMU check build): read above the chain */
/* The top of the RAM the hook may read. NOT Gestalt 'lram': the FW800 says 2047 MB, but its memory map (uwake7) has
   logical RAM mapped only up to 5FF6FFFF; a stack word pointing above would fault. With VM off it is where the map's
   runs stop being contiguous from 64 KB up; with VM on, 'lram' (the logical space is all backed); 0 = read no RAM. */
static UInt32          gActTop;
typedef UInt32 (*ActFn)(long);
static UInt32 ActRd(UInt32 a)         /* ANY LEVEL: a word of mapped RAM or of the ROM image; 0 outside them */
{
    if ((a & 3UL) || !((a >= 0x2000UL && a < gActTop && gActTop - a >= 4UL) || a >= 0xFFC00000UL)) return 0;
    return *(const volatile UInt32 *)a;
}
static UInt32 ActHw(UInt32 a)         /* ANY LEVEL: a halfword, the same bounds */
{
    if ((a & 1UL) || !((a >= 0x2000UL && a < gActTop && gActTop - a >= 2UL) || a >= 0xFFC00000UL)) return 0;
    return *(const volatile UInt16 *)a;
}
static void ActCount(UInt32 lr, UInt32 ty)     /* ANY LEVEL: one call, by its entry into NPM */
{
    UInt32 n = gActEntN, i;
    if (n > kActEntN) n = kActEntN;
    for (i = 0; i < n; i++) if (gActEnt[i].lr == lr) break;
    if (i == n) {
        i = (UInt32)IncrementAtomic((SInt32 *)&gActEntN);           /* claim a slot (its old value) */
        if (i >= kActEntN) return;
        __asm__ volatile ("sync" ::: "memory");
        gActEnt[i].lr = lr;
    }
    gActEnt[i].n[ty]++;
}
static void ActPush(UInt32 ty, UInt32 lr, UInt32 sp, UInt32 now)   /* ANY LEVEL: a report that moved the system clock */
{
    UInt32 ch[kActChainN], a68[kAct68kN], f, prev, nCh = 0, n68 = 0, sig, i, k, n, p, lim;
    UInt16 w68[kAct68kN], o68[kAct68kN];
    ActSig *s;
    /* the back chain (PowerOpen linkage): a frame's first word is its caller's frame, and the return address into that
       caller is saved at +8 of it. Frames climb; a jump of 64 KB or more, or a backward one, ends the walk. A back
       chain with its low bit set is a Mixed Mode switch frame (q16K2: 1F50D3D1, right below the 68K stack the
       _IdleUpdate trap came from): marked (1) and stepped over; what follows it may be 68K stack before PowerPC frames
       resume, so some entries after a mark are not return addresses (named at task level, or not). */
    prev = sp; f = ActRd(sp);
    while (nCh < kActChainN && (f & ~1UL) > prev && (f & ~1UL) - prev < 0x10000UL) {
        if (f & 1UL) { f &= ~1UL; ch[nCh++] = 1UL; if (nCh >= kActChainN) break; }
        ch[nCh++] = ActRd(f + 8UL);
        prev = f;
        f = ActRd(f);
    }
    /* 68K code above it: a return address right after an activity trap ($A285 _IdleUpdate, $A09F _PowerDispatch,
       $A09E) or a File Manager trap (_Open, _Close, _Read, _Write, _OpenRF, _GetFileInfo, _FSDispatch/_HFSDispatch;
       the async/HFS flag bits ignored), in the stack above r1. The words are read at the addresses found: VM off only. */
    if (gActReadStack) {
        lim = (gActTop > sp && gActTop - sp > (UInt32)kActRawB) ? sp + kActRawB : gActTop;
        for (p = sp; p + 4UL <= lim && n68 < kAct68kN; p += 2) {
            UInt32 v = (ActHw(p) << 16) | ActHw(p + 2UL), w, m;
            if ((v & 1UL) || !((v >= 0x3000UL && v < gActTop) || v >= 0xFFC00002UL)) continue;
            w = ActHw(v - 2UL); m = w & 0xF8FFUL;
            if (w != 0xA285UL && w != 0xA09FUL && w != 0xA09EUL && m != 0xA000UL && m != 0xA001UL && m != 0xA002UL
                && m != 0xA003UL && m != 0xA00AUL && m != 0xA00CUL && m != 0xA060UL) continue;
            a68[n68] = v; w68[n68] = (UInt16)w; o68[n68] = (UInt16)(p - sp); n68++;
        }
    }
    /* the route: the entry, the type, the chain and the NEAREST 68K return only (q16K2 split one caller in two on a
       stale $A09F word 1.4 KB up the stack) */
    /* (uwake17: the first 16 entries only: the deepest are the event loop's context, and split one caller into many) */
    sig = (lr * 2654435761UL) ^ ty;
    for (i = 0; i < nCh && i < 16; i++) sig = (sig ^ ch[i]) * 16777619UL;
    if (n68) sig = (sig ^ a68[0]) * 16777619UL;
    if (sig == 0) sig = 1;
    n = gActSigN; if (n > kActSigN) n = kActSigN;
    for (i = 0; i < n; i++) if (gActSig[i].sig == sig) break;
    if (i == n) {
        i = (UInt32)IncrementAtomic((SInt32 *)&gActSigN);
        if (i >= kActSigN) {                   /* the table is full: still timed, as through the hook but unnamed */
            gActFull++; gActPushTick = now; gActPushWho = 0xFFUL;
            return;
        }
        s = &gActSig[i];
        s->lr = lr; s->type = ty; s->sp = sp; s->nCh = nCh; s->n68 = n68;
        for (k = 0; k < nCh; k++) s->ch[k] = ch[k];
        for (k = 0; k < n68; k++) { s->a68[k] = a68[k]; s->w68[k] = w68[k]; s->o68[k] = o68[k]; }
        s->pushes = s->total = 0; s->minGap = s->maxGap = 0; s->raw = 0; s->rawShown = 0;
        /* a raw copy: at most two while our own pass runs (its routes, input, the self-test), the rest kept for the
           quiet windows, whose reporter is the one to read */
        if (gActReadStack && ((gClk != NULL && gClk->quiet) || gActRawN < 2UL)) {
            UInt32 r = (UInt32)IncrementAtomic((SInt32 *)&gActRawN);
            if (r < kActRawN) {
                for (k = 0; k < kActRawB / 4; k++) gActRaw[r][k] = ActRd(sp + 4UL * k);
                s->raw = r + 1;
            }
        }
        __asm__ volatile ("sync" ::: "memory");
        s->sig = sig;                                               /* published last: a search matches it only now */
    }
    s = &gActSig[i];
    if (s->pushes > 0) {
        UInt32 gap = now - s->last;
        if (s->pushes == 1 || gap < s->minGap) s->minGap = gap;
        if (gap > s->maxGap) s->maxGap = gap;
    } else s->first = now;
    s->pushes++; s->total++; s->last = now;
    gActPushTick = now; gActPushWho = i + 1;
}
static UInt32 ActHook(long type)      /* THE HOOK, in PMIdleUpdate's place: ANY LEVEL (the reporter's). Memory + the chain */
{
    ClkRec *c = gClk; UInt32 G = c ? c->G : 0, before = 0, r, ty, lr, sp;
    lr = (UInt32)__builtin_return_address(0);
    sp = (UInt32)__builtin_frame_address(0);
    ty = (UInt32)(SInt32)(SInt16)type;                             /* as PMIdleUpdate reads it (extsh) */
    if (ty > 4UL) ty = 5;
#ifndef UWAKE_QTEST_NOQUIET                                        /* (the QEMU control build passes everything on) */
    if (gNmQuiet && ty == 0UL) {                                   /* uwake21: a silent request's _IdleUpdate */
        gActQuiet++; gActQuietAll++;
        return 0;               /* not passed on. 0 = noErr; _NMInstall ignores _IdleUpdate's result (moveq #0,d0 after
                                   the trap, NPM-IDLE-TIMER.md's bytes) */
    }
#endif
    if (G) before = *(volatile UInt32 *)(G + 0x1AUL);
    r = ((ActFn)(void *)gActOrig)(type);                           /* the original, through its own vector (its TOC) */
    gActCalls[ty]++;
    ActCount(lr, ty);
    if (G && *(volatile UInt32 *)(G + 0x1AUL) != before) {
        gActPushes++;
        /* uwake17: keyboard and mouse reports are counted, not given routes: on the FW800 (uwake16A) the operator's
           few seconds at the keyboard took 23 of the 24 slots, one per input path, and the reporter that came after
           went unnamed */
        if (ty == 1UL) { gActUserMoves++; gActPushTick = kLowTicks; gActPushWho = 0xFEUL; }
        else ActPush(ty, lr, sp, kLowTicks);
    }
    return r;
}

static void ClkSample(void)           /* 68K INTERRUPT LEVEL (the heartbeat): memory reads and our own record ONLY */
{
    ClkRec *c = gClk; UInt32 G, v[kClkN], now; UInt8 moved = 0; int i;
    if (c == NULL || (G = c->G) == 0) return;
    now = kLowTicks;
    for (i = 0; i < kClkN; i++) v[i] = *(volatile UInt32 *)(G + kClkOff[i]);
    c->samples++;
    for (i = 0; i < kClkN; i++) if (v[i] != c->last[i]) moved |= (UInt8)(1U << i);
    if (moved == 0) return;
    if ((moved & 1U) && !(moved & 2U)) {          /* the system clock without keyboard/mouse: a background report */
        if (c->winSysBg > 0) {
            UInt32 gap = now - c->winLastBg;
            if (c->winSysBg == 1 || gap < c->winMinGap) c->winMinGap = gap;
            if (gap > c->winMaxGap) c->winMaxGap = gap;
        } else c->winFirstBg = now;
        c->winSysBg++; c->winLastBg = now;
        if (moved & 4U) c->winBgNet++; else if (moved & 8U) c->winBgDisk++; else c->winBgGen++;
    }
    if (moved & 2U) { c->winUser++; c->winLastUser = now; }
    if (moved & 4U) c->winNet++;
    if (moved & 8U) c->winDisk++;
    if (!(moved & 2U)) {
        ClkEv *e = &c->ev[c->events % kClkRingN];
        e->tick = now; e->sysBefore = c->last[kClkSys]; e->sysAfter = v[kClkSys];
        e->moved = moved; e->quiet = (UInt8)c->quiet;
        /* uwake15: a request moved it within this sample's 250 ms (moves are >= 10 s apart: it is this one) */
        e->who = (UInt8)(((moved & 1U) && now - gSpyPushTick <= 20UL) ? gSpyPushWho : 0UL);
        /* uwake16: the activity report that moved it, by its route (the hook times it to the tick) */
        e->act = (UInt8)(((moved & 1U) && now - gActPushTick <= 20UL) ? gActPushWho : 0UL);
        c->events++;
    }
    for (i = 0; i < kClkN; i++) c->last[i] = v[i];
}

/* Time Manager heartbeat (68K interrupt level), independent of the recorder's secondary-level chain. It blinks black/
   white until our sleep-time step has run once (mode A: the demand snapshot; mode B: Apple's teardown returned), then
   grey/white: a photo right after a wake says whether our code took part in the sleep at all. */
static TMTask   gHbTask; static TimerUPP gHbUPP; static Boolean gHbState;
static volatile UInt8 gHbDark;           /* 0x00, then 0x80 once our sleep-time step ran */
#ifdef UWAKE_QTEST
static void QtActPost(void);             /* (below, with the pass) */
#endif
#ifdef UWAKE_QTEST_PUMP
static void QtPump(void);                /* (below, with QtActPost) */
static void QtPumpLog(void);
#endif
static pascal void HeartbeatTask(TMTaskPtr tp)
{
    gHbN++;                                  /* uwake9: did the Time Manager keep running when the recorder stopped? */
    gHbState = !gHbState;
    Cell(1, (unsigned char)(gHbState ? gHbDark : 0xFF));
    ClkSample();                             /* uwake14: Mac OS's activity clocks (memory reads) */
#ifdef UWAKE_QTEST
    QtActPost();                             /* uwake16, QEMU only: a pass at the end of the quiet window, to log it */
#endif
#ifdef UWAKE_QTEST_PUMP
    QtPump();                                /* uwake21, QEMU only: the USB 2.0 driver's keepalive, stood in */
#endif
    PrimeTime((QElemPtr)tp, 250);
}

/* ---- uwake8: the address-0 transfers. Every add starts on its controller's address-0 ED: GET_DESCRIPTOR(8) (the first
   8 bytes of the device descriptor; byte 7 is the default pipe's maximum packet size), then SET_ADDRESS. uwake7B saw the
   flash's first read sit pending at least 146 ms and its new pipe built with a maximum packet size of 0 (FA 104, w0
   00000068): that read delivered nothing usable. Recorded now for every add on both controllers (fn0's healthy adds in
   the same wake are the comparison): each TD as queued, the SETUP's 8 request bytes, an 8-byte IN's buffer when first
   seen and again once the transfer has left the ED, and each TD's words again then (if Apple has not reused it yet).
   Memory reads through translations only (PhysToLog); any level. ---- */
#define kA0TdMax 4
typedef struct { UInt32 ed, td[kA0TdMax], buf, since; short tdN; } A0Watch;
static A0Watch gA0[2];
static UInt32  gA0N;                                   /* events of these kinds this wake (capped at kA0Log) */
static Boolean gEdPend[2];                             /* a TD waiting on any of fn k's control EDs (last pass) */
static void RegSnap(short k, NecFn *n);
static void A0Buf(short k, UInt16 which, UInt32 phys)  /* 8 bytes as two events; which: 0 setup, 1 data seen, 2 after */
{
    UInt32 l;
    if (gA0N + 2 > kA0Log) return;
    if (!InRAM(phys, 8) || (l = PhysToLog(phys, 8)) == 0) { gA0N++; Ev((UInt8)k, kEvA0Buf, (UInt16)(which | 0x80), phys); return; }
    gA0N += 2;
    Ev((UInt8)k, kEvA0Buf, (UInt16)(which | 0x00), RdL(l)); Ev((UInt8)k, kEvA0Buf, (UInt16)(which | 0x10), RdL(l + 4));
}
static void A0TdEv(short k, UInt8 kind, short i, UInt32 t, UInt32 w0, UInt32 cbp, UInt32 be)
{
    if (gA0N + 4 > kA0Log) return;
    gA0N += 4;
    Ev((UInt8)k, kind, (UInt16)i, t); Ev((UInt8)k, kEvTdW0, (UInt16)i, w0);
    Ev((UInt8)k, kEvTdCbp, (UInt16)i, cbp); Ev((UInt8)k, kEvTdBe, (UInt16)i, be);
}
static void A0Finish(short k)                          /* the transfer has left the ED: its TDs and its data, now */
{
    A0Watch *a = &gA0[k]; short j;
    for (j = 0; j < a->tdN; j++) {
        UInt32 l = PhysToLog(a->td[j], 16);
        if (l) A0TdEv(k, kEvA0TdDone, j, a->td[j], Swap32(RdL(l)), Swap32(RdL(l + 4)), Swap32(RdL(l + 12)));
    }
    if (a->buf) A0Buf(k, 2, a->buf);
    if (gA0N < kA0Log) { gA0N++; Ev((UInt8)k, kEvA0End, (UInt16)a->tdN, RecMs() - a->since); }
    a->tdN = 0; a->buf = 0; a->ed = 0;
}
static void A0Track(short k, NecFn *n, UInt32 ed, UInt32 tail, UInt32 headp)
{
    A0Watch *a = &gA0[k]; UInt32 t = headp & ~0xFUL; short i, j;
    /* idle: if it is the ED we follow, its transfer has ended. (Apple keeps a full-speed AND a low-speed address-0 ED
       on one controller (uwake7B fn0 at 15513): an idle one must not end the other's transfer.) */
    if (t == tail) { if (a->tdN > 0 && a->ed == ed) A0Finish(k); return; }
    if (a->tdN > 0) {                                            /* a NEW transfer, with no idle pass in between? */
        Boolean ours = false;
        for (j = 0; j < a->tdN; j++) if (a->td[j] == t) ours = true;
        if (!ours || a->ed != ed) A0Finish(k);
    }
    if (a->tdN == 0) { a->ed = ed; a->since = RecMs(); a->buf = 0; }
    for (i = 0; i < kA0TdMax && t != 0 && t != tail; i++) {
        UInt32 l, w0, cbp, nx, be; Boolean seen = false;
        if (!InRAM(t, 16) || (l = PhysToLog(t, 16)) == 0) break;
        w0 = Swap32(RdL(l)); cbp = Swap32(RdL(l + 4)); nx = Swap32(RdL(l + 8)) & ~0xFUL; be = Swap32(RdL(l + 12));
        for (j = 0; j < a->tdN; j++) if (a->td[j] == t) seen = true;
        if (!seen && a->tdN < kA0TdMax) {
            UInt32 dp = (w0 >> 19) & 3UL;
            a->td[a->tdN++] = t;
            A0TdEv(k, kEvA0Td, (short)(a->tdN - 1), t, w0, cbp, be);
            if (cbp != 0 && be - cbp == 7UL) {
                if (dp == 0) A0Buf(k, 0, cbp);                   /* the SETUP packet: which request */
                else if (dp == 2) { a->buf = cbp; A0Buf(k, 1, cbp); }   /* an 8-byte IN: the descriptor's first 8 */
            }
        }
        t = nx;
    }
    RegSnap(k, n);                                               /* is the controller working the list? (CLF, CurrentED) */
}

/* ---- uwake8: OS X's answer to an add that failed, which OS 9 lacks. IOUSBFamily (AppleUSBHubPort::DetachDevice,
   10.4 and 10.5) removes a device that did not attach and adds it again, and on every 4th failure turns its port's
   power off and on. OS 9's USL gives up (uwake7B: two requests to the flash's new address, then nothing for 345 s).
   Here, on fn1 only (the free port; the keyboard and the mouse are on fn0): a control ED for a NEW address (FA != 0)
   whose maximum packet size is 0 is a failed add's dead pipe. Once the system has settled (kRcvSettleMs after it
   appeared, task level running, the USL's add-lock free, nothing waiting on fn1's list, a device still connected),
   fn1's root-hub power goes off for kRcvOffMs and on again: the NEC reports the device leaving and coming back, and
   Apple's hub driver removes the dead device and adds it again, as at a replug. At most kRcvMax times per wake, never
   twice for one dead address. Each write is read back and repeated up to 10 times: the NEC does not always take a
   register write (Apple's errata IBB-2UE-00030; OS X rewrites up to 10 times). These, and EarlyPower's, are the ONLY
   register writes mode B makes.
   uwake9: the recovery runs at TASK LEVEL (RcvTask, from the Notification Manager pass). On the FW800 uwake8B's
   recorder timer stopped for good at +15.16 s, three seconds before the recovery it carried was due, while task level
   ran on and wrote the report. The recovery's "settled" condition needs task level anyway, and there each step is
   logged at once. RcvTask walks fn1's control list itself (through translations); the recorder still records the dead
   pipe (EdSample) for the event list.
   uwake9 also adds the PREVENTION (EarlyPower). uwake8B showed why the add fails: the flash answered its first
   descriptor read "not yet" (NAK) for over 150 ms (fn0's Bluetooth module answered the same read in 24 ms); Apple's OS 9
   add gave up at ~150 ms (the USB spec allows a device 500 ms) and went on with an all-zero buffer: packet size 0. fn1's
   ports stay unpowered until Apple's root-hub driver runs after the 12 s AppleTalk pause (+14.6 s; fn0's at +2.5 s), so
   the flash is still booting when Apple's add reaches it. EarlyPower turns fn1's root-hub power on as soon as Apple's
   reload returns (+1.8 s): the flash boots during the pause. ---- */
#define kRcvMax      2
#define kRcvSettleMs 3000UL
#define kRcvOffMs    2000UL
static UInt32 gDeadFA[2], gDeadMs[2], gDeadW0[2];        /* per controller: the dead pipe's address, since when, its w0 */
static UInt32 gDeadN[2], gDead1FA[2], gDead1Ms[2], gDead1W0[2];   /* for the report: how many, and the first */
static UInt32 gRcvFA[2];                                 /* the last dead address recovered (never twice) */
static UInt32 gRcvState, gRcvT, gRcvN, gRcvSkip;         /* TASK LEVEL: 0 idle / 1 power off; since; done; why not */
static UInt32 gRcvSeenFA, gRcvSeenMs;                    /* the dead pipe task level is waiting on, since when */
static UInt32 gEarlyPs, gEarlyWrites, gEarlyWhy, gEarlyMs;   /* EarlyPower: port 1 after, writes used, why not, when */
#ifdef UWAKE_QTEST_RCV
static UInt32 gQtRcvFA;                                  /* QEMU only: the healthy device reported dead on purpose */
#endif
static const char *RcvWhy(UInt32 why)
{
    return why == 1 ? "fn1's ports are always powered (NPS)" : why == 2 ? "fn1 switches power per port (PSM)" :
           why == 3 ? "no device connected on fn1" : why == 9 ? "fn1's PCI configuration was not restored" : "?";
}
#ifdef UWAKE_MODE_B                       /* (uwake10: the early power and the recovery are mode B's; mode A keeps the NEC) */
/* HcRhStatus: LPSC = SetGlobalPower, LPS = ClearGlobalPower. step: 1 recovery OFF, 2 recovery ON, 3 early power ON. MMIO
   only (any level). Returns the writes used; 11 = port 1's power never read back as asked. */
static UInt32 RcvPower(short k, NecFn *n, Boolean on, UInt16 step)
{
    unsigned long v = on ? 0x00010000UL : 0x00000001UL, ps = 0; short t;
    for (t = 0; t < 10; t++) {
        *(volatile unsigned long *)(n->base + kHcRhStatus) = Swap32(v);
        __asm__ volatile ("eieio" ::: "memory");
        ps = OhciRd(n->base, kHcRhPortStatus1);
        if (on ? (ps & 0x100UL) != 0 : (ps & 0x100UL) == 0) break;      /* port 1's PPS says it took */
    }
    Ev((UInt8)k, kEvRcv, (UInt16)(step | ((t + 1) << 8)), ps);
    return (UInt32)(t + 1);
}
static UInt32 RcvSwitchable(NecFn *n)                    /* 0 = power switched for all ports at once; 1 NPS, 2 PSM */
{
    UInt32 rha = OhciRd(n->base, kHcRhDescriptorA);
#ifdef UWAKE_QTEST_RCV
    rha &= ~0x200UL;                                      /* QEMU only: its OHCI declares NPS (00000203) yet carries out
                                                             the global power writes; let them and their read-back run, as
                                                             they will on the FW800's fn1 (0A000002: NPS 0, PSM 0) */
#endif
    return (rha & 0x200UL) ? 1UL : (rha & 0x100UL) ? 2UL : 0UL;
}
static NecFn *Fn1(void)                                  /* the second OHCI function: the flash's controller */
{
    short i, k;
    for (i = 0, k = 0; i < gNecN; i++) if (gNec[i].ohci) { if (k == 1) return &gNec[i]; k++; }
    return NULL;
}
#ifdef UWAKE_MODE_B
/* From WrapSleepProc, right after Apple's reload returns (inside the wake message: MMIO and memory only). */
static void EarlyPower(void)
{
    NecFn *n = Fn1();
    gEarlyPs = 0; gEarlyWrites = 0; gEarlyMs = RecMs();
    if (n == NULL || n->base == 0 || !(n->outcome == kOutRestored || n->outcome == kOutIntact)) { gEarlyWhy = 9; return; }
    gEarlyWhy = RcvSwitchable(n);
    if (gEarlyWhy) { Ev(1, kEvRcv, (UInt16)(8 + gEarlyWhy), OhciRd(n->base, kHcRhDescriptorA)); return; }
    gEarlyWrites = RcvPower(1, n, true, 3);
    gEarlyPs = OhciRd(n->base, kHcRhPortStatus1);
}
#endif
static void RcvTask(void)                               /* TASK LEVEL: each Notification Manager pass while recording */
{
    NecFn *n = Fn1(); UInt32 now = RecMs(), ed, dead = 0, pend = 0, why; short i, p, np;
    if (n == NULL || n->base == 0 || !n->rec.live) return;
    if (gRcvState == 1) {                                 /* the power is off: on again after kRcvOffMs */
        if (now - gRcvT >= kRcvOffMs) {
            UInt32 w = RcvPower(1, n, true, 2);
            gRcvState = 0; gRcvN++; gRcvFA[1] = gRcvSeenFA; gRcvSeenFA = 0;
            Lg("  uwake9 RECOVERY at +%lu ms: fn1's root-hub power ON again (%lu write(s)): Apple should now add the device "
               "again", (unsigned long)now, (unsigned long)w);
            LogFlush();
        }
        return;
    }
    if (gRcvN >= kRcvMax || now + 10000UL > kRecEndMs) return;   /* (never leave the power off past the recording) */
#ifndef UWAKE_QTEST_EDVM
    if (!gRamReadable) return;
#endif
    ed = OhciRd(n->base, kHcControlHeadED) & ~0xFUL;
    for (i = 0; i < 8 && ed != 0; i++) {
        UInt32 la, w0, tail, headp, fa, mps; Boolean skip;
        if (!InRAM(ed, 16) || (la = PhysToLog(ed, 16)) == 0) return;   /* the whole list not readable: decide nothing */
        w0 = Swap32(RdL(la)); tail = Swap32(RdL(la + 4)) & ~0xFUL; headp = Swap32(RdL(la + 8));
        fa = w0 & 0x7FUL; mps = (w0 >> 16) & 0x7FFUL; skip = ((w0 >> 14) & 1UL) != 0;
#ifdef UWAKE_QTEST_RCV
        if (fa != 0 && !skip && gQtRcvFA == 0) gQtRcvFA = fa;          /* QEMU: the first fn1 device after the wake */
        if (fa != 0 && fa == gQtRcvFA) mps = 0;                        /* is reported dead on purpose */
#endif
        if (fa != 0 && !skip && mps == 0 && fa != gRcvFA[1] && dead == 0) dead = fa;
        if ((headp & ~0xFUL) != tail) pend = 1;
        ed = Swap32(RdL(la + 12)) & ~0xFUL;
    }
    if (dead == 0) { gRcvSeenFA = 0; return; }
    if (dead != gRcvSeenFA) {
        gRcvSeenFA = dead; gRcvSeenMs = now;
        Lg("  uwake9: fn1's add built FA %lu's pipe with packet size 0 (task level saw it at +%lu ms): the recovery is due "
           "%lu ms later, once nothing is in flight", (unsigned long)dead, (unsigned long)now, (unsigned long)kRcvSettleMs);
        LogFlush();
        return;
    }
    if (now - gRcvSeenMs < kRcvSettleMs || pend) return;
    if (gUslD && RdL(gUslD + U_LOCK) != 0) return;        /* an add in flight */
    why = RcvSwitchable(n);
    if (!why) {
        np = (short)(OhciRd(n->base, kHcRhDescriptorA) & 0xFFUL); if (np > 4) np = 4; why = 3;
        for (p = 0; p < np; p++) if (OhciRd(n->base, kHcRhPortStatus1 + 4UL * (unsigned long)p) & 1UL) why = 0;
    }
    if (why) {
        if (gRcvSkip != why) {
            gRcvSkip = why; Ev(1, kEvRcv, (UInt16)(8 + why), OhciRd(n->base, kHcRhDescriptorA));
            Lg("  uwake9: the recovery is due but not done: %s", RcvWhy(why)); LogFlush();
        }
        return;
    }
    {   UInt32 w = RcvPower(1, n, false, 1);
        gRcvState = 1; gRcvT = now;
        Lg("  uwake9 RECOVERY at +%lu ms: fn1's root-hub power OFF (%lu write(s)%s) for %lu ms, for the dead pipe FA %lu",
           (unsigned long)now, (unsigned long)(w > 10 ? 10 : w), w > 10 ? ", NEVER read back" : "",
           (unsigned long)kRcvOffMs, (unsigned long)dead);
        LogFlush();
    }
}
#endif /* UWAKE_MODE_B: RcvPower .. RcvTask */

/* fn k's CONTROL list, up to 8 EDs from HcControlHeadED: FA, endpoint, low speed, sKip, Halted, a transfer waiting
   (HeadP != TailP) and the waiting TD's CC/EC. CC stays 0xE/0xF ("not accessed") while a device NAKs; EC climbs when
   it does not answer at all. Memory reads of Apple's EDs/TDs (little-endian), through translations (PhysToLog). uwake8:
   the address-0 ED's transfers (A0Track) and a new address's pipe with maximum packet size 0 (the dead pipe). */
static UInt32 gEdSum[2][8]; static UInt32 gEdN;
static void EdSample(short k, NecFn *n)
{
    unsigned long ed; short i;
    Boolean a0 = false, anyPend = false, walked = false;         /* uwake8 */
    UInt32 dead = 0, deadW0 = 0;
#ifndef UWAKE_QTEST_EDVM                  /* (a QEMU-only build reads them with VM on, to check the decoding) */
    if (!gRamReadable) return;
#endif
    if (k > 1) return;
    ed = OhciRd(n->base, kHcControlHeadED) & ~0xFUL;
    for (i = 0; i < 8; i++) {
        UInt32 sum, here = ed, w0 = 0, tail = 0, headp = 0, next = 0; Boolean raw = false;
        UInt32 la = 0;                                           /* uwake6: where to READ physical ed (0 = nowhere) */
        if (ed == 0) { sum = 0x7FFFFFFFUL; walked = true; }      /* (past the end of the list) */
        else if (!InRAM(ed, 16)) { sum = 0x7FFFFFFEUL; ed = 0; }
        else if ((la = PhysToLog(ed, 16)) == 0) { sum = 0x7FFFFFFDUL; ed = 0; }   /* no translation (yet) */
        else {
            UInt32 pend, tw0 = 0, tl;
            w0 = Swap32(RdL(la)); tail = Swap32(RdL(la + 4)) & ~0xFUL; headp = Swap32(RdL(la + 8));
            next = Swap32(RdL(la + 12)) & ~0xFUL; pend = ((headp & ~0xFUL) != tail) ? 1UL : 0UL; raw = true;
            if (pend && (tl = PhysToLog(headp & ~0xFUL, 16)) != 0) tw0 = Swap32(RdL(tl));
            {   /* uwake8: the address-0 ED's transfers; a new address's pipe with maximum packet size 0 */
                UInt32 fa = w0 & 0x7FUL, mps = (w0 >> 16) & 0x7FFUL; Boolean skip = ((w0 >> 14) & 1UL) != 0;
#ifdef UWAKE_QTEST_RCV
                if (k == 1 && fa != 0 && !skip && gQtRcvFA == 0 && gTaskSeen) gQtRcvFA = fa;   /* QEMU: the first fn1
                                                                     device after the wake is reported dead on purpose */
                if (k == 1 && fa != 0 && fa == gQtRcvFA) mps = 0;
#endif
                if (fa == 0 && !skip) { a0 = true; gRecStep = 0x40UL + (UInt32)k; A0Track(k, n, here, tail, headp); gRecStep = 0x20UL + (UInt32)k; }
                else if (fa != 0 && !skip && mps == 0 && fa != gRcvFA[k] && dead == 0) { dead = fa; deadW0 = w0; }
                if (pend) anyPend = true;
            }
            sum = 0x80000000UL | (w0 & 0x7FUL) | (((w0 >> 7) & 0xFUL) << 7) | (((w0 >> 13) & 1UL) << 11) |
                  (((w0 >> 14) & 1UL) << 12) | ((headp & 1UL) << 13) | (pend << 14) |
                  (pend ? ((((tw0 >> 26) & 3UL) << 16) | (((tw0 >> 28) & 0xFUL) << 20)) : 0UL);
            ed = next;
        }
        if (sum != gEdSum[k][i]) {
            gEdSum[k][i] = sum;
            if (++gEdN <= kEdLog) Ev((UInt8)k, kEvEd, (UInt16)i, sum);
            if (sum == 0x7FFFFFFDUL && gEdRawN < kEdRawLog) { gEdRawN++; Ev((UInt8)k, kEvEdUnread, (UInt16)i, here); }
            if (raw && gEdRawN < kEdRawLog) {            /* uwake5: the ED's own words, to tell a misread from a list */
                gEdRawN++;
                Ev((UInt8)k, kEvEdRaw, (UInt16)i, here);
                Ev((UInt8)k, kEvEdW, (UInt16)(i | 0x000), w0);   Ev((UInt8)k, kEvEdW, (UInt16)(i | 0x100), tail);
                Ev((UInt8)k, kEvEdW, (UInt16)(i | 0x200), headp); Ev((UInt8)k, kEvEdW, (UInt16)(i | 0x300), next);
            }
        }
        /* the end of the list, or a pointer the walk may not follow (outside RAM, or not confirmed readable): stop here,
           the slots after it read as "end" without being logged as such */
        if (sum == 0x7FFFFFFFUL || sum == 0x7FFFFFFEUL || sum == 0x7FFFFFFDUL) {
            short j; for (j = (short)(i + 1); j < 8; j++) gEdSum[k][j] = 0x7FFFFFFFUL;
            break;
        }
    }
    /* uwake8: what this pass saw, for the recovery (decided only on a walk that reached the list's end) */
    gEdPend[k] = anyPend;
    if (walked && !a0 && gA0[k].tdN > 0) A0Finish(k);           /* the address-0 ED left the list mid-transfer */
    if (dead) {
        if (dead != gDeadFA[k]) {
            gDeadFA[k] = dead; gDeadMs[k] = RecMs(); gDeadW0[k] = deadW0; Ev((UInt8)k, kEvDead, (UInt16)dead, deadW0);
            if (gDeadN[k]++ == 0) { gDead1FA[k] = dead; gDead1Ms[k] = gDeadMs[k]; gDead1W0[k] = deadW0; }
        }
    } else if (walked && gDeadFA[k] != 0 && gRcvState == 0) {   /* Apple removed it (a replug, or our power cycle) */
        Ev((UInt8)k, kEvDeadGone, (UInt16)gDeadFA[k], 0); gDeadFA[k] = 0;
    }
}

/* uwake5: the TDs the controller just retired (HcDoneHead: the done queue as it builds, newest first, linked through
   NextTD). Each one's CC (how it ended), direction and buffer say what the device answered: a stall, no answer
   (DeviceNotResponding), a toggle or CRC error, or success. Read while the done-head register still holds them, before
   Apple's ISR recycles the TDs. Memory reads only (VM off: physical == logical); up to 4 TDs per write-back. */
static void TdChain(short k, unsigned long a)
{
    short i;
#ifndef UWAKE_QTEST_EDVM
    if (!gRamReadable) return;
#endif
    for (i = 0; i < 4 && a != 0 && gTdN + 4 <= kTdLog; i++) {
        UInt32 w0, cbp, nx, be, la;
        a &= ~0xFUL;
        if (!InRAM(a, 16)) { gTdN++; Ev((UInt8)k, kEvTdBad, (UInt16)i, a); return; }
        if ((la = PhysToLog(a, 16)) == 0) { gTdN++; Ev((UInt8)k, kEvTdUnread, (UInt16)i, a); return; }   /* uwake6 */
        w0 = Swap32(RdL(la)); cbp = Swap32(RdL(la + 4)); nx = Swap32(RdL(la + 8)); be = Swap32(RdL(la + 12));
        gTdN += 4;
        Ev((UInt8)k, kEvTd, (UInt16)i, a); Ev((UInt8)k, kEvTdW0, (UInt16)i, w0);
        Ev((UInt8)k, kEvTdCbp, (UInt16)i, cbp); Ev((UInt8)k, kEvTdBe, (UInt16)i, be);
        a = nx;
    }
}
/* uwake5: the list registers, raw (the recorder's ED walk of fn1 read different "EDs" every wake: is the head it reads
   the one the controller uses?). Logged when they change; sampled once a second and at each of fn1's write-backs. */
static const UInt16 kSnapOff[6] = { 0x08, 0x18, 0x20, 0x24, 0x28, 0x2C };   /* CommandStatus, HCCA, Control/Bulk Head+Current */
static void RegSnap(short k, NecFn *n)
{
    short i;
    if (k > 1) return;
    for (i = 0; i < 6; i++) {
        UInt32 v = OhciRd(n->base, (unsigned long)kSnapOff[i]);
        if (v != gSnapLast[k][i] && gRegN < kRegLog) { gSnapLast[k][i] = v; gRegN++; Ev((UInt8)k, kEvReg, kSnapOff[i], v); }
    }
}

static void SampleFn(short k, NecFn *n)
{
    RecFn *r = &n->rec; unsigned long ctl, ist, ien, done, fm, pend; short p, np; int c0 = 4 + 6 * k;
    UInt32 now = RecMs();
    if (!r->live) return;
    gRecStep = 0x10UL + (UInt32)k;                               /* uwake9 */
    ctl = OhciRd(n->base, kHcControl);
    ist = OhciRd(n->base, kHcInterruptStatus) & ~0x04UL;      /* SF (every frame, enabled only now and then) left out */
    ien = OhciRd(n->base, kHcInterruptEnable); done = OhciRd(n->base, kHcDoneHead);
    fm = OhciRd(n->base, kHcFmNumber) & 0xFFFFUL;
    if (ist & kIntUE) r->ueSeen = true;
    if (ctl != r->ctl) { r->ctl = ctl; Ev((UInt8)k, kEvCtl, 0, ctl); }
    if (ist != r->ist) { r->ist = ist; if (++r->istN <= kChurnLog) Ev((UInt8)k, kEvIst, 0, ist); }
    if (ien != r->ien) { r->ien = ien; Ev((UInt8)k, kEvIen, 0, ien); }
    if (done != r->done) {
        r->done = done; if (++r->doneN <= kChurnLog) Ev((UInt8)k, kEvDone, 0, done);
        if (done != 0 && (k == 1 || r->doneN <= 40)) TdChain(k, done);   /* fn1 (the flash's) always; fn0 the first 40 */
        if (k == 1) RegSnap(k, n);
    }
    if (now - gSnapMs[k & 1] >= 1000UL) { gSnapMs[k & 1] = now; RegSnap(k, n); }
    np = n->launch.ndp; if (np > 4) np = 4;
    for (p = 0; p < np; p++) {
        unsigned long ps = OhciRd(n->base, kHcRhPortStatus1 + 4UL * (unsigned long)p);
        if (ps != r->port[p]) {
            r->port[p] = ps; Ev((UInt8)k, kEvPort, (UInt16)(p + 1), ps);
            if (now >= 60000UL) gFastUntil = now + kFastMs;          /* a replug: sample it at 8 ms again */
        }
    }
    if (fm == r->fm) {
        if (++r->fmSame == 4 && !r->fmStuck) { r->fmStuck = true; Ev((UInt8)k, kEvFmStop, 0, fm); }
    } else {
        r->fm = fm; r->fmSame = 0;
        if (r->fmStuck) { r->fmStuck = false; Ev((UInt8)k, kEvFmRun, 0, fm); }
    }
    { unsigned long hcfs = (ctl >> 6) & 3UL;
      Cell(c0, hcfs == 0 ? 0x00 : hcfs == 1 ? 0x40 : hcfs == 2 ? 0xFF : 0x80); }
    Cell(c0 + 1, (ien & kIntMIE) ? 0xFF : 0x00);
    pend = ist & ien & 0x7FUL;
    Cell(c0 + 2, (pend & 0x02UL) ? 0x00 : 0xFF);                 /* WDH */
    Cell(c0 + 3, (pend & 0x40UL) ? 0x00 : 0xFF);                 /* RHSC */
    Cell(c0 + 4, ((pend & 0x2DUL) || (ist & kIntUE)) ? 0x00 : 0xFF);   /* SO, RD, FNO pending+enabled; UE pending at all */
    Cell(c0 + 5, r->fmStuck ? 0x00 : ((fm & 0x40UL) ? 0xC0 : 0xFF));
    gRecStep = 0x20UL + (UInt32)k;                               /* uwake9: where a pass is (the stall watchdog) */
    EdSample(k, n);
    gRecStep = 0x30UL + (UInt32)k;
}

static void RecPass(void)
{
    UInt32 t = kLowTicks, h = kLowFSQHead, now = RecMs(), m, tm = kLowTime; short i, k; Boolean keys = false;
    gRecPasses++;
    gRecStep = 1; gRecPassMs = now; gHbAtPass = gHbN;           /* uwake9 */
    if (gClockN < 2 && tm != gClockLast) { gClockLast = tm; Ev(0xFF, kEvClock, (UInt16)gClockN, tm); gClockN++; }
    if ((gRecPasses & 31UL) == 0) CellsRepaint();               /* a redraw may have painted over the row */
    Cell(0, (unsigned char)((gRecPasses & 16UL) ? 0x00 : 0xFF));
    /* Ticks */
    if (t == gTicksLast) {
        if (++gTicksSame == 125 && !gTicksStuck) { gTicksStuck = true; Ev(0xFF, kEvTicksStop, 0, t); }
    } else {
        gTicksLast = t; gTicksSame = 0;
        if (gTicksStuck) { gTicksStuck = false; Ev(0xFF, kEvTicksRun, 0, t); }
    }
    Cell(2, gTicksStuck ? 0x00 : 0xFF);
    Cell(3, gCell3);
    /* the File Manager queue head: a call held 250 ms+ is logged, 2 s+ is the lone-Slave signature */
    if (h != gFsqHead) {
        if (gFsqSlow) Ev(0xFF, kEvFsqDone, 0, now - gFsqSince);
        gFsqHead = h; gFsqSince = now; gFsqSlow = false; gFsqStuck = false;
        if (h) gFsqCalls++;
    } else if (h != 0) {
        UInt32 held = now - gFsqSince;
        if (!gFsqSlow && held >= 250UL) {
            UInt16 trap = 0;
            if (h >= 0x1000UL && h < gLRam && (h & 1UL) == 0) trap = *(volatile UInt16 *)(h + 6);   /* ioTrap */
            gFsqSlow = true; gFsqSlowN++; Ev(0xFF, kEvFsqSlow, trap, h);
        }
        if (!gFsqStuck && held >= 2000UL) { gFsqStuck = true; Ev(0xFF, kEvFsqStuck, 0, h); }
    }
    Cell(16, h == 0 ? 0xFF : gFsqStuck ? 0x00 : 0x80);
    /* the cursor and the keyboard as the system sees them */
    m = kLowRawMouse;
    if (m != gMouseLast) {
        gMouseLast = m; gMouseN++;
        if (gMouseN == 1) gMouse1Ms = now;
        if (gMouseN <= kInputLog) Ev(0xFF, kEvMouse, 0, m);
    }
    Cell(18, gMouseN >= 3 ? 0xFF : 0x00);                       /* 3+ moves: a mouse, not the wake re-placing the cursor */
    for (k = 0; k < 4; k++) { UInt32 v = kLowKeyMap[k]; if (v != gKeyLast[k]) { gKeyLast[k] = v; keys = true; } }
    if (keys) {
        gKeyN++;
        if (gKeyN == 1) gKey1Ms = now;
        if (gKeyN <= kInputLog) Ev(0xFF, kEvKeys, 0, gKeyLast[0] | gKeyLast[1] | gKeyLast[2] | gKeyLast[3]);
    }
    Cell(19, gKeyN ? 0xFF : 0x00);
    gRecStep = 2;
    UslSample();
    gRecStep = 3;
    for (i = 0, k = 0; i < gNecN && k < 2; i++) if (gNec[i].ohci) { SampleFn(k, &gNec[i]); k++; }
    gRecStep = 4;
}

static OSStatus RecTimer(void *p1, void *p2)
{
    AbsoluteTime when; OSStatus e; UInt32 ms; Duration per;
    (void)p2;
    if ((UInt32)p1 != gRecGen || !gRecOn) return noErr;
    RecPass();
    ms = RecMs();
    if (ms >= kRecEndMs) {
        gRecOn = false; gRecDone = true; Ev(0xFF, kEvEnd, (UInt16)(kRecEndMs / 1000UL), ms); Cell(17, 0x00);
        return noErr;
    }
    per = (ms < 60000UL || ms < gFastUntil) ? 8 : 50;
    Cell(17, per == 8 ? 0xFF : 0x80);
    if ((UInt32)per != gRecPer) { gRecPer = (UInt32)per; Ev(0xFF, kEvPhase, (UInt16)per, ms); }
#ifdef UWAKE_QTEST_RCV
    if (!gQtDropped && ms >= 20000UL) { gQtDropped = true; gRecStep = 6; return noErr; }   /* QEMU only: the timer chain
                                                                     lost once on purpose: the watchdog must restart it */
#endif
    when = AddDurationToAbsolute(per, UpTime());
    e = SetInterruptTimer(&when, RecTimer, p1, &gRecTimer);
    if (e != noErr) { gRecArmErr = (UInt32)e; gRecOn = false; gRecDone = true; }
    gRecStep = 5;                                                /* uwake9: the next pass is armed */
    return noErr;
}
/* uwake9, TASK LEVEL (the watchdog in NmPass): a fresh timer chain. The old chain's callbacks, if one ever comes, see a
   stale generation and stop; one CPU, so a callback never runs half-way through this. */
static void RecRearm(void)
{
    AbsoluteTime when; OSStatus e;
    gRecGen++;
    when = AddDurationToAbsolute(8, UpTime());
    e = SetInterruptTimer(&when, RecTimer, (void *)gRecGen, &gRecTimer);
    if (e != noErr) { gRecArmErr = (UInt32)e; Lg("  !! uwake9: restarting the recorder failed (SetInterruptTimer %ld)", (long)e); }
}

static void RecStart(void)               /* memory and the timer only: NO screen writes from here (see gCell3) */
{
    AbsoluteTime when; short i;
    gRecN = 0; gRecLost = 0; gRecPasses = 0; gRecDone = false; gRecArmErr = 0;
    gFsqHead = 0xFFFFFFFFUL; gFsqSince = 0; gFsqCalls = 0; gFsqSlowN = 0; gFsqSlow = false; gFsqStuck = false;
    gTicksLast = kLowTicks; gTicksSame = 0; gTicksStuck = false;
    gMouseLast = kLowRawMouse; gMouseN = 0; gMouse1Ms = 0; gKeyN = 0; gKey1Ms = 0;
    for (i = 0; i < 4; i++) gKeyLast[i] = kLowKeyMap[i];
    gUslLock = 0xFFFFFFFFUL; gUslN = 0; gUslAddrN = 0; gEdN = 0; gTaskSeen = false;
    { short a, b; for (a = 0; a < 2; a++) for (b = 0; b < 8; b++) gEdSum[a][b] = 0xFFFFFFFFUL; }
    gFastUntil = 0; gRecPer = 8; gClockLast = kLowTime; gClockN = 0; gTdN = 0; gRegN = 0; gEdRawN = 0;
    gCandR = gCandW; gUnreadN = 0;                   /* uwake6: confirmed blocks stay confirmed (VM off: fixed mapping) */
    memset(gA0, 0, sizeof gA0); gA0N = 0;            /* uwake8: the address-0 capture and the recovery, per wake */
    { short a; for (a = 0; a < 2; a++) { gEdPend[a] = false; gDeadFA[a] = 0; gDeadMs[a] = 0; gDeadW0[a] = 0; gRcvFA[a] = 0;
                                         gDeadN[a] = 0; gDead1FA[a] = 0; gDead1Ms[a] = 0; gDead1W0[a] = 0; } }
    gRcvState = 0; gRcvT = 0; gRcvN = 0; gRcvSkip = 0; gRcvSeenFA = 0; gRcvSeenMs = 0;
    gEarlyWhy = 8; gEarlyWrites = 0; gEarlyPs = 0; gEarlyMs = 0;   /* uwake9: 8 = EarlyPower has not run (yet) */
    gRecStep = 0; gWdPasses = 0; gWdMs = 0; gRearmN = 0;
#ifdef UWAKE_QTEST_RCV
    gQtRcvFA = 0; gQtDropped = false;
#endif
    { short a, b; for (a = 0; a < 2; a++) for (b = 0; b < 3; b++) gValLast[a][b] = 0; }
    { short a, b; for (a = 0; a < 2; a++) { gSnapMs[a] = 0UL - 1000UL; for (b = 0; b < 6; b++) gSnapLast[a][b] = 0xFFFFFFFFUL; } }
    gRecT0 = UpTime(); gEnableMs = 0;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        memset(&n->rec, 0xFF, sizeof n->rec);    /* every first sample differs: the first pass records the state */
        n->rec.fmSame = 0; n->rec.fmStuck = false; n->rec.ueSeen = false; n->rec.istN = 0; n->rec.doneN = 0;
        n->rec.live = n->ohci && n->base != 0 && (n->outcome == kOutRestored || n->outcome == kOutIntact);
    }
    CellsRepaint();
    gRecGen++; gRecOn = true;
    when = AddDurationToAbsolute(8, UpTime());
    if (SetInterruptTimer(&when, RecTimer, (void *)gRecGen, &gRecTimer) != noErr) { gRecOn = false; gRecDone = true; }
}

/* ============================================================== MODE A: keep Apple's stack */
#ifdef UWAKE_MODE_A
static Boolean gArmed;                   /* one restore per sleep */
static Boolean gRestoredThisWake;        /* the restore ran; the enable handler acts once */

static void RestoreOhci(NecFn *n)        /* NECQuiet v3/v7 + HcFmNumber (Apple's own recovery writes it back first) */
{
    const OhciRegs *s;
    OhciRegs       *a = &n->after;
    short           p;
    Boolean         anyPower = false;
    long            settle;

    n->ohciOutcome = kOhciNone; n->mmioWrites = 0; n->hcrPolls = 0; n->fmWrote = n->fmBack = 0xFFFFFFFFUL;
    n->usedReq = n->req.ok;
    s = n->req.ok ? &n->req : (n->launch.ok ? &n->launch : NULL);
    if (s == NULL || !n->baseOK) { n->ohciOutcome = kOhciNoSnap; return; }
    if (!CfgDecodes(n)) { n->ohciOutcome = kOhciNotAnswering; return; }

    n->oRev = OhciRd(n->base, kHcRevision); n->oCtl = OhciRd(n->base, kHcControl);
    n->oHCCA = OhciRd(n->base, kHcHCCA);
    if ((n->oRev & 0xFFUL) != 0x10UL) { n->ohciOutcome = kOhciNotAnswering; return; }
    if (n->oHCCA == s->hcca) { n->ohciOutcome = kOhciIntact; return; }

    OhciWr(n, kHcCommandStatus, 0x00000001UL);              /* HCR: software reset */
    DelayMs(3);
    while ((OhciRd(n->base, kHcCommandStatus) & 1UL) && n->hcrPolls < 100) { n->hcrPolls++; DelayMs(1); }

    OhciWr(n, kHcInterruptStatus,  0xFFFFFFFFUL);           /* right after the HCR nothing meaningful is pending */
    OhciWr(n, kHcInterruptDisable, 0xFFFFFFFFUL);           /* disable all, MIE too -> no ISR until the enable */
    n->fmWrote = s->fmNumber & 0xFFFFUL;
    OhciWr(n, kHcFmNumber,        n->fmWrote);              /* else Apple's frame clock (slot 24) jumps back <= 65 s */
    n->fmBack = OhciRd(n->base, kHcFmNumber) & 0xFFFFUL;    /* (OHCI calls it read-only for the HCD: did it take?) */
    OhciWr(n, kHcPeriodicStart,   s->perStart);
    OhciWr(n, kHcBulkHeadED,      s->bulkHead);
    OhciWr(n, kHcControlHeadED,   s->ctlHead);
    OhciWr(n, kHcHCCA,            s->hcca);
    OhciWr(n, kHcFmInterval,      s->fmInterval);

    OhciWr(n, kHcRhDescriptorB,   s->rhB);
    OhciWr(n, kHcRhDescriptorA,   s->rhA | 0x00001000UL);   /* NOCP while power comes up */
    for (p = 0; p < s->ndp; p++) if (s->port[p] & 0x100UL) anyPower = true;
    if (anyPower) {
        OhciWr(n, kHcRhStatus, 0x00010000UL);                /* LPSC: global power on */
        for (p = 0; p < s->ndp; p++)
            if (s->port[p] & 0x100UL) OhciWr(n, kHcRhPortStatus1 + 4UL * (unsigned long)p, 0x00000100UL);
        settle = (long)((s->rhA >> 24) & 0xFFUL) * 2L;
        if (settle < 20) settle = 20;
        if (settle > 100) settle = 100;
        DelayMs(settle);
    }
    OhciWr(n, kHcRhDescriptorA, s->rhA);
    if (s->rhSt & 0x00008000UL) OhciWr(n, kHcRhStatus, 0x00008000UL);   /* DRWE back on */

    ReadOhci(n, a);
    n->ohciOutcome = (a->hcca == s->hcca && a->ctlHead == s->ctlHead && a->bulkHead == s->bulkHead &&
                      a->perStart == s->perStart && (a->intEn & kIntMIE) == 0UL &&
                      (a->fmInterval & 0x7FFF3FFFUL) == (s->fmInterval & 0x7FFF3FFFUL) &&
                      ((a->ctl >> 6) & 3UL) != 0UL)
                     ? kOhciRestored : kOhciVerifyFail;
}

/* The sleep demand, AFTER Apple's level 1 (HcControl = 0xC0): the interrupt enables Apple left (its ISR changes SF and
   RHSC as it goes, so the boot value is not the right one to put back). Whichever of our handlers sees HCFS Suspend. */
static void SleepSnap(short by)
{
    short i;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; unsigned long ctl;
        if (!n->ohci || !n->baseOK || n->slpOK || !CfgDecodes(n)) continue;
        ctl = OhciRd(n->base, kHcControl);
        if (((ctl >> 6) & 3UL) != 3UL) continue;              /* Apple's level 1 has not run yet */
        n->slpOK = true; n->slpBy = by; n->slpCtl = ctl;
        n->slpIen = OhciRd(n->base, kHcInterruptEnable); n->slpIst = OhciRd(n->base, kHcInterruptStatus);
        n->hcdSlp = HccaDone(NecHcca(n));
    }
}

/* ---- uwake10 (mode A): keep the NEC's state through the deep sleep, as KeyLargo's USB keeps power.
   Every unarmed sleep loses it all (stock, uwake1-9: PCI config and OHCI registers zero at the wake). Path A's Run 11
   (2026-10-23, its Wake Probe log) read it back INTACT when the NEC slept SUSPENDED with
   PCI PME armed (PMCSR = PME_Status | PME_Enable | D3hot = 0x8103): HcControl Suspend at the wake, the root ports still
   connected and powered; fn1 came back too, in Apple's own suspend state (HcControl 0xC0, no remote-wakeup bits). Apple's
   UIM never arms it: its PCI-ID table gives the NEC quirk 0x80, no PCI power management, so its sleep demand is level 1
   only (lists off, HcControl 0xC0, PMCSR untouched, MIE left on) (reference_os9_rom_ohci_wake_no_reinit). Its level 0
   at the wake resumes a controller that kept its state and re-programs nothing else: right for KeyLargo, and for a NEC
   that kept its state.
   ArmNec runs at the demand in our 250 handler: NativePowerMgrLib's sleep loop runs the chain in REVERSE, so 250 comes
   just after Apple's USB handlers (300) and their level 1 (October's reading of InsertPowerHandlerInOrder,
   pm-driverlist/pm_driverlist.c 455-457). Only a function SleepSnap saw in Suspend is armed, by one config write; nothing
   reads its registers afterwards (D3hot does not decode memory). WakeDisarm runs first at the wake, in the same handler
   (the wake loop runs in order: 250 before Apple's 300): D0, PME off, PME_Status cleared, 10 ms if it was in D3, all
   before any register access and before RestoreConfig (which only touches PMCSR when the config was lost). */
static void ArmNec(void)
{
    short i;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; UsbFn *f = &gFn[n->fi]; unsigned short pm;
        n->armPm0 = n->armPm1 = 0xFFFFUL;
        if (!n->ohci) continue;
        if (n->pmOff == 0) { n->armWhy = 1; continue; }                  /* no PCI PM capability (QEMU's OHCI) */
        pm = (unsigned short)(n->pmOff + 4);
        n->armPm0 = CfgRd16(f, pm);
        if (!n->slpOK || ((n->slpCtl >> 6) & 3UL) != 3UL) { n->armWhy = 2; continue; }   /* Apple did not suspend it */
        CfgWr16(n, pm, 0x8103UL);                 /* PME_Status (W1C) | PME_Enable | D3hot (reference_os9_pme_status_clear_when_arming) */
        n->armPm1 = CfgRd16(f, pm);
        n->armWhy = 0;
    }
}
static void WakeDisarm(NecFn *n)
{
    UsbFn *f = &gFn[n->fi]; unsigned short pm;
    n->wkPm = 0xFFFFUL;
    if (n->pmOff == 0) return;
    /* uwake20: the EHCI too when EhciDemand put it in D3hot: RestoreConfig moves PMCSR only when the config was lost, and
       a D3hot EHCI with its config intact would never decode again (our driver's next register read: a machine check) */
    if (!n->ohci && !(n->ehci && n->ehSusp)) return;
    pm = (unsigned short)(n->pmOff + 4);
    n->wkPm = CfgRd16(f, pm);
    if (n->wkPm == 0xFFFFUL) return;                                     /* (config space did not answer) */
    CfgWr16(n, pm, 0x8000UL);                     /* D0; PME_Enable 0; PME_Status written 1 = cleared */
    if ((n->wkPm & 3UL) != 0) DelayMs(10);        /* D3hot -> D0: 10 ms before its registers are used (PCI PM 1.1) */
}

/* The enable (priority 350: Apple's level 0 has run). Corrected v7:
   1. EVERY OHCI function must decode first: Apple's ISR reads an all-ones function as MIE + every source pending and
      claims the shared line forever (UE recovery = 26 ms busy-wait, every entry). One fails -> enable none.
   2. A function with UE pending stays off (enabling it starts Apple's in-ISR recovery, which has no retry limit).
   3. Clear RD (its handler re-enters the Power Manager from secondary interrupt level), SO, SF, FNO, OC. NOT WDH or
      RHSC: an unconsumed done batch and the root hub's change notice are Apple's to take (v7's blanket acknowledge
      threw them away: leaked transfers, no re-enumeration).
   4. The sources Apple had on at the sleep demand, minus UE (put back after the recorder if it never fired), MIE last. */
static void EnableA(void)
{
    short i; Boolean allDecode = true, any = false;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        if (!n->ohci) continue;
        n->enRev = n->enIen = n->enCtl = n->enIst = 0xFFFFFFFFUL;
        if (n->base != 0 && CfgDecodes(n)) {
            n->enRev = OhciRd(n->base, kHcRevision);
            if ((n->enRev & 0xFFUL) == 0x10UL) n->enIen = OhciRd(n->base, kHcInterruptEnable);
        }
        if ((n->enRev & 0xFFUL) != 0x10UL || n->enIen == 0xFFFFFFFFUL) allDecode = false;
    }
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; unsigned long src;
        if (!n->ohci) continue;
        if ((n->enRev & 0xFFUL) == 0x10UL) {
            n->enCtl = OhciRd(n->base, kHcControl); n->enIst = OhciRd(n->base, kHcInterruptStatus);
        }
        n->hcdEn = HccaDone(NecHcca(n));
        if (n->ohciOutcome != kOhciRestored) { n->enOutcome = kEnNotOurs; continue; }   /* Apple's own state: untouched */
        if (!allDecode) { n->enOutcome = kEnWithheldDecode; continue; }
        if (n->enIst & kIntUE) { n->enOutcome = kEnWithheldUE; continue; }
        if (((n->enCtl >> 6) & 3UL) != 2UL) {                 /* Apple's level 0 should have left it Operational */
            OhciWr(n, kHcControl, n->launch.ctl ? n->launch.ctl : 0x000000BCUL); n->enForcedOp = true;
        }
        src = n->slpOK ? n->slpIen : n->req.ok ? n->req.intEn : n->launch.intEn;
        if (src == 0 || src == 0xFFFFFFFFUL) src = 0x8000007BUL;
        src &= 0x4000007FUL & ~kIntUE;
        OhciWr(n, kHcInterruptStatus, 0x4000002DUL);          /* clear RD, SO, SF, FNO, OC; NOT WDH, RHSC, UE */
        OhciWr(n, kHcInterruptEnable, src);                   /* the sources first */
        OhciWr(n, kHcInterruptEnable, kIntMIE);               /* MIE last */
        n->enMask = src; n->enOutcome = kEnOn; any = true;
    }
    gCell3 = any ? 0xFF : 0x00;
}

static void UeBack(void)                 /* TASK LEVEL, after the recorder: UE back into Apple's mask if it never fired */
{
    short i;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        if (!n->ohci || n->enOutcome != kEnOn || n->rec.ueSeen || n->ueBack || !CfgDecodes(n)) continue;
        OhciWr(n, kHcInterruptEnable, kIntUE); n->ueBack = true;
    }
}

/* ---- uwake11 (mode A): keyboard wake ---------------------------------------------------------------------------------
   uwake10A (FW800, 2026-11-13) keeps the NEC's state through the sleep, but a key did not wake the Mac. Apple's level 1
   leaves HcControl at 0xC0: Suspend with RemoteWakeupConnected and RemoteWakeupEnable OFF, and RWE is what lets a
   ResumeDetected reach the system as a remote wakeup (OHCI 1.0a 7.1.2), whatever the devices signal. Path A woke this
   FW800 by key in October (Run 15, rwake3) with: HcControl RWC|RWE before the sleep; DRWE cleared (a connect change is
   then no wake); SET_FEATURE(DEVICE_REMOTE_WAKEUP) on the keyboard AND its hub; PMCSR 0x8103
   (reference_os9_pme_status_clear_when_arming); the hub's change bits acknowledged
   (reference_usb_hub_change_bits_remote_wakeup). Here Apple's stack runs the devices, so:
   1. THE SURVEY (task level, each NM pass: KwPass, further down). Apple publishes its live USB tree in the Name Registry
      under each controller (the nodes ASP lists: ASP-USB-REGISTRY.md): every device with its deviceRef, VendorID,
      ProductID and parent-deviceRef; a keyboard is an interface node 3/1/1. Through Apple's stack (USBServicesLib's
      USBDeviceRequest; standard requests and the hub's class reads only) the survey reads each device's configuration
      bmAttributes and GET_STATUS, and arms with SET_FEATURE(DEVICE_REMOTE_WAKEUP) the keyboard and each hub between it
      and the root, only where the device says it can (bmAttributes 0x20) and is not armed already; then GET_STATUS
      again. QEMU's trace (aw/out-snhA2, 2026-11-13) shows Apple's stack arming every HID device it configures (keyboard,
      mouse) and not the stick; whether it arms a HUB is one of the survey's readings. The Bluetooth module's HID proxy
      (05AC:1000) is never armed by us. On a hub on the path the port status and change bits are read (the trap). Only
      devices on a function that hosts a keyboard are asked anything (fn1's flash is left alone).
   2. THE DEMAND (KwDemand: our 250 handler, after SleepSnap, before ArmNec, the NEC still D0). On a function whose
      keyboard path the last finished survey read as armed: a stale RD cleared (with RWE it would wake at once); DRWE
      cleared (CRWE); HcControl RWC|RWE added to Apple's Suspend (0xC0 -> 0x6C0); each read back and written again, up to
      10 times (NEC errata IBB-2UE-00030). Any other function is left exactly as uwake10A leaves it.
   3. THE WAKE (KwWake: 250, after the restore, before Apple's level 0): HcControl, InterruptStatus, HcRhStatus and the
      root ports recorded, reads only. RD is left for Apple's ISR, as on KeyLargo. THE ENABLE (KwEnable: 350, Apple's
      level 0 has run): DRWE put back as Apple had it (a lost function's restore already wrote the request's value). */
#define kHcCtlRWC  0x00000200UL            /* HcControl RemoteWakeupConnected */
#define kHcCtlRWE  0x00000400UL            /* HcControl RemoteWakeupEnable */
#define kHcRhDRWE  0x00008000UL            /* HcRhStatus: read DeviceRemoteWakeupEnable; write 1 = SetRemoteWakeupEnable */
#define kHcRhCRWE  0x80000000UL            /* HcRhStatus: write 1 = ClearRemoteWakeupEnable */
#define kIntRD     0x00000008UL            /* HcInterruptStatus ResumeDetected */
static Boolean          gKwFn[kMaxNec];    /* the last FINISHED survey: an armed keyboard path on this function */
static Boolean          gKwFnKbd[kMaxNec]; /* uwake18A: the reasons, for the log -- an armed keyboard path, */
static Boolean          gKwFnBt[kMaxNec];  /* and/or a Bluetooth controller (its driver decides)             */
static UInt32           gKwSurveys;        /* surveys finished this boot */
static volatile Boolean gKwHold;           /* a sleep is under way: the survey sends nothing new */
static volatile UInt32  gKwSlpTime;        /* uwake12: the wall clock (low-memory Time) at the last sleep's request */

static void KwDemand(void)                 /* 250 at the sleep demand: MMIO only (the NEC is still D0) */
{
    short i, t, p;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; unsigned long b, v = 0;
        n->kwCtl0 = n->kwCtl1 = n->kwRh0 = n->kwRh1 = n->kwIst0 = 0xFFFFFFFFUL;
        for (p = 0; p < 4; p++) n->kwP0[p] = 0xFFFFFFFFUL;
        n->kwTries = 0; n->kwRdClr = n->kwDrweOff = false;
        if (!n->ohci || !n->baseOK) { n->kwWhy = 9; continue; }
        if (gKwSurveys == 0) { n->kwWhy = 3; continue; }
        if (!gKwFn[i]) { n->kwWhy = 1; continue; }
        if (!n->slpOK || !CfgDecodes(n)) { n->kwWhy = 2; continue; }
        b = n->base;
        n->kwCtl0 = OhciRd(b, kHcControl); n->kwRh0 = OhciRd(b, kHcRhStatus); n->kwIst0 = OhciRd(b, kHcInterruptStatus);
        for (p = 0; p < n->launch.ndp && p < 4; p++) n->kwP0[p] = OhciRd(b, kHcRhPortStatus1 + 4UL * (unsigned long)p);
        if (((n->kwCtl0 >> 6) & 3UL) != 3UL) { n->kwWhy = 2; continue; }          /* Suspend (Apple's level 1) only */
        if (n->kwIst0 & kIntRD) { OhciWr(n, kHcInterruptStatus, kIntRD); n->kwRdClr = true; }
        if (n->kwRh0 & kHcRhDRWE) {
            for (t = 0; t < 10; t++) { OhciWr(n, kHcRhStatus, kHcRhCRWE); if (!(OhciRd(b, kHcRhStatus) & kHcRhDRWE)) break; }
            n->kwDrweOff = true;
        }
        for (t = 0; t < 10; t++) {                 /* HCFS stays Suspend (11): the same state, no transition */
            OhciWr(n, kHcControl, n->kwCtl0 | kHcCtlRWC | kHcCtlRWE);
            v = OhciRd(b, kHcControl);
            if ((v & (kHcCtlRWC | kHcCtlRWE | 0xC0UL)) == (kHcCtlRWC | kHcCtlRWE | 0xC0UL)) break;
        }
        n->kwTries = (short)(t < 10 ? t + 1 : 10);
        n->kwCtl1 = v; n->kwRh1 = OhciRd(b, kHcRhStatus);
        n->kwWhy = ((v & (kHcCtlRWC | kHcCtlRWE | 0xC0UL)) == (kHcCtlRWC | kHcCtlRWE | 0xC0UL)) ? 0 : 4;
    }
}
static void KwWake(NecFn *n)               /* 250 at the wake, after our restore: what the wake left (reads only) */
{
    short p; unsigned long b = n->base;
    n->wReq = n->req; n->wSlpOK = n->slpOK; n->wSlpBy = n->slpBy;      /* uwake12: this sleep's, for its report */
    n->wSlpCtl = n->slpCtl; n->wSlpIen = n->slpIen; n->wSlpIst = n->slpIst;
    n->kwWkCtl = n->kwWkIst = n->kwWkRh = n->kwEnRh = 0xFFFFFFFFUL;
    for (p = 0; p < 4; p++) n->kwWkP[p] = 0xFFFFFFFFUL;
    if (!n->ohci || !n->baseOK || !CfgDecodes(n)) return;
    n->kwWkCtl = OhciRd(b, kHcControl); n->kwWkIst = OhciRd(b, kHcInterruptStatus); n->kwWkRh = OhciRd(b, kHcRhStatus);
    for (p = 0; p < n->launch.ndp && p < 4; p++) n->kwWkP[p] = OhciRd(b, kHcRhPortStatus1 + 4UL * (unsigned long)p);
}
static void KwEnable(void)                 /* 350 at the wake: Apple's level 0 has run; DRWE back as Apple had it */
{
    short i;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        if (!n->kwDrweOff || !n->ohci || !n->baseOK || !CfgDecodes(n)) continue;
        if (n->ohciOutcome == kOhciIntact) OhciWr(n, kHcRhStatus, kHcRhDRWE);   /* (lost: RestoreOhci wrote it back) */
        n->kwEnRh = OhciRd(n->base, kHcRhStatus);
    }
}

static OSStatus RestoreHandler(UInt32 message, void *param, UInt32 refCon, RegEntryID *regEntryID)
{
    short i;
    (void)param; (void)refCon; (void)regEntryID;
    switch (message) {
    case kPMSleepRequest:                 /* still D0: snapshot the live OHCI */
        StopRecorder(0);
        gKwHold = true;                    /* uwake11: no new survey request until the wake */
        gKwSlpTime = kLowTime;             /* uwake12: when this sleep began (a memory read) */
        for (i = 0; i < gNecN; i++) {
            NecFn *n = &gNec[i];
            n->req.ok = false; n->slpOK = false;
            if (n->ohci && n->baseOK && CfgDecodes(n)) ReadOhci(n, &n->req);
        }
        return noErr;
    case kPMSleepDemand:
    case kPMSuspendDemand:
        StopRecorder(1);
        DemandReads();                     /* every NEC function's config; uwake19: the EHCI's registers */
        SleepSnap(250);
        KwDemand();                        /* uwake11: remote wakeup on the keyboard's function, while it still decodes */
        for (i = 0; i < gNecN; i++) EhciDemand(&gNec[i]);   /* uwake20: a RUNNING EHCI suspended, halted, D3hot */
        ArmNec();                          /* uwake10: Apple's level 1 has run (reverse order): arm PME, D3hot */
        gArmed = true;
        gHbDark = 0x80;                    /* the heartbeat turns grey/white: our sleep step ran */
        return noErr;
    case kPMSleepWakeUp:
    case kPMDozeWakeUp:
    case kPMSuspendWakeUp:
    case kPMWakeToDoze:
    case kPMDozeToFullWakeUp:
        if (!gArmed) return noErr;         /* one restore per sleep */
        gArmed = false;
        for (i = 0; i < gNecN; i++) {
            NecFn *n = &gNec[i];
            WakeDisarm(n);                                     /* uwake10: D0 and PME off before anything else */
            n->hcdWake = n->ohci ? HccaDone(NecHcca(n)) : 0xFFFFFFFFUL;
            RestoreConfig(n);                                  /* PCI config back first (so it decodes) */
            EhciWake(n);                                       /* uwake19: what the wake left the EHCI; uwake20: the
                                                                  resume of what EhciDemand stopped */
            n->ohciOutcome = kOhciNone; n->enOutcome = kEnNone; n->enForcedOp = false; n->ueBack = false;
            /* registers, interrupts MASKED, HCFS Suspend. uwake10: also when the config survived (the armed NEC should
               keep both): RestoreOhci then only compares the HCCA (kOhciIntact, no writes), and restores the registers if
               they alone were lost. */
            if ((n->outcome == kOutRestored || n->outcome == kOutIntact) && n->ohci) RestoreOhci(n);
            KwWake(n);                                         /* uwake11: what woke it (reads only) */
        }
        gKwHold = false;
        gWakeN++;
        RecStart();
        Ev(0xFF, kEvRestore, (UInt16)message, gWakeN);
        gCell3 = 0x80;
        gRestoredThisWake = true;
        return noErr;
    case kPMSleepRevoke:
        gArmed = false;
        gKwHold = false;
        /* uwake20: a revoke should only follow a request, never a demand; if one ever followed our demand, the EHCI we
           stopped would be left in D3hot with no wake to resume it, and our driver's next register read would be a
           machine check. So: the wake's EHCI steps for it, now. */
        for (i = 0; i < gNecN; i++)
            if (gNec[i].ehci && gNec[i].ehSusp) { WakeDisarm(&gNec[i]); RestoreConfig(&gNec[i]); EhciWake(&gNec[i]); }
        return noErr;
    default:
        return kPMMessageNotHandled;
    }
}

static OSStatus EnableHandler(UInt32 message, void *param, UInt32 refCon, RegEntryID *regEntryID)
{
    (void)param; (void)refCon; (void)regEntryID;
    switch (message) {
    case kPMSleepWakeUp:
    case kPMDozeWakeUp:
    case kPMSuspendWakeUp:
    case kPMWakeToDoze:
    case kPMDozeToFullWakeUp:
        if (!gRestoredThisWake) return noErr;
        gRestoredThisWake = false;
        EnableA();
        KwEnable();                        /* uwake11: DRWE back as Apple had it */
        EhciEnable();                      /* uwake20: the resumed EHCI's interrupt mask, last */
        gEnableMs = RecMs();
        Ev(0xFF, kEvEnable, (UInt16)message, gEnableMs);
        return noErr;
    case kPMSleepDemand:
    case kPMSuspendDemand:
        SleepSnap(350);
        return noErr;
    case kPMSleepRequest:
    case kPMSleepRevoke:
        return noErr;
    default:
        return kPMMessageNotHandled;
    }
}
#endif /* UWAKE_MODE_A */

/* ============================================================== MODE B: Apple's PowerBook mode */
#ifdef UWAKE_MODE_B
#define kSleepQProcInfo 0x00131832UL      /* register-based: D0 = message, A0 = SleepQRecPtr, result in D0 */
static unsigned char   *gUsbG;            /* the USB Expert's gUSBGlobals (its data + 0x4CA0) */
static UniversalProcPtr gApplePb;         /* the Expert's PowerBook1,1 sleep procedure: descriptor at g - 0x280 */
static UniversalProcPtr gWrapUPP;
typedef OSErr (*SysModeFn)(void);
static SysModeFn        gBeginSys, gEndSys;
static volatile struct {
    UInt32 reqN, reqMs; SInt32 reqR;      /* 1: the request (Apple asks the class drivers) */
    UInt32 demN, demMs; SInt32 demR;      /* 2: the demand = Apple's teardown */
    UInt32 revN;                          /* 4: revoked */
    UInt32 wakeN, wakeMs, cfgMs; SInt32 wakeR;   /* 3: our config restore, then Apple's reload */
    UInt32 otherN, otherLast;
    Boolean sysMode;
} gSq;

/* The Power Manager's sleep queue (task level, inside Sleep): memory, PCI config, the timer, Apple's procedure. */
static long WrapSleepProc(long message, void *qRec)
{
    AbsoluteTime t0 = UpTime(); long r; short i;
    if (message == 1 || message == 2) StopRecorder((UInt16)(message == 2));
    if (message == 3) {
        for (i = 0; i < gNecN; i++) RestoreConfig(&gNec[i]);       /* bus master off; the reload turns it on */
        gSq.cfgMs = MsSince(t0);
        gWakeN++;
        RecStart();
        Ev(0xFF, kEvRestore, 3, gWakeN);
        gCell3 = 0x80;                                              /* grey: inside Apple's reload */
        gSq.sysMode = (gBeginSys != NULL && gEndSys != NULL);
        if (gSq.sysMode) (void)gBeginSys();                         /* the reloaded UIMs belong to the system, not to */
        r = CallUniversalProc(gApplePb, kSleepQProcInfo, message, qRec);   /* the application that asked to sleep */
        if (gSq.sysMode) (void)gEndSys();
        gSq.wakeN++; gSq.wakeMs = MsSince(t0); gSq.wakeR = (SInt32)r;
        Ev(0xFF, kEvReload, (UInt16)r, gSq.wakeMs);
        EarlyPower();                                               /* uwake9: fn1's ports powered now, not at +14.6 s */
        gCell3 = 0xFF;
        return r;
    }
    r = CallUniversalProc(gApplePb, kSleepQProcInfo, message, qRec);
    switch (message) {
    case 1:  gSq.reqN++; gSq.reqMs = MsSince(t0); gSq.reqR = (SInt32)r; break;
    case 2:  gSq.demN++; gSq.demMs = MsSince(t0); gSq.demR = (SInt32)r; gHbDark = 0x80; break;   /* heartbeat grey */
    case 4:  gSq.revN++; break;
    default: gSq.otherN++; gSq.otherLast = (UInt32)message; break;
    }
    return r;
}

static Boolean RdIsPPCSleepProc(const unsigned char *rd)   /* a native routine descriptor with the sleep-queue procInfo */
{
    return *(const UInt16 *)rd == 0xAAFEU && *(const UInt32 *)(rd + 0x0C) == kSleepQProcInfo && rd[0x11] == 1;
}
static Ptr LibSym(const unsigned char *lib, const unsigned char *sym, CFragLoadOptions how, OSErr *err, CFragSymbolClass *cls)
{
    CFragConnectionID conn = 0; Ptr mainAddr = NULL, sa = NULL; Str255 errName;
    *err = GetSharedLibrary(lib, kPowerPCCFragArch, how, &conn, &mainAddr, errName);
    if (*err != noErr) return NULL;
    *err = FindSymbol(conn, sym, &sa, cls);
    return (*err == noErr) ? sa : NULL;
}

static Boolean InstallPbMode(void)       /* TASK LEVEL (kInitialize) */
{
    OSErr e; CFragSymbolClass cls = 0; unsigned char *g, *rdNative, *rdPb; UInt32 cur; THz z;
    g = (unsigned char *)LibSym("\pUSBFamilyExpertLib", "\pgUSBGlobals", kFindCFrag, &e, &cls);
    Lg("USB Expert (already loaded): gUSBGlobals %08lX, err %d, class %d (%s)", (unsigned long)g, (int)e, (int)cls,
       cls == kDataCFragSymbol ? "data" : "NOT data");
    if (g == NULL || cls != kDataCFragSymbol) { Lg("!! no gUSBGlobals: nothing installed"); return false; }
    rdNative = g - 0x2A0; rdPb = g - 0x280; cur = *(volatile UInt32 *)(g + 0x96);
    Lg("  its sleep-queue record (g+0x90): type %d, procedure %08lX; NativeSleepTask's descriptor %08lX, the PowerBook1,1 "
       "procedure's %08lX; registered with the Native Power Manager: %d",
       (int)*(SInt16 *)(g + 0x94), (unsigned long)cur, (unsigned long)rdNative, (unsigned long)rdPb, (int)*(UInt8 *)(g + 0xB9));
    if (*(SInt16 *)(g + 0x94) != 16 || cur != (UInt32)rdNative) {
        Lg("!! the Expert's sleep procedure is not NativeSleepTask's (or not a sleep-queue record): not touching it"); return false;
    }
    if (!RdIsPPCSleepProc(rdNative) || !RdIsPPCSleepProc(rdPb)) {
        Lg("!! the routine descriptors are not where Apple's Expert has them: not touching it"); return false;
    }
#ifndef UWAKE_QTEST_NOSYS                  /* (the QEMU control build leaves system mode out on purpose) */
    /* PrivateInterfaceLib, where the Expert itself imports them from (InterfaceLib does not export them) */
    gBeginSys = (SysModeFn)LibSym("\pPrivateInterfaceLib", "\pBeginSystemMode", kReferenceCFrag, &e, &cls);
    gEndSys = (SysModeFn)LibSym("\pPrivateInterfaceLib", "\pEndSystemMode", kReferenceCFrag, &e, &cls);
#endif
    Lg("  BeginSystemMode %08lX, EndSystemMode %08lX%s", (unsigned long)gBeginSys, (unsigned long)gEndSys,
       (gBeginSys && gEndSys) ? "" : " (not both found: the reload runs in the sleeping application's context)");
    z = GetZone(); SetZone(SystemZone());
    gWrapUPP = NewRoutineDescriptor((ProcPtr)WrapSleepProc, kSleepQProcInfo, kPowerPCISA);
    SetZone(z);
    if (gWrapUPP == NULL) { Lg("!! NewRoutineDescriptor failed: nothing installed"); return false; }
    gUsbG = g; gApplePb = (UniversalProcPtr)rdPb;
    *(volatile UInt32 *)(g + 0x96) = (UInt32)gWrapUPP;     /* in place: the record keeps its place in the sleep queue */
    Lg("MODE B INSTALLED: the Expert's sleep procedure is now ours (%08lX), around Apple's PowerBook1,1 procedure",
       (unsigned long)gWrapUPP);
    return true;
}
#endif /* UWAKE_MODE_B */

/* ============================================================== decoding, for the log (task level) */
static const char *Hcfs(unsigned long ctl)
{
    switch ((ctl >> 6) & 3UL) { case 0: return "Reset"; case 1: return "Resume"; case 2: return "Operational"; default: return "Suspend"; }
}
static void IntBits(unsigned long v, char *b, int max)
{
    static const char *nm[7] = { "SO", "WDH", "SF", "RD", "UE", "FNO", "RHSC" };
    int i, k = 0; b[0] = 0;
    if (v == 0xFFFFFFFFUL) { snprintf(b, (size_t)max, "all ones: NOT DECODING"); return; }
    for (i = 0; i < 7 && k < max - 8; i++) if (v & (1UL << i)) k += snprintf(b + k, (size_t)(max - k), "%s ", nm[i]);
    if ((v & 0x40000000UL) && k < max - 6) k += snprintf(b + k, (size_t)(max - k), "OC ");
    if ((v & kIntMIE) && k < max - 6) k += snprintf(b + k, (size_t)(max - k), "MIE ");
    if (k == 0) snprintf(b, (size_t)max, "-");
}
static void PortBits(unsigned long v, char *b, int max)
{
    static const char *nm[] = { "CCS", "PES", "PSS", "POCI", "PRS", 0, 0, 0, "PPS", "LSDA" };
    int i, k = 0; b[0] = 0;
    for (i = 0; i < 10 && k < max - 8; i++) if (nm[i] && (v & (1UL << i))) k += snprintf(b + k, (size_t)(max - k), "%s ", nm[i]);
    if ((v & 0x10000UL) && k < max - 6) k += snprintf(b + k, (size_t)(max - k), "CSC ");
    if ((v & 0x20000UL) && k < max - 6) k += snprintf(b + k, (size_t)(max - k), "PESC ");
    if ((v & 0x40000UL) && k < max - 6) k += snprintf(b + k, (size_t)(max - k), "PSSC ");
    if ((v & 0x100000UL) && k < max - 6) k += snprintf(b + k, (size_t)(max - k), "PRSC ");
    if (k == 0) snprintf(b, (size_t)max, "-");
}
static const char *CfgOut(short o)
{
    switch (o) { case kOutRestored: return "RESTORED (it had been lost)"; case kOutIntact: return "intact (not lost)";
                 case kOutAbsent: return "ABSENT (device id wrong)"; case kOutVerifyFail: return "VERIFY FAILED"; default: return "not run"; }
}
static void LogRegs(const char *what, const OhciRegs *r)
{
    char ib[80], eb[80]; short p;
    if (!r->ok) { Lg("    %s: not sane / not taken", what); return; }
    IntBits(r->intSt, ib, sizeof ib); IntBits(r->intEn, eb, sizeof eb);
    Lg("    %s: HcControl %08lX (HCFS %s)  HCCA %08lX  ControlHead %08lX  BulkHead %08lX  FmInterval %08lX  FmNumber %04lX  PeriodicStart %04lX",
       what, r->ctl, Hcfs(r->ctl), r->hcca, r->ctlHead, r->bulkHead, r->fmInterval, r->fmNumber & 0xFFFFUL, r->perStart & 0xFFFFUL);
    Lg("      InterruptStatus %08lX [%s]  InterruptEnable %08lX [%s]  RhDescA %08lX (NDP %ld)  RhStatus %08lX",
       r->intSt, ib, r->intEn, eb, r->rhA, r->rhA & 0xFFUL, r->rhSt);
    for (p = 0; p < r->ndp && p < 4; p++) { char pb[80]; PortBits(r->port[p], pb, sizeof pb); Lg("      port %d: %08lX [%s]", p + 1, r->port[p], pb); }
}
#ifdef UWAKE_MODE_A
static void LogHcd(const char *when, unsigned long v)
{
    if (v == 0xFFFFFFFFUL) Lg("    HCCA done head (RAM) %s: not read (VM on, or no HCCA)", when);
    else Lg("    HCCA done head (RAM) %s: %08lX%s", when, v, v ? "  <- a done batch not consumed yet (at the enable, after a USB "
            "wake, normal: Apple's ISR takes it when interrupts return)" : " (none waiting)");
}
static const char *OhciOut(short o)
{
    switch (o) { case kOhciRestored: return "RESTORED (HCR + registers + port power; Suspend, interrupts masked)";
                 case kOhciIntact: return "intact (HCCA survived)"; case kOhciNoSnap: return "NO SNAPSHOT";
                 case kOhciNotAnswering: return "NOT ANSWERING"; case kOhciVerifyFail: return "VERIFY FAILED"; default: return "not run"; }
}
static const char *EnOut(short o)
{
    switch (o) { case kEnOn: return "ON (the sources Apple had on at sleep, minus UE; MIE last; RD/SO/SF/FNO/OC cleared, WDH/RHSC left for Apple)";
                 case kEnWithheldDecode: return "WITHHELD: an OHCI function did not decode (Apple's ISR would claim the shared line forever)";
                 case kEnWithheldUE: return "WITHHELD: UE pending (Apple's in-ISR recovery has no retry limit)";
                 case kEnNotOurs: return "not touched (we did not restore this function: Apple's own state)"; default: return "not run"; }
}
#endif

static const char *CcName(UInt32 cc)
{
    static const char *nm[16] = { "NoError", "CRC", "BitStuffing", "DataToggleMismatch", "Stall", "DeviceNotResponding",
        "PIDCheckFailure", "UnexpectedPID", "DataOverrun", "DataUnderrun", "reserved", "reserved", "BufferOverrun",
        "BufferUnderrun", "NotAccessed", "NotAccessed" };
    return nm[cc & 0xF];
}
/* uwake8: an address-0 transfer's 8 bytes (two events, memory order). which: 0 the SETUP packet, 1 an 8-byte IN's
   buffer when queued, 2 the same buffer once the transfer had left the ED. */
static void LogA0Buf(UInt32 ms, short k, unsigned which, UInt32 a, UInt32 b)
{
    unsigned x[8], i; char tail[96];
    for (i = 0; i < 4; i++) { x[i] = (unsigned)((a >> (24 - 8 * i)) & 0xFFUL); x[4 + i] = (unsigned)((b >> (24 - 8 * i)) & 0xFFUL); }
    if (which == 0) {
        const char *rq = x[1] == 6 ? "GET_DESCRIPTOR" : x[1] == 5 ? "SET_ADDRESS" : x[1] == 9 ? "SET_CONFIGURATION" :
                         x[1] == 0 ? "GET_STATUS" : x[1] == 1 ? "CLEAR_FEATURE" : x[1] == 3 ? "SET_FEATURE" : "a request";
        Lg("  %7lu  fn%d     setup packet %02X %02X %02X %02X %02X %02X %02X %02X = %s, value %04X, index %04X, length %u",
           (unsigned long)ms, k, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], rq, x[2] | (x[3] << 8), x[4] | (x[5] << 8),
           x[6] | (x[7] << 8));
        return;
    }
    tail[0] = 0;
    if (x[0] == 0x12 && x[1] == 0x01)
        snprintf(tail, sizeof tail, " = a device descriptor: USB %X.%02X, class %u, maximum packet size %u", x[3], x[2], x[4], x[7]);
    else if ((a | b) == 0) snprintf(tail, sizeof tail, " (all zero)");
    Lg("  %7lu  fn%d     the 8-byte read's buffer %s: %02X %02X %02X %02X %02X %02X %02X %02X%s", (unsigned long)ms, k,
       which == 1 ? "when queued" : "AFTERWARDS", x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], tail);
}
static void LogEd(UInt32 ms, short fn, short idx, UInt32 v)
{
    if (v == 0x7FFFFFFFUL) { Lg("  %7lu  fn%d control ED #%d: (end of the list)", (unsigned long)ms, fn, idx); return; }
    if (v == 0x7FFFFFFEUL) { Lg("  %7lu  fn%d control ED #%d: pointer OUTSIDE RAM", (unsigned long)ms, fn, idx); return; }
    if (v == 0x7FFFFFFDUL) { Lg("  %7lu  fn%d control ED #%d: NOT READ: no translation covers its memory (yet)",
                                (unsigned long)ms, fn, idx); return; }
    if (v & 0x4000UL)
        Lg("  %7lu  fn%d control ED #%d: FA %lu EP %lu%s%s%s, a TD WAITING: CC %lX (%s), EC %lu", (unsigned long)ms, fn, idx,
           v & 0x7FUL, (v >> 7) & 0xFUL, (v & 0x800UL) ? " low-speed" : "", (v & 0x1000UL) ? " sKip" : "",
           (v & 0x2000UL) ? " HALTED" : "", (v >> 20) & 0xFUL, CcName(v >> 20), (v >> 16) & 3UL);
    else
        Lg("  %7lu  fn%d control ED #%d: FA %lu EP %lu%s%s%s, idle", (unsigned long)ms, fn, idx, v & 0x7FUL, (v >> 7) & 0xFUL,
           (v & 0x800UL) ? " low-speed" : "", (v & 0x1000UL) ? " sKip" : "", (v & 0x2000UL) ? " HALTED" : "");
}

#ifdef UWAKE_MODE_A
/* ---- uwake11: the survey of Apple's USB tree (TASK LEVEL: the Notification Manager pass) ----
   One request at a time: KwIssue sends it, the USL's completion (KwDone, secondary interrupt level) marks it done, the
   next NM pass takes the result (KwTake) and sends the next (KwAdvance). A request with no completion after 5 s stops
   the survey for this boot (its parameter block may still be the USL's, so it is never reused); an immediate error (the
   USL refused it, and no completion will come: reference_os9_usb_device_ref_goes_stale) is recorded and the survey goes
   on. It runs ~20 s after boot, after each wake's report, and when a device comes or goes (the tree is re-read every
   10 s); never during a sleep or a wake's recording. The arming the demand uses is the last FINISHED survey's. */
typedef char KwPbSize[(sizeof(USBPB) == 72) ? 1 : -1];             /* USB.h's layout (mac68k = natural for USBPB) */
typedef char KwPbRef[(offsetof(USBPB, usbReference) == 28) ? 1 : -1];
typedef char KwPbCntl[(offsetof(USBPB, usb) == 48) ? 1 : -1];
typedef char KwPbFrame[(offsetof(USBPB, usbFrame) == 56) ? 1 : -1];

#define kKwMax    12
#define kKwPorts  8
#define kKwNone   1L                          /* the e* fields: request not sent */
typedef struct {
    UInt32  ref, parent, loc;                 /* Apple's deviceRef, parent-deviceRef, locationID */
    UInt16  vid, pid;
    UInt8   cls, hub, kbd, bt, want, nec;     /* DeviceClass; an external hub; has a 3/1/1 interface; 05AC:1000 (the
                                                 Bluetooth module's HID proxy); on a keyboard's path (we arm it); gNec # */
    UInt8   bth;                              /* uwake18A: a Bluetooth controller in HCI mode (DeviceClass E0, not the
                                                 proxy): its OWN driver arms or clears its remote wakeup (the Bluetooth
                                                 panel's "Allow Bluetooth devices to wake this computer"), never us */
    UInt8   attr, nPorts, kbdIf;              /* the configuration's bmAttributes (0 = not read); a hub's port count; the
                                                 boot keyboard's interface number */
    UInt16  st0, stD, st1;                    /* GET_STATUS as found; after QTEST_DISARM's clear; after our SET_FEATURE */
    SInt32  eAttr, eSt0, eIdle, eD, eSet, eSt1, eDesc, eHub;   /* 0 ok, else the USB status or the immediate error */
    UInt32  hubSt;                            /* wHubStatus | wHubChange << 16 */
    UInt32  port[kKwPorts];                   /* wPortStatus | wPortChange << 16 */
} KwDev;
static KwDev   gKw[kKwMax], gKwLast[kKwMax], gKwScan[kKwMax];   /* the running survey; the last finished; a re-read */
static short   gKwN, gKwLastN, gKwDev, gKwStep, gKwPort;
static Boolean gKwOn, gKwStuck, gKwWant = true;
static UInt32  gKwSig, gKwCheck;
static char    gKwPath[kMaxNec][128];         /* each NEC function's Name Registry path + ':' */
typedef OSStatus (*KwReqFn)(USBPB *pb);
static KwReqFn gKwReq;                        /* USBServicesLib's USBDeviceRequest */
static struct {
    USBPB            pb;
    volatile UInt32  done;
    volatile SInt32  status;
    volatile UInt32  act;
    UInt32           t0, wake;                /* Ticks at issue; gWakeN at issue (a sleep restarts the 5 s clock) */
    Boolean          busy, imm;
    UInt8            raw[128];                /* the data stage: a 64-byte window, 64-aligned inside (one page) */
} gKwQ;
enum { kKsAttr = 0, kKsSt0, kKsIdle, kKsDis, kKsDisSt, kKsSet, kKsSt1, kKsHubDesc, kKsHubSt, kKsPort, kKsNext };

static void KwDone(USBPB *pb)                 /* the USL's completion: SECONDARY INTERRUPT LEVEL, memory only */
{
    gKwQ.status = pb->usbStatus; gKwQ.act = pb->usbActCount;
    __asm__ volatile ("sync" ::: "memory");
    gKwQ.done = 1;
}
static UInt8 *KwBuf(void) { return (UInt8 *)(((UInt32)gKwQ.raw + 63UL) & ~63UL); }
static void KwIssue(UInt32 ref, UInt8 rt, UInt8 rq, UInt16 val, UInt16 idx, UInt16 len)   /* TASK LEVEL */
{
    OSStatus e; UInt8 *b = KwBuf();
    memset(&gKwQ.pb, 0, sizeof gKwQ.pb); memset(b, 0, 64);
    gKwQ.pb.pbLength = (UInt16)sizeof(USBPB); gKwQ.pb.pbVersion = kUSBCurrentPBVersion;
    gKwQ.pb.usbReference = (USBReference)ref; gKwQ.pb.usbCompletion = KwDone;
    gKwQ.pb.usb.cntl.BMRequestType = rt; gKwQ.pb.usb.cntl.BRequest = rq;
    gKwQ.pb.usb.cntl.WValue = val; gKwQ.pb.usb.cntl.WIndex = idx;      /* host order: the USL swaps them */
    gKwQ.pb.usbBuffer = len ? (void *)b : NULL; gKwQ.pb.usbReqCount = len;
    gKwQ.done = 0; gKwQ.status = kUSBPending; gKwQ.act = 0; gKwQ.imm = false; gKwQ.t0 = kLowTicks; gKwQ.wake = gWakeN;
    gKwQ.busy = true;
    e = gKwReq(&gKwQ.pb);
    if (e != noErr && e != kUSBPending) { gKwQ.status = e; gKwQ.imm = true; gKwQ.done = 1; }   /* no completion comes */
}
static Boolean KwOK(UInt32 need)              /* the data stage is usable: done (a short packet too) and long enough */
{
    return (gKwQ.status == noErr || gKwQ.status == kUSBUnderRunErr) && gKwQ.act >= need;
}
static UInt16 KwLe16(const UInt8 *b) { return (UInt16)(b[0] | (b[1] << 8)); }
static UInt16 KwNow(const KwDev *d)           /* the latest GET_STATUS read (0xFFFF = none) */
{
    return d->st1 != 0xFFFFU ? d->st1 : d->stD != 0xFFFFU ? d->stD : d->st0;
}
static Boolean KwArmedNow(const KwDev *d) { UInt16 s = KwNow(d); return s != 0xFFFFU && (s & 2U) != 0; }

static void KwTake(void)                      /* TASK LEVEL: the finished request's result, into its device */
{
    KwDev *d = &gKw[gKwDev]; const UInt8 *b = KwBuf(); SInt32 s = gKwQ.status;
    switch (gKwStep) {
    case kKsAttr:    d->eAttr = s; if (KwOK(8) && b[1] == kUSBConfDesc) d->attr = b[7]; gKwStep = kKsSt0; break;
    case kKsSt0:     d->eSt0 = s; if (KwOK(2)) d->st0 = KwLe16(b); gKwStep = kKsIdle; break;
    case kKsIdle:    d->eIdle = s; gKwStep = kKsDis; break;
    case kKsDis:     d->eD = s; gKwStep = kKsDisSt; break;
    case kKsDisSt:   if (KwOK(2)) d->stD = KwLe16(b); gKwStep = kKsSet; break;
    case kKsSet:     d->eSet = s; gKwStep = kKsSt1; break;
    case kKsSt1:     d->eSt1 = s; if (KwOK(2)) d->st1 = KwLe16(b); gKwStep = kKsHubDesc; break;
    case kKsHubDesc: d->eDesc = s; if (KwOK(3) && b[1] == kUSBHUBDesc) d->nPorts = b[2];
                     gKwStep = d->nPorts ? kKsHubSt : kKsNext; break;
    case kKsHubSt:   d->eHub = s; if (KwOK(4)) d->hubSt = KwLe16(b) | ((UInt32)KwLe16(b + 2) << 16);
                     gKwStep = kKsPort; gKwPort = 1; break;
    case kKsPort:    if (KwOK(4) && gKwPort >= 1 && gKwPort <= kKwPorts)
                         d->port[gKwPort - 1] = KwLe16(b) | ((UInt32)KwLe16(b + 2) << 16);
                     gKwPort++; break;
    default:         gKwStep = kKsNext; break;
    }
}
static Boolean KwAsk(const KwDev *d)          /* only devices on a function that hosts a keyboard -- or, uwake18A, a
                                                 Bluetooth controller -- are asked anything */
{
    short i;
    for (i = 0; i < gKwN; i++)
        if (((gKw[i].kbd && !gKw[i].bt) || gKw[i].bth) && gKw[i].nec == d->nec) return true;
    return false;
}
static Boolean KwPathArmed(const KwDev *d)    /* every hub between this device and the root reads armed */
{
    UInt32 p = d->parent; short g, j;
    for (g = 0; g < 6 && p != 0; g++) {
        for (j = 0; j < gKwN; j++) if (gKw[j].ref == p) break;
        if (j == gKwN) return true;           /* the root hub (not listed): the path is complete */
        if (!KwArmedNow(&gKw[j])) return false;
        p = gKw[j].parent;
    }
    return true;
}
static void KwLogSurvey(const KwDev *k, short n, const char *when);
/* ★★★★★★ uwake18A: A FUNCTION HOSTING A BLUETOOTH CONTROLLER GETS REMOTE WAKEUP TOO.
 *
 * The Bluetooth panel's "Allow Bluetooth devices to wake this computer" arms the CONTROLLER
 * (its driver sends SET_FEATURE(DEVICE_REMOTE_WAKEUP), btwake1); the NEC function it sits on
 * must be armed as well, and until now only a wired keyboard's function was. On the FW800
 * the A1044 shares fn0 with the wired keyboard, so fn0 was armed anyway -- but a Mac whose
 * only keyboard is Bluetooth would never have had it.
 *
 * ⚠ ARMED REGARDLESS OF THE CONTROLLER'S OWN STATUS, on purpose. HcControl RWE only lets a
 * device that signals resume wake the Mac, and a device signals only if ITS remote wakeup is
 * set -- so an unallowed (unarmed) controller still cannot wake anything, and the panel stays
 * the one switch. Reading its status instead would go stale: the survey re-runs on a device
 * change or after a wake, not when the checkbox is clicked. DRWE (a connect/disconnect wake)
 * stays cleared at the demand as before. */
static void KwFinish(void)                    /* TASK LEVEL: which functions get remote wakeup at the next demand */
{
    short i; Boolean fn[kMaxNec];
    for (i = 0; i < kMaxNec; i++) { fn[i] = false; gKwFnKbd[i] = false; gKwFnBt[i] = false; }
    for (i = 0; i < gKwN; i++) {
        const KwDev *d = &gKw[i];
        if (d->nec >= kMaxNec) continue;
        if (d->kbd && !d->bt && KwArmedNow(d) && KwPathArmed(d)) { fn[d->nec] = true; gKwFnKbd[d->nec] = true; }
        if (d->bth)                                               { fn[d->nec] = true; gKwFnBt[d->nec]  = true; }
    }
    memcpy(gKwFn, fn, sizeof gKwFn);
    memcpy(gKwLast, gKw, sizeof gKwLast); gKwLastN = gKwN;
    gKwSurveys++; gKwOn = false;
    KwLogSurvey(gKwLast, gKwLastN, "finished");
}
static void KwAdvance(void)                   /* TASK LEVEL: send the survey's next request, or finish it */
{
    while (gKwDev < gKwN) {
        KwDev *d = &gKw[gKwDev];
        if (gKwStep == kKsAttr && !KwAsk(d)) gKwStep = kKsNext;
        switch (gKwStep) {
        case kKsAttr:  KwIssue(d->ref, 0x80, kUSBRqGetDescriptor, (UInt16)(kUSBConfDesc << 8), 0, 9); return;
        case kKsSt0:   KwIssue(d->ref, 0x80, kUSBRqGetStatus, 0, 0, 2); return;
        case kKsIdle:
#ifdef UWAKE_QTEST
            /* QEMU only: SET_IDLE(0) on a keyboard. Apple's keyboard driver gives non-Apple keyboards a 24 ms idle rate
               (usb-ddk KeyboardModule.c kSetIdleRequest), and QEMU's HID turns each idle period into a "changed" event
               that wakes a suspended bus when the keyboard is armed (kwT1: a remote wakeup the instant the bus was
               suspended, no key pressed). A real keyboard sends idle reports only when polled. The FW800's Apple keyboard
               gets no SET_IDLE from Apple, and this build is never on hardware. */
            if (d->kbd && !d->bt) { KwIssue(d->ref, 0x21, 0x0A, 0, d->kbdIf, 0); return; }
#endif
            gKwStep = kKsDis; continue;
        case kKsDis:
#ifdef UWAKE_QTEST_DISARM                     /* QEMU only: Apple's arming undone first, so OUR SET_FEATURE is what runs */
            if (d->want && gKwSurveys == 0 && KwArmedNow(d)) {
                KwIssue(d->ref, 0x00, kUSBRqClearFeature, kUSBFeatureDeviceRemoteWakeup, 0, 0); return;
            }
#endif
            gKwStep = kKsSet; continue;
        case kKsDisSt: KwIssue(d->ref, 0x80, kUSBRqGetStatus, 0, 0, 2); return;
        case kKsSet:
#ifndef UWAKE_QTEST_NOARM                     /* (the QEMU control build arms nothing) */
            if (d->want && (d->attr & kUSBAtrRemoteWakeup) && KwNow(d) != 0xFFFFU && !KwArmedNow(d)) {
                KwIssue(d->ref, 0x00, kUSBRqSetFeature, kUSBFeatureDeviceRemoteWakeup, 0, 0); return;
            }
#endif
            gKwStep = kKsHubDesc; continue;
        case kKsSt1:   KwIssue(d->ref, 0x80, kUSBRqGetStatus, 0, 0, 2); return;
        case kKsHubDesc:
            if (d->hub && d->want) { KwIssue(d->ref, 0xA0, kUSBRqGetDescriptor, (UInt16)(kUSBHUBDesc << 8), 0, 9); return; }
            gKwStep = kKsNext; continue;
        case kKsHubSt: KwIssue(d->ref, 0xA0, kUSBRqGetStatus, 0, 0, 4); return;
        case kKsPort:
            if (gKwPort <= d->nPorts && gKwPort <= kKwPorts) {
                KwIssue(d->ref, 0xA3, kUSBRqGetStatus, 0, (UInt16)gKwPort, 4); return;
            }
            gKwStep = kKsNext; continue;
        default:
            gKwDev++; gKwStep = kKsAttr; gKwPort = 1; continue;
        }
    }
    KwFinish();
}

static unsigned long RegU32(RegEntryID *e, const char *name, Boolean *ok);   /* (below, with the NEC's discovery) */
static UInt32 KwU32(RegEntryID *e, const char *nm) { Boolean ok; unsigned long v = RegU32(e, nm, &ok); return ok ? (UInt32)v : 0; }
static UInt32 gKwBus[kMaxNec];                /* each NEC function's AAPL,BusNumber (0xFFFFFFFF = none) */
static short  gKwDup, gKwBusOff;              /* the last discovery: refs found twice; devices whose locationID bus
                                                 byte is not their controller's AAPL,BusNumber */
/* One USB node of function fi: a device (hub or not; root hubs skipped), or a boot keyboard interface (3/1/1). */
static void KwNode(RegEntryID *e, short fi, KwDev *d, short *n, UInt32 *kref, UInt8 *kif, short *nk, UInt32 *sig)
{
    char ty[8], rh[8]; RegPropertyValueSize sz = sizeof ty, rs = sizeof rh; short g, i; KwDev *k; UInt32 ref;
    if (RegistryPropertyGet(e, (RegPropertyName *)"AAPL,USBNodeType", ty, &sz) != noErr || sz < 4 ||
        ty[0] != 'u' || ty[1] != 's' || ty[2] != 'b') return;
    if (ty[3] == 'i') {                                                /* an interface: a boot keyboard is 3/1/1 */
        if (KwU32(e, "InterfaceClass") == 3UL && KwU32(e, "InterfaceSubClass") == 1UL &&
            KwU32(e, "InterfaceProtocol") == 1UL && *nk < kKwMax) {
            kref[*nk] = KwU32(e, "parent-deviceRef"); kif[*nk] = (UInt8)KwU32(e, "InterfaceNumber"); (*nk)++;
        }
        return;
    }
    if ((ty[3] != 'h' && ty[3] != 'd') || *n >= kKwMax) return;
    if (ty[3] == 'h' && RegistryPropertyGet(e, (RegPropertyName *)"RootHub", rh, &rs) == noErr && rs >= 1 && rh[0] == 't')
        return;                                                        /* a root hub: Apple's simulation in the UIM */
    ref = KwU32(e, "deviceRef");
    for (i = 0; i < *n; i++) if (d[i].ref == ref) { gKwDup++; return; }   /* (seen under an earlier function) */
    k = &d[(*n)++]; memset(k, 0, sizeof *k);
    k->ref = ref; k->parent = KwU32(e, "parent-deviceRef"); k->loc = KwU32(e, "locationID");
    k->vid = (UInt16)KwU32(e, "VendorID"); k->pid = (UInt16)KwU32(e, "ProductID");
    k->cls = (UInt8)KwU32(e, "DeviceClass"); k->hub = (UInt8)(ty[3] == 'h'); k->nec = (UInt8)fi;
    k->st0 = k->stD = k->st1 = 0xFFFFU; k->hubSt = 0xFFFFFFFFUL;
    k->eAttr = k->eSt0 = k->eIdle = k->eD = k->eSet = k->eSt1 = k->eDesc = k->eHub = kKwNone;
    for (g = 0; g < kKwPorts; g++) k->port[g] = 0xFFFFFFFFUL;
    if (gKwBus[fi] != 0xFFFFFFFFUL && (k->loc >> 24) != gKwBus[fi]) gKwBusOff++;
    *sig = (*sig ^ k->ref) * 0x01000193UL; *sig = (*sig ^ k->loc) * 0x01000193UL;
}
/* TASK LEVEL: Apple's USB tree, function by function. Each NEC function's OWN subtree is walked (the iterator set on
   its entry: kRegIterDescendants, then kRegIterContinue: reference_os9_nameregistry_iterate). Not by path: the FW800
   names fn0 and fn1 both "Devices:device-tree:pci:usb" (October's PMDriverList logs), as QEMU names its two OHCIs. The
   bus number Apple gives each controller (AAPL,BusNumber) and each device (locationID's top byte) is the cross-check. */
static short KwDiscover(KwDev *d, UInt32 *sig)
{
    short n = 0, nk = 0, i, j, g, fi; UInt32 kref[kKwMax]; UInt8 kif[kKwMax];
    *sig = 0x811C9DC5UL; gKwDup = gKwBusOff = 0;
    for (fi = 0; fi < gNecN && fi < kMaxNec; fi++) {
        RegEntryIter it; RegEntryID e; Boolean done = false, ok; RegEntryIterationOp op = kRegIterDescendants;
        unsigned long bus;
        gKwBus[fi] = 0xFFFFFFFFUL;
        if (!gNec[fi].ohci) continue;
        bus = RegU32(&gFn[gNec[fi].fi].id, "AAPL,BusNumber", &ok);
        if (ok) gKwBus[fi] = (UInt32)bus;
        if (RegistryEntryIterateCreate(&it) != noErr) continue;
        if (RegistryEntryIterateSet(&it, &gFn[gNec[fi].fi].id) == noErr)
            for (g = 0; g < 400; g++) {                                /* (bounded) */
                RegistryEntryIDInit(&e);
                if (RegistryEntryIterate(&it, op, &e, &done) != noErr || done) { RegistryEntryIDDispose(&e); break; }
                op = kRegIterContinue;
                KwNode(&e, fi, d, &n, kref, kif, &nk, sig);
                RegistryEntryIDDispose(&e);
            }
        RegistryEntryIterateDispose(&it);
    }
    for (j = 0; j < nk; j++) for (i = 0; i < n; i++) if (d[i].ref == kref[j]) { d[i].kbd = 1; d[i].kbdIf = kif[j]; }
    for (i = 0; i < n; i++) d[i].bt = (UInt8)(d[i].vid == 0x05ACU && d[i].pid == 0x1000U);
    for (i = 0; i < n; i++) d[i].bth = (UInt8)(d[i].cls == 0xE0U && !d[i].bt);
    for (i = 0; i < n; i++) {
        UInt32 p;
        if (!d[i].kbd || d[i].bt) continue;
        d[i].want = 1;
        for (p = d[i].parent, g = 0; g < 6 && p != 0; g++) {        /* each hub up to the root */
            for (j = 0; j < n; j++) if (d[j].ref == p) break;
            if (j == n || !d[j].hub) break;
            d[j].want = 1; p = d[j].parent;
        }
    }
    *sig ^= (UInt32)n;
    return n;
}
static void KwPass(UInt32 t)                  /* TASK LEVEL: each Notification Manager pass */
{
    if (!gInstalled || gKwReq == NULL || gKwStuck || gNecN == 0) return;
    if (gKwOn) {
        if (gKwQ.busy) {
            if (!gKwQ.done) {
                if (gKwQ.wake != gWakeN) { gKwQ.wake = gWakeN; gKwQ.t0 = t; }   /* sent before a sleep: 5 s from the wake */
                if (t - gKwQ.t0 > 5UL * 60UL) {
                    gKwStuck = true; gKwOn = false;
                    Lg("!! uwake11: a USB request (device %08lX, step %d) had no completion after 5 s: the survey stops for "
                       "this boot; the sleep keeps what the last finished survey decided (%lu finished)",
                       (unsigned long)gKw[gKwDev].ref, (int)gKwStep, (unsigned long)gKwSurveys);
                }
                return;
            }
            gKwQ.busy = false;
            KwTake();
        }
        if (gKwHold || gRecOn) {                 /* a sleep began, or a wake is being recorded: again later */
            gKwOn = false; gKwWant = true;
            Lg("  uwake11: the survey stopped part-way (%s): it runs again later",
               gKwHold ? "a sleep began" : "a wake is being recorded");
            return;
        }
        KwAdvance();
        return;
    }
    if (gKwHold || gRecOn || gWakeLogged != gWakeN || t - gBootTicks < 20UL * 60UL) return;
    if (!gKwWant && t - gKwCheck < 10UL * 60UL) return;
    gKwCheck = t;
    {
        UInt32 sig; short n = KwDiscover(gKwScan, &sig);
        if (!gKwWant && sig == gKwSig) return;    /* the same devices: nothing to ask */
        memcpy(gKw, gKwScan, sizeof gKw); gKwN = n; gKwSig = sig;
        gKwWant = false; gKwOn = true; gKwDev = 0; gKwStep = kKsAttr; gKwPort = 1;
        KwAdvance();
    }
}
/* uwake14: KwPass looks for USB devices coming and going every 10 s, but only while the task-level pass runs, and the
   pass now stops when it has nothing to do. So the put-away hook (task level, before every sleep) looks once more. If
   the devices changed since the last survey, the remote wakeup for THIS sleep follows the Name Registry alone (a
   function hosting a non-Bluetooth keyboard: Apple's stack arms every HID device and hub, uwake11's surveys on the
   FW800 and in QEMU), and the full survey runs again after the wake. Returns true when it decided. */
static Boolean KwHookCheck(void)               /* TASK LEVEL: the put-away hook */
{
    UInt32 sig; short n, i; Boolean fn[kMaxNec]; char b[96]; int k = 0;
    if (!gInstalled || gKwReq == NULL || gKwStuck || gNecN == 0 || gKwOn) return false;
    n = KwDiscover(gKwScan, &sig);
    if (sig == gKwSig) return false;
    for (i = 0; i < kMaxNec; i++) { fn[i] = false; gKwFnKbd[i] = false; gKwFnBt[i] = false; }
    for (i = 0; i < n; i++) {
        if (gKwScan[i].nec >= kMaxNec) continue;
        if (gKwScan[i].kbd && !gKwScan[i].bt) { fn[gKwScan[i].nec] = true; gKwFnKbd[gKwScan[i].nec] = true; }
        if (gKwScan[i].bth)                   { fn[gKwScan[i].nec] = true; gKwFnBt[gKwScan[i].nec]  = true; }
    }                                         /* uwake18A: a Bluetooth controller's function too; see KwFinish */
    b[0] = 0;
    for (i = 0; i < gNecN && i < kMaxNec && k < (int)sizeof b - 40; i++)
        if (gNec[i].ohci)
            k += snprintf(b + k, sizeof b - (size_t)k, "%s#%d fn%lu %s (was %s)", k ? ", " : "", (int)i,
                          (unsigned long)((gFn[gNec[i].fi].reg0 >> 8) & 7UL), fn[i] ? "ON" : "off", gKwFn[i] ? "on" : "off");
    Lg("  uwake14: the USB devices changed since the last survey (%d device(s) now, %lu survey(s) finished): remote wakeup "
       "for this sleep from the Name Registry alone: %s; the full survey runs again after the wake",
       (int)n, (unsigned long)gKwSurveys, b);
    memcpy(gKwFn, fn, sizeof gKwFn);
    gKwWant = true;
    return true;
}
static void KwInstall(void)                   /* TASK LEVEL (kInitialize) */
{
    CFragConnectionID conn = 0; Ptr mainAddr = NULL, sa = NULL; Str255 errName; CFragSymbolClass cls = 0; OSErr e;
    short i; RegPathNameSize sz;
    e = GetSharedLibrary("\pUSBServicesLib", kPowerPCCFragArch, kReferenceCFrag, &conn, &mainAddr, errName);
    if (e == noErr) e = FindSymbol(conn, "\pUSBDeviceRequest", &sa, &cls);
    gKwReq = (e == noErr && sa != NULL) ? (KwReqFn)sa : NULL;
    for (i = 0; i < gNecN && i < kMaxNec; i++) {
        UsbFn *f = &gFn[gNec[i].fi];
        gKwPath[i][0] = 0; sz = 0;
        if (RegistryEntryToPathSize(&f->id, &sz) == noErr && sz > 0 && sz < sizeof gKwPath[i] - 1 &&
            RegistryCStrEntryToPath(&f->id, gKwPath[i], sizeof gKwPath[i] - 1) == noErr) strcat(gKwPath[i], ":");
        else gKwPath[i][0] = 0;
    }
    Lg("  uwake11 keyboard wake: USBDeviceRequest %s (%d); NEC function #0's entry %s. The survey of Apple's USB tree "
       "(each NEC function's own subtree) runs ~20 s after boot, after each wake's report, and when a device comes or goes.",
       gKwReq ? "found" : "NOT FOUND: no keyboard wake (uwake10A's behaviour)", (int)e,
       gKwPath[0][0] ? gKwPath[0] : "(no path)");
}

/* ---- uwake11: the readings, decoded where they are printed ---- */
static void KwStBits(UInt16 s, char *b, int max)      /* GET_STATUS (device): bit 0 self-powered, bit 1 remote wakeup */
{
    if (s == 0xFFFFU) snprintf(b, (size_t)max, "not read");
    else snprintf(b, (size_t)max, "%04X [%sremote wakeup %s]", (unsigned)s, (s & 1U) ? "self-powered, " : "",
                  (s & 2U) ? "ON" : "off");
}
static void KwHubPort(UInt32 v, char *b, int max)     /* a hub port: wPortStatus | wPortChange << 16 (USB 2.0 11.24.2.7) */
{
    static const char *st[11] = { "connected", "enabled", "suspended", "over-current", "reset", 0, 0, 0, "power",
                                  "low-speed", "high-speed" };
    static const char *ch[5] = { "C_CONNECTION", "C_ENABLE", "C_SUSPEND", "C_OVER_CURRENT", "C_RESET" };
    int i, k;
    if (v == 0xFFFFFFFFUL) { snprintf(b, (size_t)max, "not read"); return; }
    k = snprintf(b, (size_t)max, "%04lX/%04lX [", v & 0xFFFFUL, v >> 16);
    for (i = 0; i < 11 && k < max - 20; i++) if (st[i] && (v & (1UL << i))) k += snprintf(b + k, (size_t)(max - k), "%s ", st[i]);
    for (i = 0; i < 5 && k < max - 20; i++) if (v & (1UL << (16 + i))) k += snprintf(b + k, (size_t)(max - k), "%s! ", ch[i]);
    if (k > 0 && b[k - 1] == ' ') k--;
    if (k < max - 2) { b[k++] = ']'; b[k] = 0; }
}
static const char *KwErr(SInt32 e)
{
    static char b[16];
    switch (e) {
    case kKwNone: return "not sent"; case noErr: return "ok"; case kUSBUnderRunErr: return "ok (short)";
    case kUSBUnknownDeviceErr: return "-6998 ref unknown"; case kUSBNotRespondingErr: return "-6911 not responding";
    case kUSBEndpointStallErr: return "-6912 STALL"; case kUSBPipeStalledError: return "-6979 pipe stalled";
    case kUSBTimedOut: return "-6971 timed out";
    default: snprintf(b, sizeof b, "%ld", (long)e); return b;
    }
}
static void KwLogSurvey(const KwDev *k, short n, const char *when)
{
    short i, p, j; char s0[48], sD[48], s1[48], pb[200];
    Lg("  uwake11 survey #%lu (%s, Ticks %lu): %d USB device(s) under the NEC (root hubs not listed); bus numbers: #0 %ld, "
       "#1 %ld; devices found twice %d, devices whose locationID bus is not their controller's %d",
       (unsigned long)gKwSurveys, when, (unsigned long)kLowTicks, (int)n, (long)(SInt32)gKwBus[0],
       (long)(SInt32)(gNecN > 1 ? gKwBus[1] : 0xFFFFFFFFUL), (int)gKwDup, (int)gKwBusOff);
    for (i = 0; i < n; i++) {
        const KwDev *d = &k[i]; const char *who;
        unsigned long fn = (gFn[gNec[d->nec < gNecN ? d->nec : 0].fi].reg0 >> 8) & 7UL;
        Boolean asked = (d->eSt0 != kKwNone || d->eAttr != kKwNone);
        KwStBits(d->st0, s0, sizeof s0); KwStBits(d->stD, sD, sizeof sD); KwStBits(d->st1, s1, sizeof s1);
        if (!asked) who = "not asked (no keyboard on this function)";
        else if (d->bt) who = "the Bluetooth module (HID proxy): left as Apple set it";
        else if (d->bth) who = (d->st0 != 0xFFFFU && (d->st0 & 2U))
                             ? "the Bluetooth controller: remote wakeup ON (allowed in the Bluetooth panel): it can wake the Mac"
                             : "the Bluetooth controller: remote wakeup off (not allowed in the Bluetooth panel)";
        else if (d->st1 != 0xFFFFU && (d->st1 & 2U)) who = "ARMED BY US";
        else if (d->eSet != kKwNone) who = "our arming FAILED";
        else if (d->stD == 0xFFFFU && d->st0 != 0xFFFFU && (d->st0 & 2U)) who = d->want ? "armed by Apple (kept)" :
                                                                            "armed by Apple: it can wake the Mac too";
        else if (d->want && !(d->attr & kUSBAtrRemoteWakeup)) who = "on the keyboard's path but CANNOT wake (bmAttributes)";
        else if (d->want) who = "on the keyboard's path, NOT armed";
        else who = "not armed (not on a keyboard's path)";
        Lg("    #%d fn%lu %s %04X:%04X loc %08lX ref %08lX%s: config bmAttributes %02X%s (%s); status %s%s%s%s%s => %s",
           (int)d->nec, fn, d->hub ? "hub" : "device", (unsigned)d->vid, (unsigned)d->pid, (unsigned long)d->loc,
           (unsigned long)d->ref, d->kbd ? " KEYBOARD" : "", (unsigned)d->attr,
           (d->attr & kUSBAtrRemoteWakeup) ? " [remote wakeup capable]" : d->attr ? " [no remote wakeup]" : "",
           KwErr(d->eAttr), s0, d->eD != kKwNone ? "; QEMU_DISARM cleared it: " : "", d->eD != kKwNone ? sD : "",
           d->eSet != kKwNone ? "; SET_FEATURE(DEVICE_REMOTE_WAKEUP) " : "",
           d->eSet != kKwNone ? KwErr(d->eSet) : "", who);
        if (d->eSet != kKwNone) Lg("      after our SET_FEATURE: status %s (%s)", s1, KwErr(d->eSt1));
        if (d->eIdle != kKwNone) Lg("      QEMU: SET_IDLE(0) on interface %u: %s", (unsigned)d->kbdIf, KwErr(d->eIdle));
        if (d->hub && d->want) {
            Lg("      hub: %d port(s) (descriptor %s); hub status %04lX/%04lX (%s)", (int)d->nPorts, KwErr(d->eDesc),
               d->hubSt & 0xFFFFUL, d->hubSt >> 16, KwErr(d->eHub));
            for (p = 0; p < d->nPorts && p < kKwPorts; p++) { KwHubPort(d->port[p], pb, sizeof pb); Lg("      port %d: %s", p + 1, pb); }
        }
    }
    for (j = 0, p = 0; j < gNecN && j < kMaxNec; j++) if (gKwFn[j]) p++;
    if (p == 0) Lg("    => remote wakeup at the next sleep demand: NONE (no keyboard path reads armed, no Bluetooth "
                   "controller): a key will not wake the Mac");
    else for (j = 0; j < gNecN && j < kMaxNec; j++)
        if (gKwFn[j]) Lg("    => remote wakeup at the next sleep demand: on #%d fn%lu (%s%s%s)",
                         (int)j, (gFn[gNec[j].fi].reg0 >> 8) & 7UL,
                         gKwFnKbd[j] ? "a keyboard and every hub above it read armed" : "",
                         (gKwFnKbd[j] && gKwFnBt[j]) ? "; and " : "",
                         gKwFnBt[j] ? "a Bluetooth controller is on it: its driver decides whether it may wake the Mac"
                                    : "");
}

/* The first task-level pass after a wake: the keyboard-wake facts written at once (the full report comes when the
   recorder ends, ~6 min later; if anything after the wake stops the Mac, these lines are already on disk). */
static void KwHms(unsigned long secs, char *b, int max)   /* TASK LEVEL: "15:44:27" */
{
    DateTimeRec dt;
    SecondsToDate(secs, &dt);
    snprintf(b, (size_t)max, "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
}
static void KwLogEarly(void)                  /* TASK LEVEL */
{
    short i; unsigned long now = 0; char t0[16], t1[16];
    /* uwake12: the sleep's start and this line's time, so a log of many cycles names each one, and a wake that came by
       itself at once shows as a short sleep */
    GetDateTime(&now);
    KwHms(gKwSlpTime, t0, sizeof t0); KwHms(now, t1, sizeof t1);
    Lg("  uwake12 wake %lu: the sleep began at %s (its request), this line at %s: %lu s apart (the sleep, plus a few "
       "seconds to the first task-level pass)", (unsigned long)gWakeN, gKwSlpTime ? t0 : "?", t1,
       (gKwSlpTime && now >= gKwSlpTime) ? (unsigned long)(now - gKwSlpTime) : 0UL);
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; char ib[80]; Boolean pme, rd;
        if (!n->ohci || !n->baseOK) continue;
        if (n->kwWkCtl == 0xFFFFFFFFUL) snprintf(ib, sizeof ib, "not read"); else IntBits(n->kwWkIst, ib, sizeof ib);
        pme = (n->wkPm != 0xFFFFUL && ((n->wkPm >> 15) & 1UL)); rd = (n->kwWkCtl != 0xFFFFFFFFUL && (n->kwWkIst & kIntRD));
        Lg("  uwake11 at once after wake %lu, #%d fn%lu: remote wakeup at the demand %s; at the wake PMCSR %04lX (PME_Status "
           "%s), HcControl %08lX (HCFS %s), InterruptStatus %08lX [%s] => %s", (unsigned long)gWakeN, (int)i,
           (gFn[n->fi].reg0 >> 8) & 7UL,
           n->kwWhy == 0 ? "SET" : n->kwWhy == 1 ? "not set (no armed keyboard or Bluetooth controller here)" :
           n->kwWhy == 2 ? "not set (not suspended)" :
           n->kwWhy == 3 ? "not set (no survey yet)" : n->kwWhy == 4 ? "NOT SET (the write did not take)" : "not set",
           n->wkPm, n->wkPm == 0xFFFFUL ? "not read" : pme ? "1" : "0", n->kwWkCtl, Hcfs(n->kwWkCtl), n->kwWkIst, ib,
           (pme || rd) ? "WOKEN BY USB on this function" : "not woken by USB on this function");
    }
    LogFlush();
}

#ifdef UWAKE_QTEST
/* QEMU: the bus held suspended (MIE masked, as no interrupt is taken while the FW800 sleeps) for up to 25 s, so a key
   (the harness sends Shift every 2 s) can signal remote wakeup. QEMU's OHCI then sets ResumeDetected and moves itself
   to Resume (hw/usb/hcd-ohci.c ohci_wakeup), only if the keyboard's DEVICE_REMOTE_WAKEUP is set: QEMU does not model
   RWE or PCI PME, so this checks the device side and the wake's record, not the NEC's. */
static void KwQHold(void)
{
    short i; long ms; Boolean seen = false;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        n->kwQMie = 0; n->kwQRdMs = -1;
        if (!n->ohci || !n->baseOK) continue;
        n->kwQMie = OhciRd(n->base, kHcInterruptEnable) & kIntMIE;
        if (n->kwQMie) OhciWr(n, kHcInterruptDisable, kIntMIE);
    }
    Lg("    QEMU: the bus held suspended (MIE masked) for up to 25 s: a key now should signal remote wakeup");
    LogFlush();
    for (ms = 0; ms < 25000L && !seen; ms += 50L) {
        DelayMs(50);
        for (i = 0; i < gNecN; i++) {
            NecFn *n = &gNec[i];
            if (!n->ohci || !n->baseOK || n->kwQRdMs >= 0) continue;
            if (OhciRd(n->base, kHcInterruptStatus) & kIntRD) { n->kwQRdMs = ms + 50L; seen = true; }
        }
    }
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; unsigned long c;
        if (!n->ohci || !n->baseOK) continue;
        c = OhciRd(n->base, kHcControl);
        if (n->kwQRdMs >= 0) Lg("    QEMU hold, #%d: REMOTE WAKEUP: ResumeDetected %ld ms into the hold; HcControl %08lX (HCFS %s)",
                                (int)i, n->kwQRdMs, c, Hcfs(c));
        else Lg("    QEMU hold, #%d: no ResumeDetected in %ld ms; HcControl %08lX (HCFS %s)", (int)i, ms, c, Hcfs(c));
    }
}
static void KwQUnmask(void)
{
    short i;
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        if (n->ohci && n->baseOK && n->kwQMie) { OhciWr(n, kHcInterruptEnable, kIntMIE); n->kwQMie = 0; }
    }
}
#endif
#endif /* UWAKE_MODE_A */

#ifdef UWAKE_MODE_A
static void LogUsbVols(void);              /* uwake20 (with the put-away hook, below) */
#endif
static void LogWake(void)                  /* TASK LEVEL: the wake that just finished recording */
{
    short i, k; UInt32 j, n = gRecN; char ts[32];
    if (n > kRecMax) n = kRecMax;          /* gRecN counts reservations */
    LgStamp(ts, sizeof ts);
    Lg("");
    Lg("=== WAKE %lu (report written %s, %lu ms after the wake's first step): what happened, then everything the "
       "recorder saw (ms after that step) ===", (unsigned long)gWakeN, ts, (unsigned long)RecMs());
#ifdef UWAKE_MODE_B
    Lg("  the sleep queue (Apple's PowerBook procedure, through ours): request x%lu (last answered %ld in %lu ms); "
       "demand = Apple's teardown x%lu (%ld, %lu ms); revoked x%lu; other messages x%lu (last %lu)",
       (unsigned long)gSq.reqN, (long)gSq.reqR, (unsigned long)gSq.reqMs, (unsigned long)gSq.demN, (long)gSq.demR,
       (unsigned long)gSq.demMs, (unsigned long)gSq.revN, (unsigned long)gSq.otherN, (unsigned long)gSq.otherLast);
    Lg("  the wake: our PCI config restore %lu ms, then Apple's reload (%s) returned %ld after %lu ms in all",
       (unsigned long)gSq.cfgMs, gSq.sysMode ? "in system mode" : "NOT in system mode", (long)gSq.wakeR,
       (unsigned long)gSq.wakeMs);
#endif
#ifdef UWAKE_MODE_A
    if (gKwSurveys == 0) Lg("  uwake11: no survey had finished before this sleep (remote wakeup was not set anywhere)");
    else KwLogSurvey(gKwLast, gKwLastN, "the last one before this sleep");
#endif
    for (i = 0; i < gNecN; i++) {
        NecFn *n2 = &gNec[i]; UsbFn *f = &gFn[n2->fi];
        Lg("  NEC fn %ld (%s, reg %08lX): PCI config %s: before cmd %04lX BAR0 %08lX PMCSR %04lX E4 %08lX -> after cmd %04lX BAR0 %08lX PMCSR %04lX E4 %08lX",
           (f->reg0 >> 8) & 7UL, n2->ohci ? "OHCI" : "not OHCI", f->reg0, CfgOut(n2->outcome),
           n2->wCmd, n2->wBar0, n2->wPMCSR, n2->wE4, n2->aCmd, n2->aBar0, n2->aPMCSR, n2->aE4);
        if (n2->ehci) {                    /* uwake19: the EHCI across this sleep; uwake20: its suspend and resume */
            if (n2->ehBase == 0)
                Lg("    EHCI: registers not read (AAPL,address is not BAR0): nothing done to it");
            else if (n2->ehWhy == 0 || n2->ehWhy == 3)
                LogEhciCycle(n2);
            else {
                LogEhci("at the sleep demand", &n2->ehDem);
                LogEhci("after the wake, its PCI config restored", &n2->ehWake);
                if (n2->ehDem.ok && EhciRunning(&n2->ehDem))
                    Lg("    uwake19 verdict: !! the EHCI routed ports to itself at the sleep demand but was not running "
                       "(Run/Stop 0, CONFIGFLAG 1: its driver had stopped it), so it was left as it was");
                else if (!n2->ehWake.ok)
                    Lg("    uwake19 verdict: !! the EHCI could not be read after the wake");
                else if (EhciRunning(&n2->ehWake))
                    Lg("    uwake19 verdict: !! the EHCI is RUNNING or routing ports to itself after the wake, and nothing "
                       "here started it");
                else
                    /* CONFIGFLAG 0 routes every port to the companions; the spec then forces PORT_OWNER to 1, so a clear
                       one is reported, not judged (QEMU's standalone usb-ehci, which has no companions, reads 0). */
                    Lg("    uwake19 verdict: the NEC's USB 2.0 half came back stopped with CONFIGFLAG 0, every port routed to "
                       "the OHCIs: the EHCI ROM did not change what Apple's stack runs across the sleep%s",
                       EhciPortsCompanion(&n2->ehWake) ? "" : " (note: PORT_OWNER read 0 on a port; the spec says 1)");
            }
        }
        if (!n2->ohci) continue;
        Lg("    recorder: InterruptStatus changed %lu times, HcDoneHead %lu times (the first %d of each are listed)%s",
           n2->rec.istN, n2->rec.doneN, kChurnLog, n2->rec.ueSeen ? "; UE WAS SEEN PENDING" : "");
#ifdef UWAKE_MODE_A
        /* uwake12: this wake's own sleep (kept at the wake), not whatever the next sleep's request has put there since */
        if (n2->wReq.ok) LogRegs("the sleep request's snapshot (the restore source)", &n2->wReq);
        if (n2->wSlpOK) { char ib[80], eb[80]; IntBits(n2->wSlpIst, ib, sizeof ib); IntBits(n2->wSlpIen, eb, sizeof eb);
            Lg("    the sleep demand, after Apple's level 1 (seen by our %d handler): HcControl %08lX (HCFS %s), InterruptStatus %08lX [%s], InterruptEnable %08lX [%s]",
               n2->wSlpBy, n2->wSlpCtl, Hcfs(n2->wSlpCtl), n2->wSlpIst, ib, n2->wSlpIen, eb); }
        else Lg("    the sleep demand: neither of our handlers saw Apple's level 1 (no Suspend): the enable falls back to the request's mask");
        /* uwake10: the arming, what the NEC said at the wake, and the verdict */
        if (n2->armWhy == 0)
            Lg("    uwake10 arming at the demand: PMCSR %04lX -> %04lX (PME_Status cleared, PME_Enable, D3hot)", n2->armPm0,
               n2->armPm1);
        else Lg("    uwake10 arming at the demand: NOT armed (%s); PMCSR %04lX", n2->armWhy == 1 ? "no PCI PM capability" :
                n2->armWhy == 2 ? "Apple had not suspended it" : "no sleep demand reached our handler", n2->armPm0);
        if (n2->wkPm != 0xFFFFUL)
            Lg("    uwake10 at the wake, before our D0 write: PMCSR %04lX = D%lu, PME_Enable %lu, PME_Status %lu%s", n2->wkPm,
               n2->wkPm & 3UL, (n2->wkPm >> 8) & 1UL, (n2->wkPm >> 15) & 1UL,
               ((n2->wkPm >> 15) & 1UL) ? " (this function signalled the wake)" : "");
        /* (uwake10A on the FW800 showed the controller kept while the PCI command register alone was reset: the verdict
           follows the CONTROLLER; the config is reported apart) */
        Lg("    uwake10 VERDICT: %s%s", n2->ohciOutcome == kOhciIntact ?
           "it KEPT its controller state through the sleep (its HCCA as before the sleep; HCFS: see the wake line): Apple's level 0 resumes it, as on "
           "KeyLargo" : n2->ohciOutcome == kOhciRestored ?
           "it LOST its controller state (we restored it, as uwake1 mode A would): its devices were powered down" :
           "not judged (see the OHCI line)",
           n2->outcome == kOutRestored ? "; its PCI config needed restoring (see the PCI config line)" :
           n2->outcome == kOutIntact ? "; its PCI config was intact" : "");
        /* uwake11: keyboard wake, at the demand, at the wake, at the enable */
        if (n2->kwWhy == 0 || n2->kwWhy == 4) {
            char ib[80], p1[80], p2[80];
            IntBits(n2->kwIst0, ib, sizeof ib);
            Lg("    uwake11 at the demand: remote wakeup %s: HcControl %08lX -> %08lX (HCFS %s, RWC %lu, RWE %lu) after %d "
               "write(s); HcRhStatus %08lX -> %08lX (DRWE %lu -> %lu); InterruptStatus %08lX [%s]%s",
               n2->kwWhy == 0 ? "SET" : "NOT SET (the write did not take)", n2->kwCtl0, n2->kwCtl1, Hcfs(n2->kwCtl1),
               (n2->kwCtl1 >> 9) & 1UL, (n2->kwCtl1 >> 10) & 1UL, (int)n2->kwTries, n2->kwRh0, n2->kwRh1,
               (n2->kwRh0 >> 15) & 1UL, (n2->kwRh1 >> 15) & 1UL, n2->kwIst0, ib,
               n2->kwRdClr ? " (a stale RD: cleared)" : "");
            PortBits(n2->kwP0[0], p1, sizeof p1); PortBits(n2->kwP0[1], p2, sizeof p2);
            Lg("      root ports at the demand: 1 %08lX [%s]  2 %08lX [%s]", n2->kwP0[0], p1, n2->kwP0[1], p2);
            if (n2->kwP0[2] != 0xFFFFFFFFUL) { PortBits(n2->kwP0[2], p1, sizeof p1);
                Lg("      root port 3 at the demand: %08lX [%s]", n2->kwP0[2], p1); }
        } else Lg("    uwake11 at the demand: remote wakeup not set (%s)", n2->kwWhy == 1 ?
                  "no armed keyboard or Bluetooth controller on this function" :
                  n2->kwWhy == 2 ? "Apple had not suspended it" : n2->kwWhy == 3 ? "no survey had finished" : "not reached");
        if (n2->kwWkCtl != 0xFFFFFFFFUL) {
            char ib[80], p1[80], p2[80];
            IntBits(n2->kwWkIst, ib, sizeof ib);
            Lg("    uwake11 at the wake, before Apple's level 0: HcControl %08lX (HCFS %s, RWE %lu), InterruptStatus %08lX [%s], "
               "HcRhStatus %08lX (DRWE %lu)", n2->kwWkCtl, Hcfs(n2->kwWkCtl), (n2->kwWkCtl >> 10) & 1UL, n2->kwWkIst, ib,
               n2->kwWkRh, (n2->kwWkRh >> 15) & 1UL);
            PortBits(n2->kwWkP[0], p1, sizeof p1); PortBits(n2->kwWkP[1], p2, sizeof p2);
            Lg("      root ports at the wake: 1 %08lX [%s]  2 %08lX [%s]", n2->kwWkP[0], p1, n2->kwWkP[1], p2);
            if (n2->kwWkP[2] != 0xFFFFFFFFUL) { PortBits(n2->kwWkP[2], p1, sizeof p1);
                Lg("      root port 3 at the wake: %08lX [%s]", n2->kwWkP[2], p1); }
            if (n2->kwWhy == 0) {
                Boolean pme = (n2->wkPm != 0xFFFFUL && ((n2->wkPm >> 15) & 1UL)), rd = (n2->kwWkIst & kIntRD) != 0;
                Lg("    uwake11 VERDICT: %s (PME_Status %s, ResumeDetected %lu, HCFS %s at the wake)",
                   (pme || rd) ? "WOKEN BY USB on this function: a key, or another device armed on it" :
                   "NOT woken by USB on this function: the power button, or another source",
                   n2->wkPm == 0xFFFFUL ? "not read" : pme ? "1" : "0", rd ? 1UL : 0UL, Hcfs(n2->kwWkCtl));
            }
        }
        if (n2->kwEnRh != 0xFFFFFFFFUL)
            Lg("    uwake11 at the enable: DRWE put back as Apple had it: HcRhStatus %08lX (DRWE %lu)", n2->kwEnRh,
               (n2->kwEnRh >> 15) & 1UL);
        LogHcd("at the sleep demand", n2->hcdSlp);
        LogHcd("at the wake, before the restore", n2->hcdWake);
        Lg("    OHCI: %s (snapshot: %s; found at wake: rev %08lX, HcControl %08lX, HCCA %08lX; HCR polls %d); HcFmNumber wrote %04lX, read back %04lX",
           OhciOut(n2->ohciOutcome), n2->usedReq ? "the sleep request's" : "the boot one's", n2->oRev, n2->oCtl, n2->oHCCA,
           n2->hcrPolls, n2->fmWrote & 0xFFFFUL, n2->fmBack & 0xFFFFUL);
        if (n2->ohciOutcome == kOhciRestored || n2->ohciOutcome == kOhciVerifyFail) LogRegs("after the restore", &n2->after);
        { char ib[80], eb[80]; IntBits(n2->enIst, ib, sizeof ib); IntBits(n2->enIen, eb, sizeof eb);
          Lg("    the enable found (Apple's level 0 had run): rev %08lX, HcControl %08lX (HCFS %s), InterruptStatus %08lX [%s], InterruptEnable %08lX [%s]",
             n2->enRev, n2->enCtl, Hcfs(n2->enCtl), n2->enIst, ib, n2->enIen, eb); }
        LogHcd("at the enable", n2->hcdEn);
        { char eb[80]; IntBits(n2->enMask, eb, sizeof eb);
          Lg("    the enable: %s%s; mask %08lX [%s]; UE put back after the recorder: %s", EnOut(n2->enOutcome),
             n2->enForcedOp ? " (HcControl was NOT Operational: we wrote it)" : "", n2->enMask, eb,
             n2->ueBack ? "yes" : "no"); }
#endif
    }
    Lg("  enable at +%lu ms; recorder: %lu passes, %lu events (%lu lost), timer error %ld; File Manager: %lu calls, %lu held 250 ms+",
       (unsigned long)gEnableMs, (unsigned long)gRecPasses, (unsigned long)n, (unsigned long)gRecLost, (long)gRecArmErr,
       (unsigned long)gFsqCalls, (unsigned long)gFsqSlowN);
    Lg("  input after the wake: the cursor moved %lu times (first at +%lu ms), the keyboard changed %lu times (first at +%lu ms)",
       (unsigned long)gMouseN, (unsigned long)gMouse1Ms, (unsigned long)gKeyN, (unsigned long)gKey1Ms);
#ifdef UWAKE_MODE_A
    LogUsbVols();                          /* uwake20: still mounted after the wake? (the sleepnet lines name them before) */
#endif
    Lg("  uwake5 readings: retired-TD events %lu (cap %d), list-register changes %lu (cap %d), raw control EDs %lu (cap %d)%s",
       (unsigned long)gTdN, kTdLog, (unsigned long)gRegN, kRegLog, (unsigned long)gEdRawN, kEdRawLog,
#ifdef UWAKE_QTEST_EDVM
       gRamReadable ? "" : " -- virtual memory is on: read anyway (QEMU check build)");
#else
       gRamReadable ? "" : " -- TDs and EDs not read: virtual memory is on");
#endif
    {   /* uwake8/9: the address-0 capture, the failed adds, the early power, fn1's recovery, the recorder's restarts */
        short a;
        Lg("  uwake8 readings: address-0 transfer events %lu (cap %d)", (unsigned long)gA0N, kA0Log);
        for (a = 0; a < 2; a++) {
            if (gDeadN[a])
                Lg("  !! fn%d: %lu add(s) built a pipe with maximum packet size 0 (a failed add); the first: FA %lu at +%lu ms "
                   "(ED w0 %08lX)", a, (unsigned long)gDeadN[a], (unsigned long)gDead1FA[a], (unsigned long)gDead1Ms[a],
                   (unsigned long)gDead1W0[a]);
            else Lg("    fn%d: no add built a pipe with maximum packet size 0", a);
        }
#ifdef UWAKE_MODE_B
        if (gEarlyWhy == 0)
            Lg("  uwake9 early power: fn1's root-hub power ON at +%lu ms, as Apple's reload returned (%lu write(s)%s); port 1 "
               "then %08lX", (unsigned long)gEarlyMs, (unsigned long)(gEarlyWrites > 10 ? 10 : gEarlyWrites),
               gEarlyWrites > 10 ? ", NEVER read back" : "", (unsigned long)gEarlyPs);
        else Lg("  uwake9 early power: not done (%s)", gEarlyWhy == 8 ? "it did not run" : RcvWhy(gEarlyWhy));
        Lg("  uwake9 recovery (task level; after a failed add on fn1, its root-hub power off for %lu ms, then on): done %lu "
           "time(s)%s%s", (unsigned long)kRcvOffMs, (unsigned long)gRcvN, gRcvSkip ? "; once due but not done: " : "",
           gRcvSkip ? RcvWhy(gRcvSkip) : "");
#endif
        Lg("  uwake9 recorder: restarted %lu time(s) after it had stopped%s", (unsigned long)gRearmN,
           gRearmN ? " (the \"recorder had stopped\" lines, above and in the list, say where)" : "");
    }
    {   /* uwake6: the translations the recorder read through, and the blocks refused */
        static UInt32 sl[kWinMax], sp[kWinMax]; UInt32 m = gWinN, a, b2, ident = 0;
        if (m > kWinMax) m = kWinMax;
        for (a = 0; a < m; a++) { sl[a] = gWinL[a]; sp[a] = gWinP[a]; if (sl[a] == sp[a]) ident++; }
        for (a = 1; a < m; a++) {                       /* by logical address */
            UInt32 vl = sl[a], vp = sp[a]; b2 = a;
            while (b2 > 0 && sl[b2 - 1] > vl) { sl[b2] = sl[b2 - 1]; sp[b2] = sp[b2 - 1]; b2--; }
            sl[b2] = vl; sp[b2] = vp;
        }
        Lg("  uwake6 memory: %lu blocks of 64 KB translated (GetPhysical; %lu of them logical == physical), %lu refused; "
           "%lu reads skipped for want of a translation", (unsigned long)m, (unsigned long)ident, (unsigned long)gRejN,
           (unsigned long)gUnreadN);
        for (a = 0; a < m; ) {                          /* runs of blocks with one offset */
            b2 = a;
            while (b2 + 1 < m && sl[b2 + 1] == sl[b2] + kWinBytes && sp[b2 + 1] - sl[b2 + 1] == sp[a] - sl[a]) b2++;
            Lg("    logical %08lX-%08lX -> physical %08lX-%08lX%s", (unsigned long)sl[a],
               (unsigned long)(sl[b2] + kWinBytes - 1UL), (unsigned long)sp[a], (unsigned long)(sp[b2] + kWinBytes - 1UL),
               sp[a] == sl[a] ? " (the same)" : "");
            a = b2 + 1;
        }
        for (a = 0; a < gRejN && a < 24; a++)
            Lg("    refused: logical block %08lX (%s %ld; first physical run %08lX)", (unsigned long)gRej[a],
               gRejE[a] == 0 ? "not one run of 64 KB: GetPhysical" : "error", (long)gRejE[a], (unsigned long)gRejP[a]);
    }
    {   /* uwake7: was the memory map right where the recorder read through it? (MapCheck) */
        UInt32 a;
        if (gRunN == 0 && gRemapN == 0) Lg("  uwake7 map check: no memory map (see the boot section)");
        else {
            Lg("  uwake7 map check: %lu blocks of 64 KB translated again after the wake (those read through the map, and "
               "those searched): %lu pages of 4 KB agree with the map, %lu DIFFER, %lu outside it",
               (unsigned long)gMapBlocks, (unsigned long)gMapPgSame, (unsigned long)gMapPgDiff, (unsigned long)gMapPgNone);
            if (gMapPgDiff == 0 && gMapPgSame > 0)
                Lg("    the map held: the readings through it read what the controllers pointed at");
            if (gMapPgDiff) {
                Lg("  !! the memory map was WRONG in %lu block(s) of 64 KB (the first %d below; \"READ\" = the recorder had "
                   "read through it).", (unsigned long)gMapDiffBlocks, kDiffMax);
                Lg("  !! Every reading made before +%lu ms (when the first was found) may have read other memory: DISCARD "
                   "them. Later readings came from translations made after the wake, or from the map taken again.",
                   (unsigned long)gDiffMs[0]);
                for (a = 0; a < gMapDiffBlocks && a < kDiffMax; a++)
                    Lg("    +%lu ms: block %08lX (%s): %lu of its pages wrong; the first, logical %08lX: the map said physical "
                       "%08lX, after the wake %08lX", (unsigned long)gDiffMs[a], (unsigned long)(gDiffL[a] & ~(kWinBytes - 1UL)),
                       gDiffUsed[a] ? "READ" : "searched", (unsigned long)gDiffPg[a], (unsigned long)gDiffL[a],
                       (unsigned long)gDiffM[a], (unsigned long)gDiffF[a]);
            }
            if (gRemapN) Lg("  the map was taken again %lu time(s) after a wrong page (the runs are listed above, where it "
                            "happened)%s", (unsigned long)gRemapN,
                            gMapOff ? "; it was wrong again and stayed OFF: only blocks translated after the wake were read" : "");
        }
    }
    if (gUslD) {
        short a, nrec = 0;
        Lg("  USL add-lock: %lu changes this wake (the first %d listed); the adds' references pointed at %d address(es)"
           " (QEMU: the PARENT hub's, not the new device's)", (unsigned long)gUslN, kUslLog, gUslAddrN);
        for (a = 0; a < 128; a++) {                 /* every device Apple's USB layer knows now, by USB address */
            unsigned long rec = gUslD + U_DEVREC + (unsigned long)a * U_DEVRECLEN;
            if (InRAM(rec, U_DEVRECLEN) && RdL(rec) != 0) {
                volatile UInt8 *b = (volatile UInt8 *)rec; unsigned long w, q;
                Lg("    device at address %d: ref %08lX, VID %04X PID %04X", a, (unsigned long)RdL(rec),
                   (unsigned)(b[102] | (b[103] << 8)), (unsigned)(b[104] | (b[105] << 8)));
                for (w = 0; w < U_DEVRECLEN; w += 32) {    /* uwake5: the whole record (a failed add left VID 0) */
                    char hx[100]; int o = 0;
                    hx[0] = 0;
                    for (q = w; q < w + 32UL && q < U_DEVRECLEN; q += 4UL)
                        o += snprintf(hx + o, sizeof hx - (size_t)o, " %08lX", (unsigned long)RdL(rec + q));
                    Lg("      +%3lu:%s", w, hx);
                }
                nrec++;
            }
        }
        Lg("    (%d device records; the root hubs are 05AC:8005)", nrec);
    } else Lg("  USL add-lock: not read (see the boot section)");
    Lg("  control EDs: %lu changes (the first %d listed)%s", (unsigned long)gEdN, kEdLog,
       gRamReadable ? "" : " -- not read: virtual memory is on");
    for (j = 0; j < n; j++) {
        const RecEv *e = &gRec[j]; char b[96];
        k = e->fn;
        switch (e->kind) {
        case kEvCtl:  Lg("  %7lu  fn%d HcControl %08lX = HCFS %s%s%s%s%s", (unsigned long)e->ms, k, (unsigned long)e->val, Hcfs(e->val),
                         (e->val & 4) ? " PLE" : "", (e->val & 8) ? " IE" : "", (e->val & 0x10) ? " CLE" : "", (e->val & 0x20) ? " BLE" : ""); break;
        case kEvIst:  IntBits(e->val, b, sizeof b); Lg("  %7lu  fn%d InterruptStatus %08lX [%s]", (unsigned long)e->ms, k, (unsigned long)e->val, b); break;
        case kEvIen:  IntBits(e->val, b, sizeof b); Lg("  %7lu  fn%d InterruptEnable %08lX [%s]", (unsigned long)e->ms, k, (unsigned long)e->val, b); break;
        case kEvDone: Lg("  %7lu  fn%d HcDoneHead %08lX", (unsigned long)e->ms, k, (unsigned long)e->val); break;
        case kEvPort: PortBits(e->val, b, sizeof b); Lg("  %7lu  fn%d port %d %08lX [%s]", (unsigned long)e->ms, k, e->aux, (unsigned long)e->val, b); break;
        case kEvFmStop: Lg("  %7lu  fn%d frame counter STOPPED at %04lX", (unsigned long)e->ms, k, (unsigned long)e->val); break;
        case kEvFmRun:  Lg("  %7lu  fn%d frame counter running again (%04lX)", (unsigned long)e->ms, k, (unsigned long)e->val); break;
        case kEvFsqSlow: Lg("  %7lu  File Manager: a call held 250 ms (ioTrap %04X, pb %08lX)", (unsigned long)e->ms, e->aux, (unsigned long)e->val); break;
        case kEvFsqStuck: Lg("  %7lu  File Manager: the SAME call held 2 s (pb %08lX) -- the lone-Slave signature", (unsigned long)e->ms, (unsigned long)e->val); break;
        case kEvFsqDone: Lg("  %7lu  File Manager: that call finished after %lu ms", (unsigned long)e->ms, (unsigned long)e->val); break;
        case kEvTicksStop: Lg("  %7lu  Ticks STOPPED (the 60 Hz tick has not moved for 1 s)", (unsigned long)e->ms); break;
        case kEvTicksRun:  Lg("  %7lu  Ticks moving again", (unsigned long)e->ms); break;
        case kEvEnable: Lg("  %7lu  ENABLE ran (message %u)", (unsigned long)e->ms, e->aux); break;
        case kEvRestore: Lg("  %7lu  WAKE STEP 1 ran (message %u), wake %lu", (unsigned long)e->ms, e->aux, (unsigned long)e->val); break;
        case kEvReload: Lg("  %7lu  Apple's reload RETURNED %d after %lu ms", (unsigned long)e->ms, (int)(SInt16)e->aux, (unsigned long)e->val); break;
        case kEvPhase: Lg("  %7lu  recorder: now every %u ms", (unsigned long)e->ms, e->aux); break;
        case kEvEnd:   Lg("  %7lu  recorder: END (%u s)", (unsigned long)e->ms, e->aux); break;
        case kEvTd: {
            UInt32 w0 = 0, cbp = 0, be = 0; Boolean got = false;
            if (j + 3 < n && gRec[j + 1].kind == kEvTdW0 && gRec[j + 2].kind == kEvTdCbp && gRec[j + 3].kind == kEvTdBe) {
                w0 = gRec[j + 1].val; cbp = gRec[j + 2].val; be = gRec[j + 3].val; got = true; j += 3;
            }
            if (got) Lg("  %7lu  fn%d   retired TD #%u at %08lX: CC %lX (%s), EC %lu, toggle %lu, DI %lu, %s, CBP %08lX, BE %08lX",
                        (unsigned long)e->ms, k, e->aux, (unsigned long)e->val, (unsigned long)((w0 >> 28) & 0xFUL),
                        CcName(w0 >> 28), (unsigned long)((w0 >> 26) & 3UL), (unsigned long)((w0 >> 24) & 3UL),
                        (unsigned long)((w0 >> 21) & 7UL),
                        ((w0 >> 19) & 3UL) == 0 ? "SETUP" : ((w0 >> 19) & 3UL) == 1 ? "OUT" : ((w0 >> 19) & 3UL) == 2 ? "IN" : "DP?",
                        (unsigned long)cbp, (unsigned long)be);
            else Lg("  %7lu  fn%d   retired TD #%u at %08lX (its words not recorded)", (unsigned long)e->ms, k, e->aux,
                    (unsigned long)e->val);
            } break;
        case kEvTdW0: case kEvTdCbp: case kEvTdBe:
            Lg("  %7lu  fn%d   (a TD word, kind %u: %08lX)", (unsigned long)e->ms, k, e->kind, (unsigned long)e->val); break;
        case kEvTdBad: Lg("  %7lu  fn%d   retired TD #%u: pointer %08lX OUTSIDE RAM", (unsigned long)e->ms, k, e->aux,
                          (unsigned long)e->val); break;
        case kEvTdUnread: Lg("  %7lu  fn%d   retired TD #%u at %08lX: NOT READ (no translation covers it yet)",
                             (unsigned long)e->ms, k, e->aux, (unsigned long)e->val); break;
        case kEvMapStale:
            if (e->aux < kDiffMax)
                Lg("  %7lu  !! the memory map is WRONG at logical %08lX (it said physical %08lX; now %08lX; a block %s): "
                   "no more reads through it", (unsigned long)e->ms, (unsigned long)gDiffL[e->aux],
                   (unsigned long)gDiffM[e->aux], (unsigned long)gDiffF[e->aux],
                   gDiffUsed[e->aux] ? "the recorder had READ through" : "searched");
            else Lg("  %7lu  !! the memory map is WRONG at logical %08lX: no more reads through it", (unsigned long)e->ms,
                    (unsigned long)e->val);
            break;
        case kEvA0Td: case kEvA0TdDone: {                /* uwake8: an address-0 transfer's TD, queued or afterwards */
            UInt32 w0 = 0, cbp = 0, be = 0; Boolean got = false;
            if (j + 3 < n && gRec[j + 1].kind == kEvTdW0 && gRec[j + 2].kind == kEvTdCbp && gRec[j + 3].kind == kEvTdBe) {
                w0 = gRec[j + 1].val; cbp = gRec[j + 2].val; be = gRec[j + 3].val; got = true; j += 3;
            }
            if (got) Lg("  %7lu  fn%d   address-0 TD #%u at %08lX %s: CC %lX (%s), EC %lu, toggle %lu, DI %lu, %s, CBP %08lX, "
                        "BE %08lX", (unsigned long)e->ms, k, e->aux, (unsigned long)e->val,
                        e->kind == kEvA0Td ? "queued" : "AFTERWARDS", (unsigned long)((w0 >> 28) & 0xFUL), CcName(w0 >> 28),
                        (unsigned long)((w0 >> 26) & 3UL), (unsigned long)((w0 >> 24) & 3UL), (unsigned long)((w0 >> 21) & 7UL),
                        ((w0 >> 19) & 3UL) == 0 ? "SETUP" : ((w0 >> 19) & 3UL) == 1 ? "OUT" : ((w0 >> 19) & 3UL) == 2 ? "IN" : "DP?",
                        (unsigned long)cbp, (unsigned long)be);
            else Lg("  %7lu  fn%d   address-0 TD #%u at %08lX (its words not recorded)", (unsigned long)e->ms, k, e->aux,
                    (unsigned long)e->val);
            } break;
        case kEvA0Buf:
            if (e->aux & 0x80) Lg("  %7lu  fn%d     an address-0 buffer at %08lX: NOT READ (no translation)", (unsigned long)e->ms,
                                  k, (unsigned long)e->val);
            else if (!(e->aux & 0x10) && j + 1 < n && gRec[j + 1].kind == kEvA0Buf && gRec[j + 1].aux == (e->aux | 0x10)) {
                LogA0Buf(e->ms, k, e->aux & 0x0FU, e->val, gRec[j + 1].val); j++;
            } else Lg("  %7lu  fn%d     (half an address-0 buffer: %08lX)", (unsigned long)e->ms, k, (unsigned long)e->val);
            break;
        case kEvA0End: Lg("  %7lu  fn%d   the address-0 transfer has left the ED (%u TD(s) followed; first seen %lu ms before)",
                          (unsigned long)e->ms, k, e->aux, (unsigned long)e->val); break;
        case kEvDead: Lg("  %7lu  !! fn%d DEAD PIPE: the add built FA %u's pipe with maximum packet size 0 (ED w0 %08lX)",
                         (unsigned long)e->ms, k, e->aux, (unsigned long)e->val); break;
        case kEvDeadGone: Lg("  %7lu  fn%d: the dead pipe (FA %u) has left the list (Apple removed that device)", (unsigned long)e->ms,
                             k, e->aux); break;
        case kEvRcv: {
            unsigned step = e->aux & 0xFFU, tries = e->aux >> 8; char pb[64];
            if (step >= 1 && step <= 3) {
                PortBits(e->val, pb, sizeof pb);
                Lg("  %7lu  %s: fn%d's root-hub power %s (the write took %u attempt(s)%s); port 1 now %08lX [%s]",
                   (unsigned long)e->ms, step == 3 ? "EARLY POWER" : "RECOVERY", k, step == 1 ? "OFF" : "ON",
                   tries > 10 ? 10 : tries, tries > 10 ? ", NEVER read back" : "", (unsigned long)e->val, pb);
            } else Lg("  %7lu  fn%d's power not switched: %s (HcRhDescriptorA %08lX)", (unsigned long)e->ms, k,
                      RcvWhy(step - 8U), (unsigned long)e->val);
            } break;
        case kEvRecStall: Lg("  %7lu  !! the recorder had stopped: its last pass began at +%lu ms and reached step %02X; "
                             "restarted", (unsigned long)e->ms, (unsigned long)e->val, e->aux); break;
        case kEvMapRemap: Lg("  %7lu  the memory map taken again (#%u): %lu runs%s", (unsigned long)e->ms, e->aux,
                             (unsigned long)e->val, e->val ? "; reads through it resume" : ": none, reads stay off"); break;
        case kEvEdUnread: Lg("  %7lu  fn%d   (that control ED #%u is at %08lX)", (unsigned long)e->ms, k, e->aux,
                             (unsigned long)e->val); break;
        case kEvReg: {
            const char *rn = e->aux == 0x08 ? "HcCommandStatus" : e->aux == 0x18 ? "HcHCCA" : e->aux == 0x20 ? "HcControlHeadED" :
                             e->aux == 0x24 ? "HcControlCurrentED" : e->aux == 0x28 ? "HcBulkHeadED" : e->aux == 0x2C ? "HcBulkCurrentED" : "?";
            if (e->aux == 0x08)
                Lg("  %7lu  fn%d %s %08lX [%s%s%s%s]", (unsigned long)e->ms, k, rn, (unsigned long)e->val,
                   (e->val & 1UL) ? "HCR " : "", (e->val & 2UL) ? "CLF " : "", (e->val & 4UL) ? "BLF " : "",
                   (e->val & 8UL) ? "OCR " : "");
            else Lg("  %7lu  fn%d %s %08lX", (unsigned long)e->ms, k, rn, (unsigned long)e->val);
            } break;
        case kEvEdRaw: {
            UInt32 wv[4]; short q; Boolean got = true;
            for (q = 0; q < 4; q++) {
                if (j + 1 + (UInt32)q < n && gRec[j + 1 + q].kind == kEvEdW && (gRec[j + 1 + q].aux & 0xFF) == (e->aux & 0xFF))
                    wv[q] = gRec[j + 1 + q].val;
                else got = false;
            }
            if (got) { Lg("  %7lu  fn%d control ED #%u at %08lX, raw: w0 %08lX TailP %08lX HeadP %08lX NextED %08lX",
                          (unsigned long)e->ms, k, e->aux, (unsigned long)e->val, (unsigned long)wv[0], (unsigned long)wv[1],
                          (unsigned long)wv[2], (unsigned long)wv[3]); j += 4; }
            else Lg("  %7lu  fn%d control ED #%u at %08lX (raw words not recorded)", (unsigned long)e->ms, k, e->aux,
                    (unsigned long)e->val);
            } break;
        case kEvEdW: Lg("  %7lu  fn%d   (an ED word: %08lX)", (unsigned long)e->ms, k, (unsigned long)e->val); break;
        case kEvClock: {
            DateTimeRec dt; SecondsToDate((unsigned long)e->val, &dt);
            Lg("  %7lu  the wall clock (network time) ticked to %02d:%02d:%02d%s", (unsigned long)e->ms, dt.hour, dt.minute,
               dt.second, e->aux == 0 ? " (its first tick after the wake step)" : "");
            } break;
        case kEvSleepStop: Lg("  %7lu  recorder: STOPPED by the next sleep's %s (before the NEC powered down)",
                              (unsigned long)e->ms, e->aux ? "demand" : "request"); break;
        case kEvMouse: Lg("  %7lu  the cursor moved: v %d h %d", (unsigned long)e->ms, (int)(SInt16)(e->val >> 16), (int)(SInt16)(e->val & 0xFFFFUL)); break;
        case kEvKeys:  Lg("  %7lu  the keyboard changed (KeyMap OR %08lX)", (unsigned long)e->ms, (unsigned long)e->val); break;
        case kEvUsl:   Lg("  %7lu  USL add-lock %s, add token %s, force %s, queue %s; address field %u, start frame %lu",
                          (unsigned long)e->ms, (e->aux & 1) ? "HELD" : "free", (e->aux & 8) ? "set" : "0",
                          (e->aux & 4) ? "SET" : "0", (e->aux & 2) ? "PENDING" : "empty", (unsigned)(e->aux >> 8),
                          (unsigned long)e->val); break;
        case kEvUslRef: Lg("  %7lu  USL: the add's device reference -> %08lX (assigning address %u)", (unsigned long)e->ms,
                           (unsigned long)e->val, e->aux); break;
        case kEvEd:    LogEd(e->ms, k, (short)e->aux, e->val); break;
        case kEvTaskFirst: Lg("  %7lu  TASK LEVEL running (the first Notification Manager pass after the wake)", (unsigned long)e->ms); break;
        case kEvTaskGap:   Lg("  %7lu  task level had been SILENT for %lu ms (no Notification Manager pass)",
                              (unsigned long)e->ms, (unsigned long)e->val); break;
        default:       Lg("  %7lu  ? kind %u fn %d val %08lX", (unsigned long)e->ms, e->kind, k, (unsigned long)e->val); break;
        }
    }
    Lg("=== END OF WAKE %lu ===", (unsigned long)gWakeN);
}

/* ============================================================== the QEMU self-tests (task level, from the NM pass) */
#ifdef UWAKE_QTEST
#ifdef UWAKE_MODE_A
/* uwake20: QEMU's usb-ehci runs no driver (ours stalls QEMU at "Starting Up": reference_qemu_rom_driver_boot_test), so
   the self-test starts it the way our USB 2.0 driver leaves the FW800's: an empty periodic list, an async ring of one
   queue head (the head of the reclamation list, its overlay halted: nothing to run), USBCMD 00010031 (our driver's: ITC
   1, both schedules, Run), CONFIGFLAG, every port powered, then a port reset on each connected port (QEMU then enables
   a high-speed device: the usb-storage stick). The simulated demand then finds a running EHCI with an enabled
   high-speed device, which is what EhciDemand is for. Task level (allocation, LockMemory, GetPhysical); the memory is
   left locked (the controller reads it from now on). QEMU's EHCI has no companions: PORT_OWNER reads 0 on every port. */
static Ptr gQeMem;
static Boolean QEhciStart(NecFn *n)
{
    LogicalToPhysicalTable tbl; unsigned long count = 8, op, fl, qh, flPhys, qhPhys, v; short i; OSErr e; UInt32 ms;
    volatile unsigned long *q;
    if (!n->ehci || n->ehBase == 0 || !CfgDecodes(n)) { Lg("    QEMU EHCI: not readable: not started"); return false; }
    v = OhciRd(n->ehBase, 0x00);
    if ((v >> 16) != 0x0100UL || (v & 0xFFUL) < 0x0CUL || (v & 0xFFUL) > 0x40UL) {
        Lg("    QEMU EHCI: capability word %08lX: not started", v); return false;
    }
    op = n->ehBase + (v & 0xFFUL);
    if (gQeMem == NULL) gQeMem = NewPtrSysClear(3L * 4096L);
    if (gQeMem == NULL) { Lg("    QEMU EHCI: no memory: not started"); return false; }
    fl = ((unsigned long)gQeMem + 4095UL) & ~4095UL;               /* the frame list: one 4 KB page */
    qh = fl + 4096UL;                                               /* the queue head: the next page's first 64 bytes */
    for (i = 0; i < 8; i++) { tbl.physical[i].address = NULL; tbl.physical[i].count = 0; }
    tbl.logical.address = (void *)fl; tbl.logical.count = 8192UL;
    e = LockMemory((void *)fl, 8192UL);
    if (e == noErr) e = GetPhysical(&tbl, &count);
    if (e != noErr || count < 1) { Lg("    QEMU EHCI: LockMemory/GetPhysical %d: not started", (int)e); return false; }
    flPhys = (unsigned long)tbl.physical[0].address;
    qhPhys = (tbl.physical[0].count >= 8192UL) ? flPhys + 4096UL :
             (count >= 2 && tbl.physical[0].count == 4096UL) ? (unsigned long)tbl.physical[1].address : 0UL;
    if ((flPhys & 0xFFFUL) != 0 || qhPhys == 0 || (qhPhys & 0x1FUL) != 0) {
        Lg("    QEMU EHCI: physical %08lX / %08lX not usable: not started", flPhys, qhPhys); return false;
    }
    for (i = 0; i < 1024; i++) ((volatile unsigned long *)fl)[i] = Swap32(1UL);   /* every frame: Terminate */
    q = (volatile unsigned long *)qh;
    q[0] = Swap32(qhPhys | 2UL);                                    /* horizontal link: itself, type QH */
    q[1] = Swap32(0x00408000UL | (2UL << 12));                      /* max packet 64, H (the head), high speed */
    q[2] = Swap32(0x40000000UL);                                    /* Mult 1 */
    q[3] = 0; q[4] = Swap32(1UL); q[5] = Swap32(1UL);               /* current; next and alternate qTD: Terminate */
    q[6] = Swap32(0x00000040UL);                                    /* token: Halted, not Active */
    for (i = 7; i < 16; i++) q[i] = 0;
    __asm__ volatile ("sync" ::: "memory");
    EhWr(n, op + kEhUsbIntr, 0);
    EhWr(n, op + kEhCtrlDs, 0); EhWr(n, op + kEhPerBase, flPhys); EhWr(n, op + kEhAsync, qhPhys);
    (void)OhciRd(op, kEhAsync);
    EhWr(n, op + kEhUsbCmd, 0x00010031UL);
    (void)EhWait(op + kEhUsbSts, kEhStsHalt, 0, 20, &ms);
    EhWr(n, op + kEhCfgFlag, 1UL); (void)OhciRd(op, kEhCfgFlag);
    for (i = 0; i < (short)(OhciRd(n->ehBase, 0x04) & 0xFUL) && i < kMaxEhPorts; i++) {
        unsigned long a = op + kEhPortSc + 4UL * (unsigned long)i;
        EhWr(n, a, (OhciRd(a, 0) & ~kEhPsRW1C) | kEhPsPP);
    }
    DelayMs(30);
    for (i = 0; i < (short)(OhciRd(n->ehBase, 0x04) & 0xFUL) && i < kMaxEhPorts; i++) {
        unsigned long a = op + kEhPortSc + 4UL * (unsigned long)i;
        v = OhciRd(a, 0);
        if (!(v & kEhPsCCS)) continue;
        EhWr(n, a, (v & ~(kEhPsRW1C | kEhPsPE)) | kEhPsPR); DelayMs(50);
        EhWr(n, a, OhciRd(a, 0) & ~(kEhPsRW1C | kEhPsPR));
        (void)EhWait(a, kEhPsPR, 0, 20, &ms); DelayMs(5);
        v = OhciRd(a, 0);
        EhWr(n, a, v);                                              /* the change bits acknowledged (RW1C) */
        Lg("    QEMU EHCI port %d: PORTSC %08lX after its reset (%s)", (int)i + 1, v,
           (v & kEhPsPE) ? "enabled: a high-speed device" : "NOT enabled");
    }
    Lg("    QEMU EHCI started: frame list %08lX, async head %08lX; USBCMD %08lX, USBSTS %08lX, CONFIGFLAG %08lX", flPhys,
       qhPhys, OhciRd(op, kEhUsbCmd), OhciRd(op, kEhUsbSts), OhciRd(op, kEhCfgFlag));
    return true;
}
/* uwake20: what the deep sleep may do to the EHCI, in QEMU (which loses nothing): the core-well registers reset (USBCMD,
   USBINTR, the list bases, FRINDEX); UWAKE_QTEST_CFLOST also CONFIGFLAG (the auxiliary power lost); UWAKE_QTEST_PRESET
   instead resets the stick's port (its device no longer suspended at the wake). Between EhciDemand and EhciWake. */
static void QEhciSleepLoss(NecFn *n)
{
    unsigned long op; short i;
    if (!n->ehci || !n->ehSusp || n->ehBase == 0 || !n->ehDem.ok) return;
    op = n->ehBase + (n->ehDem.cap & 0xFFUL);
#ifdef UWAKE_QTEST_PRESET
    for (i = 0; i < n->ehDem.n; i++) if (n->ehSuspMask & (1U << i)) {
        unsigned long a = op + kEhPortSc + 4UL * (unsigned long)i;
        EhWr(n, a, (OhciRd(a, 0) & ~(kEhPsRW1C | kEhPsPE)) | kEhPsPR); DelayMs(50);
        EhWr(n, a, OhciRd(a, 0) & ~(kEhPsRW1C | kEhPsPR)); DelayMs(5);
        /* (uw20e3, 2026-11-17: QEMU keeps Suspend through a port reset, which the EHCI spec says clears it, so the reset
           alone left the port reading suspended. Clearing Enable, which QEMU lets software do, gives the wake a port
           whose device did not stay suspended.) */
        EhWr(n, a, OhciRd(a, 0) & ~(kEhPsRW1C | kEhPsPE));
        Lg("    QEMU (PRESET): port %d reset and disabled during the sleep: PORTSC %08lX", (int)i + 1, OhciRd(a, 0));
    }
#else
    (void)i;
#endif
    EhWr(n, op + kEhUsbCmd, 0x00080000UL);                          /* the reset value (halted, no schedule) */
    EhWr(n, op + kEhUsbIntr, 0); EhWr(n, op + kEhPerBase, 0); EhWr(n, op + kEhAsync, 0); EhWr(n, op + kEhFrIndex, 0);
#ifdef UWAKE_QTEST_CFLOST
    EhWr(n, op + kEhCfgFlag, 0);
    Lg("    QEMU (CFLOST): CONFIGFLAG cleared during the sleep, with the core-well registers");
#endif
    Lg("    QEMU: the EHCI's core-well registers reset during the sleep: USBCMD %08lX, PERIODICLISTBASE %08lX, "
       "ASYNCLISTADDR %08lX, CONFIGFLAG %08lX", OhciRd(op, kEhUsbCmd), OhciRd(op, kEhPerBase), OhciRd(op, kEhAsync),
       OhciRd(op, kEhCfgFlag));
}
static void QSimCycleA(void)               /* a sleep's worth of register loss on a LIVE Apple stack */
{
    short i;
    Lg(""); Lg("=== QEMU self-test (mode A): the request's snapshot, Apple's level 1, an HCR (what a power loss resets), "
               "the restore, Apple's level 0, the enable ===");
    for (i = 0; i < gNecN; i++) if (gNec[i].ehci) (void)QEhciStart(&gNec[i]);   /* uwake20: a running EHCI to carry */
    gKwSlpTime = kLowTime;                                         /* uwake12: the simulated sleep's start */
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        if (!n->ohci || !n->baseOK) continue;
        n->req.ok = false; n->slpOK = false; ReadOhci(n, &n->req);
        LogRegs("the request's snapshot", &n->req);
        OhciWr(n, kHcControl, OhciRd(n->base, kHcControl) & ~0x3CUL); DelayMs(2);   /* Apple's level 1 (0x60EC) */
        OhciWr(n, kHcControl, 0x000000C0UL); DelayMs(3);
    }
    DemandReads();                                                 /* uwake19: the handler's demand reads, EHCI included */
    SleepSnap(0);
    KwDemand();                                                    /* uwake11: remote wakeup on the keyboard's function */
    for (i = 0; i < gNecN; i++) EhciDemand(&gNec[i]);              /* uwake20: as the handler, before ArmNec */
    ArmNec();                                                      /* uwake10 (QEMU's OHCI has no PCI PM: "not armed") */
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; char m[40];
        if (!n->ehci) continue;
        EhPorts(n->ehSuspMask, m, sizeof m);
        Lg("    #%d uwake20 demand: %s; ports suspended: %s; USBSTS %08lX after the halt", (int)i,
           n->ehWhy == 0 ? "STOPPED (suspended, halted)" : n->ehWhy == 1 ? "not running: left alone" :
           n->ehWhy == 3 ? "stopped, but HCHalted never read 1" : "not readable", m, n->ehStsH);
        QEhciSleepLoss(n);
    }
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        if (!n->ohci || !n->baseOK) continue;
        Lg("    #%d uwake10 arming: %s; uwake11 remote wakeup: %s (HcControl %08lX -> %08lX, HcRhStatus %08lX -> %08lX)",
           (int)i, n->armWhy == 0 ? "ARMED" : n->armWhy == 1 ? "not armed (no PCI PM capability)" :
           n->armWhy == 2 ? "not armed (not suspended)" : "not armed (not reached)",
           n->kwWhy == 0 ? "SET" : n->kwWhy == 1 ? "not set (no armed keyboard or Bluetooth controller here)" :
           n->kwWhy == 3 ? "not set (no survey yet)" :
           n->kwWhy == 4 ? "NOT SET (the write did not take)" : "not set", n->kwCtl0, n->kwCtl1, n->kwRh0, n->kwRh1);
    }
    KwQHold();                                                     /* a key, while the bus is suspended */
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i];
        if (!n->ohci || !n->baseOK) continue;
        WakeDisarm(n);
        n->hcdWake = HccaDone(NecHcca(n));
#ifndef UWAKE_QTEST_KEEP
        OhciWr(n, kHcCommandStatus, 0x00000001UL); DelayMs(5);     /* HCR: HCCA, heads, FmInterval back to reset */
#else
        Lg("    QTEST_KEEP: no simulated power loss (the NEC kept its state, as armed on the FW800 it should)");
#endif
        RestoreConfig(n);                                          /* QEMU keeps PCI config: "intact" */
        n->enOutcome = kEnNone; n->enForcedOp = false; n->ueBack = false;
        RestoreOhci(n);
        KwWake(n);                                                 /* uwake11: what the wake left */
        Lg("    #%d restore: %s; HcFmNumber wrote %04lX, read back %04lX; uwake11 at the wake: HcControl %08lX (HCFS %s), "
           "InterruptStatus %08lX%s", (int)i, OhciOut(n->ohciOutcome), n->fmWrote & 0xFFFFUL, n->fmBack & 0xFFFFUL,
           n->kwWkCtl, Hcfs(n->kwWkCtl), n->kwWkIst, (n->kwWkIst != 0xFFFFFFFFUL && (n->kwWkIst & kIntRD)) ? " (RD)" : "");
    }
    for (i = 0; i < gNecN; i++) {                                  /* uwake19: the other functions, as the handler does them
                                                                      (its loop runs RestoreConfig for EVERY function) */
        NecFn *n = &gNec[i]; char m1[40], m2[40];
        if (n->ohci) continue;
        WakeDisarm(n);                                             /* uwake20: the EHCI too, when we put it in D3hot */
        RestoreConfig(n);
        EhciWake(n);
        EhPorts(n->ewResMask, m1, sizeof m1); EhPorts(n->ewGoneMask, m2, sizeof m2);
        Lg("    #%d (not OHCI) config: %s; EHCI after the wake: %s; uwake20 wake %d: ports resumed %s, did not stay "
           "suspended %s; USBCMD %08lX, USBSTS %08lX", (int)i, CfgOut(n->outcome),
           !n->ehci ? "not an EHCI" : n->ehWake.ok ? "read" : "not read", (int)n->ewWhy, m1, m2, n->ewCmd, n->ewSts);
    }
    gWakeN++; RecStart(); Ev(0xFF, kEvRestore, 0, gWakeN); gCell3 = 0x80;
    for (i = 0; i < gNecN; i++) {                                  /* Apple's level 0 (0x6468..0x6598): it runs for
                                                                      EVERY function, restored by us or kept (uwake10) */
        NecFn *n = &gNec[i]; unsigned long hcfs;
        if (!n->ohci || !n->baseOK) continue;
        hcfs = (OhciRd(n->base, kHcControl) >> 6) & 3UL;
        if (hcfs == 2UL) continue;
        if (hcfs == 3UL) OhciWr(n, kHcControl, 0x00000040UL);
        if (hcfs != 0UL) DelayMs(20);
        OhciWr(n, kHcControl, 0x00000080UL); DelayMs(3);
        OhciWr(n, kHcControl, 0x000000BCUL);
    }
    EnableA(); KwEnable(); EhciEnable(); gEnableMs = RecMs(); Ev(0xFF, kEvEnable, 0, gEnableMs);
    KwQUnmask();                                                   /* MIE back: Apple's ISR takes RD now, as after a wake */
    Lg("    QEMU: MIE back on; Apple's ISR takes any ResumeDetected now, as it would after the FW800's wake");
    LogFlush();
}
#endif
#ifdef UWAKE_MODE_B
static UInt32 gQPhase, gQTicks;
static void QSimSleepB(void)               /* the sleep queue's request + demand: Apple's teardown, with the HC powered */
{
    long r1, r2;
    Lg(""); Lg("=== QEMU self-test (mode B): the sleep queue's request and demand through our procedure (Apple's teardown) ===");
    r1 = WrapSleepProc(1, gUsbG + 0x90);
    Lg("    request: %ld in %lu ms", r1, (unsigned long)gSq.reqMs);
    if (r1 != 0) { (void)WrapSleepProc(4, gUsbG + 0x90); Lg("!! a driver refused the sleep: revoked, no teardown"); gQPhase = 9; LogFlush(); return; }
    r2 = WrapSleepProc(2, gUsbG + 0x90);
    Lg("    demand (Apple's teardown): %ld in %lu ms; the wake follows in 3 s", r2, (unsigned long)gSq.demMs);
    LogFlush();
}
static void QSimWakeB(void)
{
    long r3;
    r3 = WrapSleepProc(3, gUsbG + 0x90);
    Lg("    wake (our config restore + Apple's reload): %ld in %lu ms (config %lu ms; %s)", r3, (unsigned long)gSq.wakeMs,
       (unsigned long)gSq.cfgMs, gSq.sysMode ? "system mode" : "NOT system mode");
    LogFlush();
}
#endif
#endif /* UWAKE_QTEST */

/* ============================================================== the put-away before sleep (ship3's SleepNetHook)
 * Ported from the Path A companion (ohci_companion_resident.c 2370-2763, ship2/ship3: proven on hardware 2026-11-09/10)
 * and extended for Apple's USB sleep mode. Apple's teardown (mode B), or the power loss itself (mode A), yanks a mounted
 * USB volume (operator, 2026-11-12: "unexpectedly disconnected" at the wake; unflushed writes are lost), and a server
 * volume left mounted brings back the power manager's "connected to a server" alert, which crashed this machine (the
 * companion's fix, gone with the companion). HandleSleep+0x168 calls PmgrBase+0x60 ("SleepNetHook", PowerPrivEqu.a; NPM
 * never sets it) as CallUniversalProc(hook, 0x1022, type), BEFORE CheckAppleTalk (+0x1CC) and the classic sleep queue
 * (+0x258, where Apple's teardown runs) -- checked 2026-11-12 -- so every disk still answers. A non-zero answer takes
 * the alert's own Cancel exit: the Mac stays awake. For a USER sleep (raw type 2, -128, -129 or 13, at PmgrBase+0x2FA):
 *   1. every SERVER volume, by AppleShare's AutoRemounter steps: ReleaseFolder(Trash), then count the FCBs whose own
 *      ioFCBVRefNum is the volume; one open -> cancel, put NOTHING away, a notice naming it (servers left to the Auto
 *      Remounter when its unmount-before-sleep option is on);
 *   2. every USB volume (its driver's DriverDescription nameInfoStr: ".AppleUSBMassStorage" = Apple's UMSS, checked in
 *      the ROM; "usb,massdsk" = ours): its open files walked, classed and logged (SnhFileKind), but UnmountVol DECIDES
 *      (uwake4: the walk's own verdict refused a sleep over File Sharing's "AppleShare PDS", a file only File Sharing can
 *      release). A volume with an open USER file goes first, so a refusal comes before anything is put away;
 *   3. every USB volume put away (UnmountVol; no Eject: both drivers' eject paths post "You may now remove..."), then
 *      every server (Eject + UnmountVol); a failure -> cancel, a notice naming what is still open on that volume, or the
 *      File Sharing users holding it, or the error (ship3 left a busy USB volume mounted: under Apple's sleep mode that
 *      is the yank).
 * IDLE sleep (raw 1) with a USB volume mounted: cancelled quietly, as CheckAppleTalk does for servers (putting it away
 * there could leave it unmounted with the Mac awake, if CheckAppleTalk then refuses). Type 6 is left alone.
 * CONTEXT: task level inside HandleSleep, in whichever application serviced the power manager's call: the File Manager
 * yes (NPM itself makes File Manager calls here), QuickDraw and dialogs NO (notices go through the Notification
 * Manager), no formatted printing. Each step is logged and flushed at once (a hang leaves its last line); the FCB walks
 * collect first and log after (the log's own open and close would shift the FCB indices). Installed only if
 * HandleSleep's hook call site is byte-for-byte the analysed NativePowerMgrLib (d9e42f48); chains whatever hook was
 * there before (asked first: its refusal stands). */
#define kSnhHookSlot      0x60UL       /* PmgrBase: SleepNetHook */
#define kSnhRawType       0x2FAUL      /* PmgrBase: this sleep's raw type (SInt16) */
#define kNpmPMSimpleSleep 0x0CE54UL    /* PMSimpleSleep's offset in NativePowerMgrLib d9e42f48's code section */
#define kNpmHookSite      0x0BFA4UL    /* HandleSleep+0x168 in that section: the SleepNetHook call */
#define kSnhMaxVols       16
#define kSnhMaxWalk       1024         /* the FCB walk's bound; it ends at its first error (-38 past the last FCB) */
#define kSnhKeep          8            /* FCBs kept per walk: an HFS+ disk's B-trees + Desktop DB/DF + PDS all fit */
#define kSnhAfpFileBusy   (-5010)      /* afpFileBusy: File Sharing 7.6.1+'s UnmountVol answer when GUESTS have files open */
enum {
    kSleepNetHookProcInfo = kRegisterBased
                          | RESULT_SIZE(SIZE_CODE(sizeof(SInt16)))
                          | REGISTER_RESULT_LOCATION(kRegisterD0)
                          | REGISTER_ROUTINE_PARAMETER(1, kRegisterD0, SIZE_CODE(sizeof(SInt16)))
};
typedef char snh_chk_procinfo[(kSleepNetHookProcInfo == 0x1022) ? 1 : -1];
/* uwake13: NPM's own call of the File Sharing server (_ServerDispatch, A094): D0 = 0, D1 = the trap word, A0 = the pb,
   an OSErr back in D0 (NativePowerMgrLib d9e42f48 ServerDispatch, 0xCE70: CallOSTrapUniversalProc with this procInfo) */
enum {
    kSnhServerDispatchProcInfo = kRegisterBased
                               | RESULT_SIZE(SIZE_CODE(sizeof(SInt16)))
                               | REGISTER_RESULT_LOCATION(kRegisterD0)
                               | REGISTER_ROUTINE_PARAMETER(1, kRegisterD0, SIZE_CODE(sizeof(long)))
                               | REGISTER_ROUTINE_PARAMETER(2, kRegisterD1, SIZE_CODE(sizeof(long)))
                               | REGISTER_ROUTINE_PARAMETER(3, kRegisterA0, SIZE_CODE(sizeof(long)))
};
typedef char snh_chk_srvprocinfo[(kSnhServerDispatchProcInfo == 0x02671822) ? 1 : -1];
typedef char snh_chk_procinfolen[(sizeof(ProcessInfoRec) == 0x3C) ? 1 : -1];
typedef char snh_chk_vmserver[(offsetof(GetVolParmsInfoBuffer, vMServerAdr) == 10) ? 1 : -1];
typedef char snh_chk_vdrefnum[(offsetof(HParamBlockRec, volumeParam.ioVDRefNum) == 68) ? 1 : -1];
typedef char snh_chk_volindex[(offsetof(HParamBlockRec, volumeParam.ioVolIndex) == 28) ? 1 : -1];
typedef char snh_chk_iobuffer[(offsetof(HParamBlockRec, ioParam.ioBuffer) == 32) ? 1 : -1];
typedef char snh_chk_fcbindex[(offsetof(FCBPBRec, ioFCBIndx) == 28) ? 1 : -1];
typedef char snh_chk_fcbflnm [(offsetof(FCBPBRec, ioFCBFlNm) == 32) ? 1 : -1];
typedef char snh_chk_fcbvref [(offsetof(FCBPBRec, ioFCBVRefNum) == 52) ? 1 : -1];   /* the AutoRemounter reads +0x34 */
typedef char snh_chk_fcbparid[(offsetof(FCBPBRec, ioFCBParID) == 58) ? 1 : -1];
/* ReleaseFolder(vRefNum, folderType) = _AliasDispatch (0xA823) selector 0x0B, called the way FoldersLib's glue does. */
enum {
    kSnhReleaseFolderProcInfo = kD0DispatchedPascalStackBased
                              | RESULT_SIZE(SIZE_CODE(sizeof(OSErr)))
                              | DISPATCHED_STACK_ROUTINE_SELECTOR_SIZE(kFourByteCode)
                              | DISPATCHED_STACK_ROUTINE_PARAMETER(1, SIZE_CODE(sizeof(short)))
                              | DISPATCHED_STACK_ROUTINE_PARAMETER(2, SIZE_CODE(sizeof(OSType)))
};
typedef char snh_chk_relfolder[(kSnhReleaseFolderProcInfo == 0xEE8) ? 1 : -1];
typedef char snh_chk_nameinfo[(offsetof(DriverDescription, driverType.nameInfoStr) == 8) ? 1 : -1];
typedef char snh_chk_nmstr   [(offsetof(NMRec, nmStr) == 24) ? 1 : -1];
static const UInt32 kSnhSite[6] = { 0x81290060UL, 0x2c090000UL, 0x41820028UL,    /* lwz r9,96(r9); cmpwi r9,0; beq */
                                    0x7d234b78UL, 0x7fa5eb78UL, 0x38801022UL };   /* mr r3,r9; mr r5,r29; li r4,0x1022 */
typedef UInt32 (*SnhWonOptsFn)(void);
static UniversalProcPtr  gSnhRD, gSnhPrev;
static UInt32            gSnhG;
static SnhWonOptsFn      gSnhWonOpts;
static Boolean           gSnhTried;
static volatile UInt32   gSnhCalls, gSnhActed, gSnhSrvAway, gSnhUsbAway, gSnhDenied, gSnhIdleDenied, gSnhPrevDenied;

static void LgNow(const char *s)                 /* TASK LEVEL: one line, appended and written at once; no formatting */
{
    long n = (long)strlen(s);
    if (n > 299) n = 299;
    if (gLogLen + n + 1 > (long)sizeof gLog) LogFlush();
    if (gLogLen + n + 1 <= (long)sizeof gLog) { memcpy(gLog + gLogLen, s, (size_t)n); gLogLen += n; gLog[gLogLen++] = '\r'; }
    LogFlush();
}
/* ---- text for the log and the notices, without printf (ship3's helpers) ---- */
typedef struct { char s[256]; int n; } SnhLine;
static void SlC(SnhLine *b, const char *c)
{ while (*c && b->n < 255) b->s[b->n++] = *c++; b->s[b->n] = 0; }
static void SlP(SnhLine *b, const unsigned char *p)
{ int i; for (i = 1; i <= p[0] && b->n < 255; i++) b->s[b->n++] = (char)p[i]; b->s[b->n] = 0; }
static void SlU(SnhLine *b, unsigned long u)
{
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + (int)(u % 10UL)); u /= 10UL; } while (u != 0 && k < 11);
    while (k > 0 && b->n < 255) b->s[b->n++] = t[--k];
    b->s[b->n] = 0;
}
static void SlD(SnhLine *b, long v)
{ if (v < 0) { SlC(b, "-"); SlU(b, 0UL - (unsigned long)v); } else SlU(b, (unsigned long)v); }
static void SlH(SnhLine *b, unsigned long v, int digits)
{
    static const char hx[] = "0123456789abcdef"; int j;
    SlC(b, "0x");
    for (j = (digits - 1) * 4; j >= 0 && b->n < 255; j -= 4) b->s[b->n++] = hx[(v >> j) & 0xF];
    b->s[b->n] = 0;
}
static Boolean PStrEq(const unsigned char *a, const unsigned char *b)
{ int i; if (a[0] != b[0]) return false; for (i = 1; i <= a[0]; i++) if (a[i] != b[i]) return false; return true; }

/* a USB block driver, by its DriverDescription: 1 Apple's (USB 1.1, through the OHCIs), 2 ours (usb,massdsk: the
   USB 2.0 driver, through the EHCI), 0 neither */
static short SnhUsbKind(short dRef)
{
    static const unsigned char kApple[] = "\p.AppleUSBMassStorage", kOurs[] = "\pusb,massdsk";
    UnitNumber unit = 0; DriverFlags flags = 0; DriverOpenCount count = 0; Str255 name; RegEntryID dev;
    CFragSystem7Locator loc; CFragConnectionID conn = 0; DriverEntryPointPtr ep = 0; DriverDescription desc;
    OSErr err;
    if (dRef >= 0) return 0;                     /* driver refNums are negative; 0 = no driver */
    memset(&loc, 0, sizeof(loc)); memset(&desc, 0, sizeof(desc)); name[0] = 0;
    RegistryEntryIDInit(&dev);
    err = GetDriverInformation(dRef, &unit, &flags, &count, name, &dev, &loc, &conn, &ep, &desc);
    RegistryEntryIDDispose(&dev);
    if (err != noErr) return 0;
    return PStrEq(desc.driverType.nameInfoStr, kApple) ? 1 : PStrEq(desc.driverType.nameInfoStr, kOurs) ? 2 : 0;
}
static Boolean SnhIsUsbDisk(short dRef) { return SnhUsbKind(dRef) != 0; }
#ifdef UWAKE_MODE_A
static short SnhVolDrv(short vRef)               /* the volume's driver refNum (0 = none or no such volume) */
{
    HParamBlockRec pb;
    memset(&pb, 0, sizeof(pb));
    pb.volumeParam.ioNamePtr = NULL; pb.volumeParam.ioVRefNum = vRef; pb.volumeParam.ioVolIndex = 0;
    return (PBHGetVInfoSync(&pb) == noErr) ? pb.volumeParam.ioVDRefNum : 0;
}
static void SnhCollect(short *srv, int *nSrv, short *usb, int *nUsb);
static void SnhVolName(short vRef, Str255 vol);
static void LogUsbVols(void)                     /* uwake20, TASK LEVEL (the report): the USB volumes mounted now */
{
    short srv[kSnhMaxVols], usb[kSnhMaxVols]; int nSrv = 0, nUsb = 0, i, l; Str255 vol; char nm[64];
    SnhCollect(srv, &nSrv, usb, &nUsb);
    if (nUsb == 0) { Lg("  USB volumes mounted when this report was written: none"); return; }
    for (i = 0; i < nUsb; i++) {
        short k = SnhUsbKind(SnhVolDrv(usb[i]));
        SnhVolName(usb[i], vol);
        l = vol[0] < 63 ? vol[0] : 63; memcpy(nm, vol + 1, (size_t)l); nm[l] = 0;
        Lg("  USB volume mounted when this report was written: \"%s\" (vRef %d), %s", nm, (int)usb[i],
           k == 2 ? "USB 2.0 (our driver)" : "USB 1.1 (Apple's driver)");
    }
}
#endif
static void SnhCollect(short *srv, int *nSrv, short *usb, int *nUsb)   /* every mounted volume, classified */
{
    HParamBlockRec pb; GetVolParmsInfoBuffer vp; short idx, vRef, dRef;
    *nSrv = *nUsb = 0;
    for (idx = 1; idx <= 64; idx++) {
        memset(&pb, 0, sizeof(pb));
        pb.volumeParam.ioNamePtr = NULL; pb.volumeParam.ioVRefNum = 0; pb.volumeParam.ioVolIndex = idx;
        if (PBHGetVInfoSync(&pb) != noErr) break;                       /* past the last volume */
        vRef = pb.volumeParam.ioVRefNum; dRef = pb.volumeParam.ioVDRefNum;
        memset(&pb, 0, sizeof(pb)); memset(&vp, 0, sizeof(vp));
        pb.ioParam.ioNamePtr = NULL; pb.ioParam.ioVRefNum = vRef;
        pb.ioParam.ioBuffer = (Ptr)&vp; pb.ioParam.ioReqCount = (long)sizeof(vp);
        if (PBHGetVolParmsSync(&pb) == noErr && vp.vMServerAdr != 0) { if (*nSrv < kSnhMaxVols) srv[(*nSrv)++] = vRef; }
        else if (SnhIsUsbDisk(dRef))                                  { if (*nUsb < kSnhMaxVols) usb[(*nUsb)++] = vRef; }
    }
}
static void SnhVolName(short vRef, Str255 vol)   /* "" when there is no such volume */
{
    HParamBlockRec pb;
    vol[0] = 0;
    memset(&pb, 0, sizeof(pb));
    pb.volumeParam.ioNamePtr = vol; pb.volumeParam.ioVRefNum = vRef; pb.volumeParam.ioVolIndex = 0;
    if (PBHGetVInfoSync(&pb) != noErr) vol[0] = 0;
}
static void SnhLogVol(const char *what, short vRef)
{
    SnhLine b; Str255 vol;
    SnhVolName(vRef, vol);
    b.n = 0; b.s[0] = 0;
    SlC(&b, "  sleepnet:   "); SlC(&b, what); SlC(&b, " volume \""); SlP(&b, vol); SlC(&b, "\" (vRef "); SlD(&b, vRef);
    SlC(&b, ")");
    LgNow(b.s);
}
static void SnhLogErr(const char *what, OSErr e)
{
    SnhLine b;
    b.n = 0; b.s[0] = 0; SlC(&b, what); SlC(&b, " "); SlD(&b, e);
    LgNow(b.s);
}
/* ---- the open files of a volume, by APPLE'S RULE: PBGetFCBInfo by index with ioVRefNum = the volume, counting an FCB
 * only if its own ioFCBVRefNum IS that volume (AppleShare's AutoRemounter, 'CUST' 45 +0x14e8..+0x1520). Each one is
 * classed (SnhFileKind) for the log and the notice; for a USB volume only UnmountVol decides. */
enum { kSnhUser = 0, kSnhOwn, kSnhDesktop, kSnhSharing };
typedef struct { short idx, ref, vref, flags; long flNm, parID; unsigned char name[32]; } SnhFcb;
typedef struct {
    int returned, onVol, userOnVol, ownN, dtN, pdsN; OSErr endErr;
    SnhFcb on[kSnhKeep], user[kSnhKeep];
} SnhWalk;
static void SnhKeep(SnhFcb *k, short idx, const FCBPBRec *fb, const unsigned char *nm)
{
    int i, n = (nm[0] > 31) ? 31 : nm[0];
    k->idx = idx; k->ref = fb->ioRefNum; k->vref = fb->ioFCBVRefNum; k->flags = fb->ioFCBFlags; k->flNm = fb->ioFCBFlNm;
    k->parID = fb->ioFCBParID;
    k->name[0] = (unsigned char)n;
    for (i = 1; i <= n; i++) k->name[i] = nm[i];
}
/* What an open file on a local volume is:
 *   kSnhOwn      the volume's own B-trees and bitmap (file numbers below 16): the ROM's UnmountVol skips them
 *                (rom-tools/upstream/boot3/OS/HFS/TFSVOL.a, UnMountVolTrap: "Is it an internal file?");
 *   kSnhDesktop  the Desktop Manager's "Desktop DB"/"Desktop DF" (or the old "Desktop") in the root: UnmountVol's own
 *                first step, DesktopCloseDownProc, closes them;
 *   kSnhSharing  File Sharing's invisible "AppleShare PDS", held open on every mounted volume while File Sharing is on
 *                (hardware 2026-11-12, uwake3B: "AppleShare PDS is open on the USB disk PNY1" refused a sleep the user
 *                had every right to). The ROM's UnmountVol has no exemption for it (TFSVOL.a: AppleShare's 'GRT ' hack
 *                was stripped, <SM1>); File Sharing 7.6.1 and later answers UnmountVol on a shared volume itself and
 *                lets it go when no guest has it mounted (Apple technote NW29: fBsyErr, or afpFileBusy -5010 for
 *                guests; UnmountIt 1.2's read-me: File Sharing no longer has to be stopped). UNPROVEN on this Mac until
 *                test B (the companion's UnmountVol took PNY1 with 0 on 2026-11-11, but File Sharing's state then was
 *                not recorded): if UnmountVol answers -47 with only the PDS left, the notice says File Sharing kept it;
 *   kSnhUser     anything else. */
static int SnhFileKind(const FCBPBRec *fb, const unsigned char *nm)
{
    static const unsigned char kDB[] = "\pDesktop DB", kDF[] = "\pDesktop DF", kD[] = "\pDesktop",
                               kPDS[] = "\pAppleShare PDS";
    if ((unsigned long)fb->ioFCBFlNm < 16UL) return kSnhOwn;
    if (fb->ioFCBParID == 2L && (PStrEq(nm, kDB) || PStrEq(nm, kDF) || PStrEq(nm, kD))) return kSnhDesktop;
    if (PStrEq(nm, kPDS)) return kSnhSharing;                          /* any directory */
    return kSnhUser;
}
static void SnhWalkVol(short vRef, SnhWalk *w)
{
    FCBPBRec fb; Str255 nm; short idx; int kind;
    memset(w, 0, sizeof(*w));
    for (idx = 1; idx <= kSnhMaxWalk; idx++) {
        memset(&fb, 0, sizeof(fb)); nm[0] = 0;
        fb.ioNamePtr = nm; fb.ioVRefNum = vRef; fb.ioFCBIndx = idx;
        w->endErr = PBGetFCBInfoSync(&fb);
        if (w->endErr != noErr) break;
        w->returned++;
        if (fb.ioFCBVRefNum != vRef) continue;
        if (w->onVol < kSnhKeep) SnhKeep(&w->on[w->onVol], idx, &fb, nm);
        w->onVol++;
        kind = SnhFileKind(&fb, nm);
        if (kind == kSnhOwn) w->ownN++;
        else if (kind == kSnhDesktop) w->dtN++;
        else if (kind == kSnhSharing) w->pdsN++;
        else {
            if (w->userOnVol < kSnhKeep) SnhKeep(&w->user[w->userOnVol], idx, &fb, nm);
            w->userOnVol++;
        }
    }
}
static void SnhLogFcb(const char *tag, const SnhFcb *k)
{
    SnhLine b;
    b.n = 0; b.s[0] = 0;
    SlC(&b, "  sleepnet:     "); SlC(&b, tag); SlC(&b, " index "); SlD(&b, k->idx); SlC(&b, ": refNum "); SlD(&b, k->ref);
    SlC(&b, ", file ID "); SlD(&b, k->flNm); SlC(&b, ", in dir "); SlD(&b, k->parID); SlC(&b, ", flags ");
    SlH(&b, (unsigned long)(unsigned short)k->flags, 4); SlC(&b, ", \""); SlP(&b, k->name); SlC(&b, "\"");
    LgNow(b.s);
}
static void SnhLogWalk(short vRef, const SnhWalk *w)
{
    SnhLine b; int i;
    b.n = 0; b.s[0] = 0;
    SlC(&b, "  sleepnet:   open files of vRef "); SlD(&b, vRef); SlC(&b, ": "); SlD(&b, w->returned);
    SlC(&b, " returned, "); SlD(&b, w->onVol); SlC(&b, " on the volume (its own "); SlD(&b, w->ownN);
    SlC(&b, ", the Desktop's "); SlD(&b, w->dtN); SlC(&b, ", File Sharing's "); SlD(&b, w->pdsN);
    SlC(&b, ", user files "); SlD(&b, w->userOnVol); SlC(&b, "); the walk ended with "); SlD(&b, w->endErr);
    LgNow(b.s);
    for (i = 0; i < w->onVol && i < kSnhKeep; i++) SnhLogFcb("on the volume:", &w->on[i]);
    if (w->onVol > kSnhKeep)                                     /* past the listed ones: the user files */
        for (i = 0; i < w->userOnVol && i < kSnhKeep; i++) SnhLogFcb("user file:", &w->user[i]);
}
static OSErr SnhReleaseTrash(short vRef)
{
    UniversalProcPtr t = (UniversalProcPtr)GetToolboxTrapAddress(0xA823);          /* _AliasDispatch */
    return (OSErr)CallUniversalProc(t, kSnhReleaseFolderProcInfo, 0x0BL, (long)vRef, (long)kTrashFolderType);
}
static OSErr SnhPutAway(short vRef)              /* UnmountVol (fBsyErr if a file is open) */
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof(pb));
    pb.volumeParam.ioNamePtr = NULL; pb.volumeParam.ioVRefNum = vRef;
    return PBUnmountVol(&pb);
}
static OSErr SnhPutAwayServer(short vRef, OSErr *ejectErr)   /* AppleShare's AutoRemounter: Eject, then UnmountVol */
{
    ParamBlockRec pb;
    memset(&pb, 0, sizeof(pb));
    pb.volumeParam.ioNamePtr = NULL; pb.volumeParam.ioVRefNum = vRef;
    *ejectErr = PBEject(&pb);                    /* its result is not what decides (the AutoRemounter drops both) */
    return SnhPutAway(vRef);
}
/* Cancel this sleep with a Notification Manager notice: why = the open file it names, File Sharing users holding the
 * volume (afpFileBusy), File Sharing's own file (UnmountVol's -47 with only "AppleShare PDS" open), or the error. */
enum { kSnhWhyFile = 0, kSnhWhyError, kSnhWhyGuests, kSnhWhySharing };
static void SnhRefuse(short vRef, Boolean server, int why, const unsigned char *file, OSErr err)
{
    SnhLine b; Str255 vol; NMRecPtr nm; StringPtr s; OSErr e; int i;
    const char *what = server ? "the server volume \"" : "the USB disk \"";
    gSnhDenied++;
    SnhVolName(vRef, vol);
    b.n = 0; b.s[0] = 0;
    SlC(&b, "The computer did not sleep: ");
    if (why == kSnhWhyFile && file != NULL && file[0] != 0) {
        SlC(&b, "\""); SlP(&b, file); SlC(&b, "\" is open on "); SlC(&b, what); SlP(&b, vol);
        SlC(&b, "\". Close it, then choose Sleep again.");
    } else if (why == kSnhWhyGuests) {
        SlC(&b, what); SlP(&b, vol); SlC(&b, "\" is in use by File Sharing users (error "); SlD(&b, err);
        SlC(&b, "). Disconnect them in the File Sharing control panel, then choose Sleep again.");
    } else if (why == kSnhWhySharing) {
        SlC(&b, "File Sharing kept "); SlC(&b, what); SlP(&b, vol); SlC(&b, "\" busy (error "); SlD(&b, err);
        SlC(&b, "). Put it away in the Finder, or turn File Sharing off, then choose Sleep again.");
    } else {
        SlC(&b, what); SlP(&b, vol); SlC(&b, "\" could not be put away (error "); SlD(&b, err);
        SlC(&b, "). Put it away in the Finder, then choose Sleep again.");
    }
    nm = (NMRecPtr)NewPtrSysClear((Size)sizeof(NMRec));
    s  = (StringPtr)NewPtrSysClear((Size)256);
    if (nm == NULL || s == NULL) { LgNow("  sleepnet:   no memory for the notice (the sleep is still cancelled)"); return; }
    s[0] = (unsigned char)b.n;
    for (i = 0; i < b.n; i++) s[1 + i] = (unsigned char)b.s[i];
    nm->qType = nmType; nm->nmMark = 0; nm->nmIcon = NULL; nm->nmSound = NULL;
    nm->nmStr = s; nm->nmResp = (NMUPP)-1L; nm->nmRefCon = 0x534e4820L;   /* -1: removed once shown; 'SNH ' */
    e = NMInstall(nm);
    SnhLogErr("  sleepnet:   notice posted (NMInstall; 0 = ok)", e);
    LgNow(b.s);
}
static void SnhLogAway(int away)                 /* after a refusal: the USB volumes this sleep had already put away */
{
    SnhLine b;
    if (away <= 0) return;
    b.n = 0; b.s[0] = 0;
    SlC(&b, "  sleepnet:   "); SlD(&b, away);
    SlC(&b, " USB volume(s) had been put away before it; they stay put away (unplug and plug them back to mount them)");
    LgNow(b.s);
}
/* UnmountVol refused a USB volume: what is still open on it is the reason (UnmountVol's first step,
 * DesktopCloseDownProc, has already closed the Desktop database). Cancels the sleep with the matching notice. */
static void SnhRefuseUsb(short vRef, OSErr err, SnhWalk *w, int away)
{
    SnhWalkVol(vRef, w);
    LgNow("  sleepnet:   UnmountVol refused it; open on it now:");
    SnhLogWalk(vRef, w);
    if (err == kSnhAfpFileBusy) SnhRefuse(vRef, false, kSnhWhyGuests, NULL, err);
    else if (w->userOnVol > 0) {
        SnhLogFcb("OPEN USER FILE:", &w->user[0]);
        SnhRefuse(vRef, false, kSnhWhyFile, w->user[0].name, err);
    }
    else if (w->pdsN > 0) SnhRefuse(vRef, false, kSnhWhySharing, NULL, err);
    else                  SnhRefuse(vRef, false, kSnhWhyError, NULL, err);
    SnhLogAway(away);
    LgNow("  sleepnet:   -> sleep CANCELLED: a USB disk could not be put away");
}
/* ---- uwake13: Mac OS's own answer for an IDLE sleep, asked the way NativePowerMgrLib asks it (NPM-CHECKAPPLETALK.md:
   CheckAppleTalk, which HandleSleep runs right after this hook). With AppleTalk active and the Auto Remounter not
   engaged, an idle sleep is refused if a server is mounted or File, Printer or Web Sharing is on. The user (2026-11-14):
   keep Apple's rule. So when sharing will refuse it, the hook must put NOTHING away (uwake12A put the share away and the
   Mac then stayed awake without it, at every idle-timer expiry). TASK LEVEL: these are the calls NPM itself makes here. */
static Boolean SnhAppleTalkActive(void) { return (*(volatile UInt8 *)0x0291UL & 0x0FU) != 0; }   /* PortBUse low nibble */
static SInt16 SnhServerCall(short csCode, SInt16 *word)   /* NPM's ServerDispatch: csCode in, the pb's word +0x1C out */
{
    UInt8 pb[0x50]; long r;
    *word = -50;
    if (GetOSTrapAddress(0xA094) == GetToolTrapAddress(0xA89F)) return -50;   /* no File Sharing server (unimplemented) */
    memset(pb, 0, sizeof pb);
    *(SInt16 *)(pb + 0x1A) = csCode;
    r = CallOSTrapUniversalProc(*(UniversalProcPtr volatile *)0x0650UL, kSnhServerDispatchProcInfo, 0L, 0xA094L, (long)pb);
    if ((SInt16)r != 0) return (SInt16)r;
    *word = *(SInt16 *)(pb + 0x1C);
    return 0;
}
static short SnhFileSharing(SInt16 *status, SInt16 *version)   /* NPM's FileSharingActive: 1 on, 0 off */
{
    SInt16 s, v; UInt32 g;
    (void)SnhServerCall(5, &s);                                   /* ServerStatus (-50 if the call failed) */
    (void)SnhServerCall(14, &v);                                  /* ServerVersion */
    *status = s; *version = v;
    if (v == 1) return (s == -1 || s == -2) ? 1 : 0;
    if (v == 0) {
        g = *(volatile UInt32 *)0x0B50UL;
        return (s == -1 && g != 0 && !(g & 1UL) && InRAM(g, 0x12) && *(volatile SInt16 *)(g + 0x10) > 0) ? 1 : 0;
    }
    return 0;
}
static Boolean SnhPrinterSharing(void)                            /* NPM's PrinterSharingActive */
{
    long resp = 0;
    return Gestalt('zak ', &resp) == noErr && (resp & (long)0x80000000UL) != 0;
}
static Boolean SnhWebSharing(void)                                /* NPM's WebSharingActive (FindTypeCreatorProcess) */
{
    ProcessSerialNumber psn; ProcessInfoRec info;
    psn.highLongOfPSN = 0; psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof info); info.processInfoLength = (UInt32)sizeof info;
        if (GetProcessInformation(&psn, &info) != noErr) break;
        if (info.processType == 'appe' && info.processSignature == 'wbsh') return true;
    }
    return false;
}
/* What the hook decides, for a sleep of this raw type. 0 = sleep on, 1 = cancel. */
static SInt16 SnhDecide(SInt16 raw)
{
    short srv[kSnhMaxVols], usb[kSnhMaxVols], ord[kSnhMaxVols]; Boolean busy[kSnhMaxVols];
    int nSrv = 0, nUsb = 0, nOrd = 0, i, away = 0; OSErr e, ej; SnhWalk w; SnhLine b;
    /* uwake12: EVERY sleep that can go ahead with a server mounted puts it away here, chosen or idle, whatever the Auto
       Remounter says. uwake11A on the FW800 (2026-11-13 evening, log 42c39bf7): with the Auto Remounter engaged
       (PMGetWakeOnNetActOptions bit 8, which also makes NPM's CheckAppleTalk skip its server check) this hook left the
       share to it, the share was still mounted through the sleep ("unexpectedly disconnected" at the wake) and the
       Finder hung after the wake (the cursor moved, the clock and input stopped). The Auto Remounter ignores the results
       of its own Eject and UnmountVol (reference_os9_appleshare_trash_usage_map_open). And uwake10/11's mode A returned
       for an idle type before looking at servers at all. NPM's idle types are 1 and 12 (CheckAppleTalk denies those
       silently while a server is mounted, unless the Auto Remounter is engaged). An idle sleep is never refused with a
       notice: it is simply not taken (the idle timer tries again later). */
    Boolean user = (raw == 2 || raw == -128 || raw == -129 || raw == 13);
    Boolean idle = (raw == 1 || raw == 12);
    Boolean arm = (gSnhWonOpts != NULL && (gSnhWonOpts() & 0x8UL) != 0);    /* the Auto Remounter engaged */
    Boolean quiet;
    char ts[32];
    SnhCollect(srv, &nSrv, usb, &nUsb);
    quiet = (idle && gSnhIdleDenied > 3 && (gSnhIdleDenied % 50UL) != 0);    /* repeated idle refusals: log 1 in 50 */
    if (!quiet) {
        LgStamp(ts, sizeof ts);
        b.n = 0; b.s[0] = 0;
        SlC(&b, "=== sleepnet " kUwVer " (" kUwTag kUwVariant "), "); SlC(&b, ts); SlC(&b, ": ");
        SlC(&b, user ? "a CHOSEN sleep" : idle ? "an IDLE sleep" : "another sleep"); SlC(&b, " (raw type "); SlD(&b, raw);
        SlC(&b, "): "); SlD(&b, nSrv); SlC(&b, " server volume(s), "); SlD(&b, nUsb); SlC(&b, " USB volume(s) mounted; the Auto Remounter ");
        SlC(&b, arm ? "ENGAGED (put away here regardless)" : "not engaged");
        LgNow(b.s);
    }
    if (!user && !idle) { LgNow("  sleepnet:   -> left alone (neither a chosen nor an idle sleep)"); return 0; }
    if (idle) {                                                   /* uwake13: Mac OS's own rule first (it runs after us) */
        SInt16 fsS = 0, fsV = 0; short fs = SnhFileSharing(&fsS, &fsV);
        Boolean ps = SnhPrinterSharing(), ws = SnhWebSharing(), at = SnhAppleTalkActive();
        if (!quiet) {
            b.n = 0; b.s[0] = 0;
            SlC(&b, "  sleepnet:   Mac OS's idle rule (CheckAppleTalk): AppleTalk "); SlC(&b, at ? "active" : "inactive");
            SlC(&b, ", File Sharing "); SlC(&b, fs > 0 ? "ON" : "off"); SlC(&b, " (status "); SlD(&b, fsS);
            SlC(&b, ", version "); SlD(&b, fsV); SlC(&b, "), Printer Sharing "); SlC(&b, ps ? "ON" : "off");
            SlC(&b, ", Web Sharing "); SlC(&b, ws ? "ON" : "off");
            LgNow(b.s);
        }
        if (at && !arm && (fs > 0 || ps || ws)) {
            gSnhIdleDenied++;
            if (!quiet) LgNow("  sleepnet:   -> Mac OS does not take an idle sleep while sharing is on (its own rule, kept): nothing "
                              "put away; the Mac stays awake, only the display sleeps");
            return 0;                                             /* Mac OS refuses it now, silently, as on any Mac */
        }
    }
#ifdef UWAKE_MODE_A
    /* uwake10: mode A keeps Apple's USB stack and the NEC alive through the sleep (the NEC armed so it keeps its state, as
       KeyLargo keeps its USB powered): a USB disk stays mounted, so it is neither put away nor a reason to refuse a sleep,
       idle or chosen. Servers are still put away. */
    if (nUsb > 0 && !quiet) {
        LgNow("  sleepnet:   the USB volume(s) stay mounted: in mode A USB stays alive through the sleep");
        for (i = 0; i < nUsb; i++)                               /* uwake20: which driver each is on (1.1 or 2.0) */
            SnhLogVol(SnhUsbKind(SnhVolDrv(usb[i])) == 2 ? "staying mounted, USB 2.0 (our driver):" :
                      "staying mounted, USB 1.1 (Apple's driver):", usb[i]);
    }
    nUsb = 0;
#endif
    if (idle && nUsb > 0) {                                      /* mode B: never yank a mounted USB volume when idle */
        gSnhIdleDenied++;
        if (!quiet) {
            b.n = 0; b.s[0] = 0;
            SlC(&b, "  sleepnet:   -> IDLE sleep not taken: "); SlD(&b, nUsb);
            SlC(&b, " USB volume(s) mounted (put them away to let the Mac sleep on its own); idle refusals so far ");
            SlU(&b, (unsigned long)gSnhIdleDenied);
            LgNow(b.s);
        }
        return 1;
    }
    if (nSrv == 0 && nUsb == 0) { if (!quiet) LgNow("  sleepnet:   -> nothing to put away: the sleep goes ahead"); return 0; }
    gSnhActed++;
    for (i = 0; i < nSrv; i++) {                                  /* every volume checked first: put none away if one is busy */
        SnhLogVol("server", srv[i]);
        SnhLogErr("  sleepnet:   ReleaseFolder(Trash) (the AutoRemounter's first step; its result is not used):",
                  SnhReleaseTrash(srv[i]));
        SnhWalkVol(srv[i], &w);
        SnhLogWalk(srv[i], &w);
        if (w.onVol > 0) {
            if (idle) {                                           /* no notice for an idle sleep: it is just not taken */
                gSnhIdleDenied++;
                LgNow("  sleepnet:   -> IDLE sleep not taken: a file is open on that server volume (no notice; nothing was "
                      "put away; the Mac stays awake and the idle timer tries again later)");
                return 1;
            }
            SnhRefuse(srv[i], true, kSnhWhyFile, w.on[0].name, 0);
            LgNow("  sleepnet:   -> sleep CANCELLED: a file is open on that server volume; nothing was put away");
            return 1;
        }
    }
    for (i = 0; i < nUsb; i++) {                                  /* walked and logged; UnmountVol decides (SnhFileKind) */
        SnhLogVol("USB", usb[i]);
        SnhWalkVol(usb[i], &w);
        SnhLogWalk(usb[i], &w);
        busy[i] = (Boolean)(w.userOnVol > 0);
        if (busy[i]) SnhLogFcb("open user file (UnmountVol decides; this disk goes first):", &w.user[0]);
    }
    for (i = 0; i < nUsb; i++) if (busy[i])  ord[nOrd++] = usb[i];   /* a likely refusal first, before any put-away */
    for (i = 0; i < nUsb; i++) if (!busy[i]) ord[nOrd++] = usb[i];
    for (i = 0; i < nOrd; i++) {
        SnhLogVol("putting away (UnmountVol) USB", ord[i]);
        e = SnhPutAway(ord[i]);
        SnhLogErr("  sleepnet:   UnmountVol (0 = put away):", e);
        if (e != noErr) { SnhRefuseUsb(ord[i], e, &w, away); return 1; }
        gSnhUsbAway++; away++;
    }
    for (i = 0; i < nSrv; i++) {
        SnhLogVol("putting away (Eject, then UnmountVol) server", srv[i]);
        e = SnhPutAwayServer(srv[i], &ej);
        b.n = 0; b.s[0] = 0;
        SlC(&b, "  sleepnet:   Eject "); SlD(&b, ej); SlC(&b, ", UnmountVol "); SlD(&b, e); SlC(&b, " (0 = put away)");
        LgNow(b.s);
        if (e != noErr) {
            if (idle) {
                gSnhIdleDenied++;
                SnhLogAway(away);
                LgNow("  sleepnet:   -> IDLE sleep not taken: a server volume could not be put away (no notice)");
                return 1;
            }
            SnhRefuse(srv[i], true, kSnhWhyError, NULL, e);
            SnhLogAway(away);
            LgNow("  sleepnet:   -> sleep CANCELLED: a server volume could not be put away");
            return 1;
        }
        gSnhSrvAway++;
    }
#ifdef UWAKE_MODE_A
    LgNow("  sleepnet:   -> the sleep goes ahead (servers put away; USB disks stay mounted)");
#else
    LgNow("  sleepnet:   -> the sleep goes ahead (the USB disks mount again after the wake, once their USB is back)");
#endif
    return 0;
}
/* THE HOOK: HandleSleep+0x168, task level. Returns 0 = sleep on, 1 = cancel. */
static void NmRestart(void);                     /* (below, with the task-level pass) */
static SInt16 UwSleepNetHook(SInt16 type)
{
    gSnhCalls++;
    NmRestart();                                 /* uwake14: the pass, stopped while idle, runs again after this sleep */
#ifdef UWAKE_MODE_A
    (void)KwHookCheck();                         /* uwake14: devices that came or went while the pass was stopped */
#endif
    if (gSnhPrev != NULL) {                                      /* chained hook first: its refusal stands */
        SInt16 r = (SInt16)CallUniversalProc(gSnhPrev, kSleepNetHookProcInfo, type);
        if (r != 0) { gSnhPrevDenied++; return r; }
    }
    return SnhDecide(*(volatile SInt16 *)(gSnhG + kSnhRawType));
}
static void SnhInstall(void)                     /* TASK LEVEL, once, from the first Notification Manager pass */
{
    CFragConnectionID conn = 0; Ptr mainAddr = NULL, tv = NULL, p = NULL; Str255 errName; CFragSymbolClass c = 0;
    UInt32 G, code; const volatile UInt32 *site; int i; THz z; OSErr e;
    if (gSnhTried) return;
    gSnhTried = true;
    G = *(volatile UInt32 *)0x00000D18UL;                                         /* PmgrBase */
    e = GetSharedLibrary("\pNativePowerMgrLib", kPowerPCCFragArch, kReferenceCFrag, &conn, &mainAddr, errName);
    if (e == noErr) e = FindSymbol(conn, "\pPMSimpleSleep", &tv, &c);
    if (G < 0x1000UL || e != noErr || tv == NULL) {
        Lg("  sleepnet: no PmgrBase (%08lX) or no PMSimpleSleep (%d): the put-away before sleep is NOT installed",
           (unsigned long)G, (int)e); return;
    }
    code = *(UInt32 *)tv;                                                         /* the TVector's code address */
    site = (const volatile UInt32 *)(code - kNpmPMSimpleSleep + kNpmHookSite);
    for (i = 0; i < 6; i++) if (site[i] != kSnhSite[i]) break;
    if (i < 6) { Lg("  sleepnet: HandleSleep is not the analysed NativePowerMgrLib (word %d differs): NOT installed", i); return; }
    if (FindSymbol(conn, "\pPMGetWakeOnNetActOptions", &p, &c) == noErr && p != NULL) gSnhWonOpts = (SnhWonOptsFn)p;
    z = GetZone(); SetZone(SystemZone());
    gSnhRD = NewRoutineDescriptor((ProcPtr)UwSleepNetHook, kSleepNetHookProcInfo, GetCurrentArchitecture());
    SetZone(z);
    if (gSnhRD == NULL) { Lg("  sleepnet: routine descriptor allocation FAILED: NOT installed"); return; }
    gSnhG = G;
    gSnhPrev = *(UniversalProcPtr volatile *)(G + kSnhHookSlot);
    __asm__ volatile ("sync" ::: "memory");
    *(UniversalProcPtr volatile *)(G + kSnhHookSlot) = gSnhRD;                    /* publish last */
    Lg("  sleepnet: put-away before sleep INSTALLED at PmgrBase+0x60 (servers + USB volumes); the hook it chains %08lX "
       "(0 = none); Auto Remounter option getter %08lX", (unsigned long)gSnhPrev, (unsigned long)gSnhWonOpts);
}
#ifdef UWAKE_QTEST
/* ⚠ QEMU ONLY. QEMU's power manager is not the analysed one, so the hook is not installed there: this drives SnhDecide
 * itself, once, against a QEMU usb-storage stick (Apple's USB Mass Storage Support mounts it), before the mode's own
 * simulated sleep: a file held open -> a user sleep must be CANCELLED naming it; a file named "AppleShare PDS" held open
 * BY US (QEMU runs no File Sharing) -> the walk classes it File Sharing's and does not refuse, UnmountVol answers -47
 * (it is our open file), so the sleep must be CANCELLED with "File Sharing kept ... busy" (the backstop, and the notice
 * hardware would show if File Sharing does not let go); closed -> an idle sleep must be refused; then a user sleep must
 * put the stick away (its volume gone). The simulated sleep and wake that follow reload USB, and Apple should mount the
 * stick again. */
static Boolean gSnhQDone;
static void SnhQTest(void)
{
    short srv[kSnhMaxVols], usb[kSnhMaxVols]; int nSrv = 0, nUsb = 0; FSSpec sp; short ref = 0; OSErr e; SInt16 r;
    LgNow("=== QTEST sleepnet: the put-away before sleep, driven directly (QEMU only)");
    SnhCollect(srv, &nSrv, usb, &nUsb);
    Lg("  QTEST sleepnet: %d server volume(s), %d USB volume(s) found", nSrv, nUsb); LogFlush();
    if (nUsb == 0) { LgNow("  QTEST sleepnet: NO USB volume -- run QEMU with a usb-storage stick"); return; }
    SnhLogVol("QTEST USB", usb[0]);
    e = FSMakeFSSpec(usb[0], 2L, "\pQTEST open file", &sp);
    if (e == fnfErr) e = FSpCreate(&sp, 'ttxt', 'TEXT', smSystemScript);
    if (e == noErr || e == dupFNErr) e = FSpOpenDF(&sp, fsRdWrPerm, &ref);
    SnhLogErr("  QTEST sleepnet: a file created and held open on the stick (0 = ok):", e);
    /* The expected answers differ by mode: mode B puts USB disks away (and refuses when it cannot); mode A keeps them
       mounted, so a USB disk is never a reason to refuse (uwake10+). Servers: none in QEMU (no AFP server). */
#ifdef UWAKE_MODE_A
#define kQtExpOpen  "(mode A: 0, the stick stays mounted)"
#define kQtExpPds   "(mode A: 0, the stick stays mounted)"
#define kQtExpIdle  "(mode A: 0, the stick stays mounted)"
#define kQtExpLeft  "(mode A: 1, the stick stays mounted)"
#else
#define kQtExpOpen  "(mode B: 1 = cancelled, as it must be)"
#define kQtExpPds   "(mode B: 1 = cancelled by UnmountVol's -47, \"File Sharing kept\")"
#define kQtExpIdle  "(mode B: 1 = refused, as it must be)"
#define kQtExpLeft  "(mode B: 0 = put away)"
#endif
    r = SnhDecide(2);
    SnhLogErr("  QTEST sleepnet: chosen sleep WITH the file open -> " kQtExpOpen, r);
    if (ref) FSClose(ref);
    ref = 0;
    e = FSMakeFSSpec(usb[0], 2L, "\pAppleShare PDS", &sp);
    if (e == fnfErr) e = FSpCreate(&sp, 'pds ', 'MACS', smSystemScript);
    if (e == noErr || e == dupFNErr) e = FSpOpenDF(&sp, fsRdWrPerm, &ref);
    SnhLogErr("  QTEST sleepnet: \"AppleShare PDS\" created and held open on the stick BY US (0 = ok):", e);
    r = SnhDecide(2);
    SnhLogErr("  QTEST sleepnet: chosen sleep WITH it open -> " kQtExpPds, r);
    if (ref) FSClose(ref);
    r = SnhDecide(1);
    SnhLogErr("  QTEST sleepnet: IDLE sleep (type 1) with the stick mounted -> " kQtExpIdle, r);
    r = SnhDecide(12);
    SnhLogErr("  QTEST sleepnet: IDLE sleep (type 12) with the stick mounted -> " kQtExpIdle, r);
    r = SnhDecide(6);
    SnhLogErr("  QTEST sleepnet: a forced sleep (type 6) -> (0 = left alone)", r);
    r = SnhDecide(2);
    SnhLogErr("  QTEST sleepnet: chosen sleep, file closed -> (0 = the sleep goes ahead)", r);
    SnhCollect(srv, &nSrv, usb, &nUsb);
    Lg("  QTEST sleepnet: USB volumes left mounted: %d " kQtExpLeft, nUsb); LogFlush();
}
#endif

/* ---- uwake6, TASK LEVEL: find translations for the recorder (see PhysToLog). GetPhysical is the system's own answer:
   a logical block is kept only when it maps to ONE physical run the whole block long (wherever that run is: QEMU under
   VM shifts by 16 KB; the FW800 runs VM off). The logical blocks within 1 MB of each controller's HCCA and list heads
   (their physical values taken as logical) are searched at the first pass after a wake, before the post-wake freeze
   where the flash's first transfers fall; a block the recorder met later is searched for at the next pass. ---- */
/* uwake7, TASK LEVEL: the map's physical address for logical l (0 = the map has no run there). Linear: the runs are
   sorted by physical start, and this runs only at task level. */
static UInt32 RunLogToPhys(UInt32 l)
{
    UInt32 i, n = gRunN;
    for (i = 0; i < n; i++) if (l - gRunL[i] < gRunLen[i]) return gRunP[i] + (l - gRunL[i]);
    return 0;
}
/* uwake7, TASK LEVEL: compare a fresh GetPhysical of logical block b, page by page, with the map. A block with a page
   that differs drops the map (gMapOff: the recorder stops reading through it at once) and is kept for the report (its
   first wrong page, how many); ValidatePass then takes the map again. */
static void MapCheck(UInt32 b, const LogicalToPhysicalTable *t, unsigned long count)
{
    UInt32 o = 0, nd = 0, l1 = 0, m1 = 0, f1 = 0; short i;
    if (gRunN == 0) return;                             /* no map (not taken, or being taken again) */
    gMapBlocks++;
    for (i = 0; i < (short)count && i < 8 && o < kWinBytes; i++) {
        UInt32 p = (UInt32)t->physical[i].address, len = (UInt32)t->physical[i].count, k;
        if (len == 0 || p == 0) break;
        for (k = 0; k < len && o + k < kWinBytes; k += 0x1000UL) {
            UInt32 mp = RunLogToPhys(b + o + k);
            if (mp == 0) gMapPgNone++;
            else if (mp == p + k) gMapPgSame++;
            else { if (nd++ == 0) { l1 = b + o + k; m1 = mp; f1 = p + k; } gMapPgDiff++; }
        }
        o += len;
    }
    if (nd == 0) return;
    if (gMapDiffBlocks < kDiffMax) {
        UInt32 x = gMapDiffBlocks;
        gDiffL[x] = l1; gDiffM[x] = m1; gDiffF[x] = f1; gDiffMs[x] = RecMs(); gDiffPg[x] = nd; gDiffUsed[x] = gValUsed;
    }
    if (!gMapOff) { gMapOff = 1; __asm__ volatile ("sync" ::: "memory"); Ev(0xFF, kEvMapStale, (UInt16)gMapDiffBlocks, l1); }
    gMapDiffBlocks++;
}
static void ValidateBlock(UInt32 b)
{
    LogicalToPhysicalTable tbl; unsigned long count = 8; OSErr e; short i;
    if (b < kWinBytes || BlockKnown(b)) return;         /* (the first 64 KB is low memory: never a controller's pool) */
    for (i = 0; i < 8; i++) { tbl.physical[i].address = NULL; tbl.physical[i].count = 0; }
    tbl.logical.address = (void *)b; tbl.logical.count = kWinBytes;
    /* GetPhysical wants the range locked (QEMU, VM on: -623 notLockedErr without it); every driver here locks first
       (usb2-ehci/src/ehci_os.c 472-476). With VM off locking changes nothing; with VM on it pins the pages only for
       this call. A lock that fails (an unmapped range) is a refusal. */
    e = LockMemory((void *)b, kWinBytes);
    if (e == noErr) {
        e = GetPhysical(&tbl, &count);
        (void)UnlockMemory((void *)b, kWinBytes);   /* (undoing our own lock: nothing to decide on its result) */
    }
    if (e == noErr) MapCheck(b, &tbl, count);           /* uwake7: is the map still right here? */
    if (e == noErr && count == 1 && tbl.physical[0].count == kWinBytes && tbl.physical[0].address != NULL) {
        if (gWinN < kWinMax) {                          /* publish the pair, then the count */
            gWinL[gWinN] = b; gWinP[gWinN] = (UInt32)tbl.physical[0].address;
            __asm__ volatile ("sync" ::: "memory"); gWinN++;
        }
    } else if (gRejN < kWinMax) {
        gRejP[gRejN] = (e == noErr && count >= 1) ? (UInt32)tbl.physical[0].address : 0UL;
        gRejE[gRejN] = (SInt32)e; gRej[gRejN] = b; __asm__ volatile ("sync" ::: "memory"); gRejN++;
    }
}
static void ValidateAround(UInt32 p)                    /* the blocks within 1 MB of a pointer a controller holds */
{
    UInt32 b = p & ~(kWinBytes - 1UL); long d;
    if (p == 0 || !InRAM(p, 16)) return;
    for (d = -16; d <= 16; d++) {
        UInt32 x = b + (UInt32)(d * (long)kWinBytes);
        if (InRAM(x, kWinBytes)) ValidateBlock(x);
    }
}
/* uwake7, TASK LEVEL, once after boot: the memory map (see RunFind). Every logical megabyte from 64 KB to Gestalt
   'lram' is asked about (MapProbe: LockMemory, GetPhysical with room for 8 runs, UnlockMemory); the scan itself is
   map_scan.h's MsScan (uwake8: a megabyte of hole in one step; scripts/test-map-scan.c runs the same code on the host
   against the FW800's measured map: 3128 calls instead of uwake7B's 17947, the same 3 runs). Adjacent runs merge; the
   result is sorted by physical start, then published (the recorder reads gRunN last). Logged: the runs, the holes, the
   cost. */
static Boolean gMapDone;
static int MapProbe(unsigned long l, unsigned long size, unsigned long ph[8], unsigned long ln[8], unsigned long *count,
                    void *ctx)
{
    LogicalToPhysicalTable tbl; unsigned long c = 8; OSErr e; short i;
    (void)ctx;
    for (i = 0; i < 8; i++) { tbl.physical[i].address = NULL; tbl.physical[i].count = 0; }
    tbl.logical.address = (void *)l; tbl.logical.count = size;
    e = LockMemory((void *)l, size);
    if (e == noErr) {
        e = GetPhysical(&tbl, &c);
        (void)UnlockMemory((void *)l, size);            /* (undoing our own lock: nothing to decide on its result) */
    }
    if (e != noErr) return (int)e;
    for (i = 0; i < 8; i++) { ph[i] = (unsigned long)tbl.physical[i].address; ln[i] = (unsigned long)tbl.physical[i].count; }
    *count = c;
    return 0;
}
static void MapMemory(void)
{
    static UInt32 tl[kRunMax], tp[kRunMax], tn[kRunMax];
    UInt32 end = gLRam & ~(kWinBytes - 1UL), n, calls = 0, holes = 0, ident = 0, a, b;
    int fullScan = 0; Boolean full; UInt32 t0 = kLowTicks;
    gMapDone = true;
#ifndef UWAKE_QTEST_EDVM
    if (!gRamReadable) { Lg("uwake7 memory map: not taken (virtual memory is on: the recorder reads no controller memory)"); return; }
#endif
#ifdef UWAKE_QTEST
    if (end > 0x08000000UL) end = 0x08000000UL;          /* QEMU (VM on): the first 128 MB hold every pool, and
                                                             locking all of a VM-backed space would page it all in */
#endif
    n = MsScan(kWinBytes, end, MapProbe, NULL, tl, tp, tn, kRunMax, &calls, &holes, &fullScan);
    full = fullScan != 0;
    for (a = 1; a < n; a++) {                           /* by physical start */
        UInt32 vl = tl[a], vp = tp[a], vn = tn[a]; b = a;
        while (b > 0 && tp[b - 1] > vp) { tl[b] = tl[b - 1]; tp[b] = tp[b - 1]; tn[b] = tn[b - 1]; b--; }
        tl[b] = vl; tp[b] = vp; tn[b] = vn;
    }
    gRunN = 0;                                          /* (taken again: the recorder sees no map while it is rewritten;
                                                           it interrupts this code, never the reverse) */
    __asm__ volatile ("sync" ::: "memory");
    for (a = 0; a < n; a++) { gRunL[a] = tl[a]; gRunP[a] = tp[a]; gRunLen[a] = tn[a]; if (tl[a] == tp[a]) ident++; }
    __asm__ volatile ("sync" ::: "memory");
    gRunN = n;
    if (gRemapN) Lg("  uwake7 memory map TAKEN AGAIN (#%lu, +%lu ms after the wake step): a page of the last one was wrong",
                    (unsigned long)gRemapN, (unsigned long)RecMs());
    Lg("%suwake7 memory map (GetPhysical over logical %08lX-%08lX): %lu runs (%lu at logical == physical), %lu holes of "
       "64 KB, %lu calls, %lu ticks%s", gRemapN ? "  " : "", (unsigned long)kWinBytes, (unsigned long)(end - 1UL),
       (unsigned long)n, (unsigned long)ident, (unsigned long)holes, (unsigned long)calls, (unsigned long)(kLowTicks - t0),
       full ? "; !! the run table filled: the rest is unmapped for the recorder" : "");
    for (a = 0; a < n && a < 48; a++)
        Lg("  physical %08lX-%08lX <- logical %08lX-%08lX%s", (unsigned long)tp[a], (unsigned long)(tp[a] + tn[a] - 1UL),
           (unsigned long)tl[a], (unsigned long)(tl[a] + tn[a] - 1UL), tl[a] == tp[a] ? " (the same)" : "");
    if (n > 48) Lg("  (%lu more runs)", (unsigned long)(n - 48));
#ifdef UWAKE_QTEST_STALE
    /* ⚠ QEMU ONLY, fault injection: the STARTUP map made wrong on purpose, as a remapping would leave it. The run with
       the lowest physical start is moved 64 KB up in logical memory (one 64 KB shorter, so it stays inside its own
       logical range: every read through it still lands in RAM, just the wrong RAM). The map check must find it at the
       wake's first task-level pass, drop it, take the map again, and the reads must come right. */
    if (gRemapN == 0 && n > 0 && gRunLen[0] > 2UL * kWinBytes) {
        gRunN = 0; __asm__ volatile ("sync" ::: "memory");
        gRunL[0] += kWinBytes; gRunLen[0] -= kWinBytes;
        __asm__ volatile ("sync" ::: "memory"); gRunN = n;
        Lg("!! QTEST_STALE: the map's first run made WRONG on purpose (now physical %08lX <- logical %08lX): the check "
           "after the wake must find it", (unsigned long)gRunP[0], (unsigned long)gRunL[0]);
    }
#endif
    LogFlush();
}
static void ValidatePass(void)                          /* TASK LEVEL, each Notification Manager pass while recording */
{
    static const unsigned long kOff[3] = { kHcHCCA, kHcControlHeadED, kHcBulkHeadED };
    short i, k, j;
    gValUsed = true;                                    /* uwake7: first the blocks read through the map (they decide */
    while (gUsedR != gUsedW) { ValidateBlock(gUsed[gUsedR % kCandMax]); gUsedR++; }   /* whether a reading stands) */
    gValUsed = false;
    for (i = 0, k = 0; i < gNecN && k < 2; i++) if (gNec[i].ohci) {
        NecFn *n = &gNec[i];
        if (n->rec.live) for (j = 0; j < 3; j++) {
            UInt32 p = OhciRd(n->base, kOff[j]) & ~0xFUL;
            if ((p & ~(kWinBytes - 1UL)) != (gValLast[k][j] & ~(kWinBytes - 1UL))) { gValLast[k][j] = p; ValidateAround(p); }
        }
        k++;
    }
    while (gCandR != gCandW) { ValidateAround(gCand[gCandR % kCandMax]); gCandR++; }   /* (physical blocks met) */
    if (gMapOff && gRemapN < kRemapMax) {               /* uwake7: a page of the map was wrong: take the map again */
        gRemapN++;
        MapMemory();
        if (gRunN > 0) { __asm__ volatile ("sync" ::: "memory"); gMapOff = 0; }
        Ev(0xFF, kEvMapRemap, (UInt16)gRemapN, gRunN);
    }
}

#ifdef UWAKE_QTEST
/* ⚠ QEMU ONLY, TASK LEVEL, once after the simulated wake. QEMU's OHCI empties HcDoneHead within the frame, so the
   recorder never catches a retired TD there. Prove the whole read path on two TDs of known contents instead: our own
   block is translated by the system (GetPhysical: under QEMU's VM it is NOT logical == physical), the TDs are written
   as a controller writes them (little-endian, linked by PHYSICAL address), and TdChain is handed the physical address,
   exactly as it is handed HcDoneHead: #0 CC 4 Stall, EC 1, toggle 3, DI 2, IN, CBP 12345670, BE 1234567F, then #1:
   CC 0, SETUP, DI 7, CBP 0000A000, BE 0000A007, end of chain. The report must decode exactly that. */
static Boolean gQtdDone;
static void QtdSelfCheck(void)
{
    static UInt32 qtd[8] __attribute__((aligned(16)));
    UInt32 lb, p0 = 0, i;
    gQtdDone = true;
    lb = (UInt32)&qtd[0] & ~(kWinBytes - 1UL);
    if (((UInt32)&qtd[7] & ~(kWinBytes - 1UL)) != lb) { Lg("  QTEST TD reader: the test TDs straddle a block: skipped"); return; }
    ValidateBlock(lb);
    for (i = 0; i < gWinN; i++) if (gWinL[i] == lb) p0 = gWinP[i] + ((UInt32)&qtd[0] - lb);
    if (p0 == 0) { Lg("  QTEST TD reader: our own block got no translation: skipped"); return; }
    qtd[0] = Swap32(0x47540000UL); qtd[1] = Swap32(0x12345670UL); qtd[2] = Swap32(p0 + 16UL); qtd[3] = Swap32(0x1234567FUL);
    qtd[4] = Swap32(0x00E00000UL); qtd[5] = Swap32(0x0000A000UL); qtd[6] = 0;                qtd[7] = Swap32(0x0000A007UL);
    Lg("  QTEST TD reader: two known TDs at logical %08lX = physical %08lX (the report must decode them)",
       (unsigned long)(UInt32)&qtd[0], (unsigned long)p0);
    TdChain(1, p0);
}
#endif

/* TASK LEVEL: how many resident drivers of THIS extension are installed (any build: they all carry the driver name
   "FW800USBWake" and the DriverDescription nameInfoStr "uwak,resdnt"). An old copy from before uwake6 cannot stand
   down, so it installs beside us (QEMU, 2026-11-12: uwake6B + uwake5B both installed and both logged). */
static short CountOurDrivers(void)
{
    static const unsigned char kName[] = "\pFW800USBWake", kInfo[] = "\puwak,resdnt";
    DriverRefNum refs[64]; ItemCount got; UnitNumber hi = HighestUnitNumber(), u = 0; short n = 0, i;
    for (;;) {
        UnitNumber end = (UnitNumber)(((unsigned long)u + 63UL > (unsigned long)hi) ? hi : (UnitNumber)(u + 63));
        got = 64;
        if (LookupDrivers(u, end, false, &got, refs) != noErr) return (short)-1;
        for (i = 0; i < (short)got && i < 64; i++) {
            UnitNumber unit = 0; DriverFlags flags = 0; DriverOpenCount count = 0; Str255 name; RegEntryID dev;
            CFragSystem7Locator loc; CFragConnectionID conn = 0; DriverEntryPointPtr ep = 0; DriverDescription desc;
            memset(&loc, 0, sizeof loc); memset(&desc, 0, sizeof desc); name[0] = 0;
            RegistryEntryIDInit(&dev);
            if (GetDriverInformation(refs[i], &unit, &flags, &count, name, &dev, &loc, &conn, &ep, &desc) == noErr &&
                (PStrEq(name, kName) || PStrEq(desc.driverType.nameInfoStr, kInfo))) n++;
            RegistryEntryIDDispose(&dev);
        }
        if (end >= hi) break;
        u = (UnitNumber)(end + 1);
    }
    return n;
}
/* TASK LEVEL, once (the first Notification Manager pass, after every extension has loaded): another copy of this
   extension? A uwake6+ copy that loaded second stood down and counted itself (InstallMe's kDupProp); an older copy
   cannot, and runs too (CountOurDrivers). Either way the log says so with "!!", which the runbooks treat as stop. */
static Boolean gDupChecked;
static void DupCheck(void)
{
    RegEntryID me; UInt32 n = 0; RegPropertyValueSize n4 = sizeof n; short running;
    gDupChecked = true;
    RegistryEntryIDInit(&me);
    if (RegistryCStrEntryLookup(NULL, (const RegCStrPathName *)"Devices:FW800USBWake", &me) == noErr &&
        RegistryPropertyGet(&me, (RegPropertyName *)"uwake,stood-down", &n, &n4) == noErr && n > 0)
        Lg("!! %lu other copy(ies) of this extension found at startup: they installed nothing, this one runs. Keep ONE "
           "file in Extensions (\"Sleep Assistant\"; remove \"FW800 Sleep Assistant\", \"FW800 USB Wake\", \"FW800USBWake\").",
           (unsigned long)n);
    RegistryEntryIDDispose(&me);
    running = CountOurDrivers();
    if (running > 1)
        Lg("!! %d copies of this extension are RUNNING (an older one cannot stand down): remove the old file (\"FW800 USB "
           "Wake\" or \"FW800USBWake\") from Extensions, then restart. Its own log is \"FW800 USB Wake Log\".", running);
    /* (a uwake6+ copy under the old name "FW800 Sleep Assistant" loads BEFORE "Sleep Assistant", alphabetically: then IT
       runs and this file stands down; its own log is "FW800 Sleep Assistant Log") */
    else if (running < 0)
        Lg("  (could not list the installed drivers to check for a second copy)");
}

/* ============================================================== the task-level pass (Notification Manager) */
static NMRecPtr gNM;
static UInt32   gNmPasses, gNmLast;       /* (gBootTicks, gInstalled: with the wake counters, for uwake11's survey) */
static UInt32   gNmWakeSeen, gNmWakeTicks, gNmPassesSeen, gNmStall;   /* the report's safety net */

/* ---- uwake14: the pass runs only while it has work. EVERY NMInstall counts as system activity: Mac68KROM's
   _NMInstall calls _IdleUpdate (Apple, 1991: "to wake the system up when NM procs want to run") and NativePowerMgrLib
   gives the system-sleep timer 10 s back for it, at most once per 10 s (usb-wake/NPM-IDLE-TIMER.md). uwake1 to uwake13
   re-armed this pass at the end of every pass, i.e. every event-loop turn, so the system timer never ran out: "only the
   display sleeps". Now it re-arms only while there is work (the boot's one-time steps, the keyboard-wake survey, a wake
   to record and report, QEMU's scheduled tests). Otherwise it stops, and the put-away hook (task level, inside
   HandleSleep, before every sleep) arms it again, so the first pass after the wake comes as before. With no hook (a
   power manager that is not the analysed one) it keeps running: nothing else would bring it back after a wake. ---- */
static volatile Boolean gNmStopped;
static UInt32  gNmStops, gNmRestarts;
static Boolean gClkTried;
#ifndef UWAKE_QTEST
static Boolean gNmNoHookSaid;                  /* (QEMU builds stop without the hook: see the end of NmPass) */
#endif

typedef OSErr   (*ClkGetLastFn)(void *info);   /* NativePowerMgrLib's exports: CFM calls, TASK LEVEL only */
typedef UInt8   (*ClkByteFn)(void);
typedef Boolean (*ClkBoolFn)(void);
static ClkGetLastFn gClkGetLast;
static ClkByteFn    gClkSleepTO, gClkDimTO;
static ClkBoolFn    gClkAutoOff;
static UInt32       gClkG;                     /* PmgrBase once the offsets checked out (the task-level readings) */
static UInt32       gClkWinSamples0;

static UInt32 ClkApi(short type, Boolean *ok)  /* TASK LEVEL: GetLastActivity. ActivityInfo has the 68K layout (SInt16
                                                   type, the UInt32 time at offset 2); a PowerPC struct would pad to 4. */
{
    UInt8 info[8];
    memset(info, 0, sizeof info);
    info[0] = (UInt8)(((UInt16)type) >> 8); info[1] = (UInt8)type;
    *ok = (gClkGetLast != NULL && gClkGetLast(info) == noErr);
    return ((UInt32)info[2] << 24) | ((UInt32)info[3] << 16) | ((UInt32)info[4] << 8) | (UInt32)info[5];
}
static void ClkAge(UInt32 now, UInt32 v, char *b, size_t max)
{
    if (v == 0) snprintf(b, max, "never");
    else if ((SInt32)(now - v) >= 0) snprintf(b, max, "%lu s ago", (unsigned long)((now - v) / 60UL));
    else snprintf(b, max, "%lu s AHEAD", (unsigned long)((v - now) / 60UL));
}
static void ClkLogNow(const char *what)        /* TASK LEVEL: the four clocks, Energy Saver's timeouts, the gates */
{
    char a[kClkN][24], s[112]; UInt32 now = kLowTicks, v[kClkN]; int i; Boolean ok = true;
    UInt8 sto = 0, dto = 0;
    for (i = 0; i < kClkN; i++) {
        v[i] = gClkG ? *(volatile UInt32 *)(gClkG + kClkOff[i]) : ClkApi((short)i, &ok);
        ClkAge(now, v[i], a[i], sizeof a[i]);
    }
    Lg("  uwake14 %s: Mac OS's activity clocks: system %s, keyboard/mouse %s, network %s, disk %s",
       what, a[kClkSys], a[kClkUser], a[kClkNet], a[kClkDisk]);
    if (gClkSleepTO) sto = gClkSleepTO();
    if (gClkDimTO) dto = gClkDimTO();
    if (gClkSleepTO == NULL) snprintf(s, sizeof s, "the system-sleep timeout could not be read");
    else if (sto == 0) snprintf(s, sizeof s, "the system NEVER sleeps when idle");
    else snprintf(s, sizeof s, "the system sleeps %u min %u s after the system clock (%ld s from now)",
                  (unsigned)(sto * 15U / 60U), (unsigned)(sto * 15U % 60U),
                  (long)sto * 15L - (long)((SInt32)(now - v[kClkSys]) / 60L));
    if (gClkG)
        Lg("  uwake14 %s: Energy Saver: %s; the display after %u min %u s of no keyboard/mouse; automatic sleep %s "
           "(AutoSleepControl count %d); Mac OS's other idle-sleep gate (PmgrBase+0x0F bit 0 with +0x10 bit 1) %s",
           what, s, (unsigned)(dto * 15U / 60U), (unsigned)(dto * 15U % 60U),
           (gClkAutoOff && gClkAutoOff()) ? "DISABLED by software" : "allowed",
           (int)*(volatile SInt8 *)(gClkG + 0xF8UL),
           ((*(volatile UInt8 *)(gClkG + 0x0FUL) & 1U) && (*(volatile UInt8 *)(gClkG + 0x10UL) & 2U)) ? "CLOSED" : "open");
    else
        Lg("  uwake14 %s: Energy Saver: %s; the display after %u min %u s; automatic sleep %s", what, s,
           (unsigned)(dto * 15U / 60U), (unsigned)(dto * 15U % 60U),
           (gClkAutoOff && gClkAutoOff()) ? "DISABLED by software" : "allowed");
}
static void ClkLogWindow(const char *what)     /* TASK LEVEL: what moved the clocks in the window (since the last stop or
                                                   restart), from the heartbeat's counters and ring */
{
    const volatile ClkRec *c = gClk; UInt32 now = kLowTicks, since, n, j, idx[8]; int k = 0, m = 0; char b[200];
    if (c == NULL || c->G == 0) { Lg("  uwake14 %s: (Mac OS's activity clocks were not watched: see the boot lines)", what); return; }
    since = c->winSince;
    Lg("  uwake14 %s: in the %lu min %lu s %s (%lu samples): background reports moved the system clock %lu time(s) "
       "(%lu with the network clock, %lu with the disk clock, %lu alone: e.g. a Notification Manager request); "
       "keyboard/mouse activity %lu time(s)", what, (unsigned long)((now - since) / 3600UL),
       (unsigned long)((now - since) / 60UL % 60UL), c->quiet ? "since the pass stopped" : "with the pass RUNNING",
       (unsigned long)(c->samples - gClkWinSamples0), (unsigned long)c->winSysBg, (unsigned long)c->winBgNet,
       (unsigned long)c->winBgDisk, (unsigned long)c->winBgGen, (unsigned long)c->winUser);
    if (c->winSysBg > 0)
        Lg("    the first %lu s into the window, the last %lu s; %s%lu to %lu s apart", (unsigned long)((c->winFirstBg - since) / 60UL),
           (unsigned long)((c->winLastBg - since) / 60UL), c->winSysBg > 1 ? "" : "(one) ",
           (unsigned long)(c->winMinGap / 60UL), (unsigned long)(c->winMaxGap / 60UL));
    for (n = c->events, j = 0; j < kClkRingN && j < n && m < 8; j++) {          /* the last 8 in the window, newest first */
        const volatile ClkEv *e = &c->ev[(n - 1UL - j) % kClkRingN];
        if ((SInt32)(e->tick - since) < 0) break;
        idx[m++] = (n - 1UL - j) % kClkRingN;
    }
    b[0] = 0;
    while (m > 0 && k < (int)sizeof b - 40) {
        const volatile ClkEv *e = &c->ev[idx[--m]];
        k += snprintf(b + k, sizeof b - (size_t)k, "%s+%lus %s%s%s%s", k ? ", " : "", (unsigned long)((e->tick - since) / 60UL),
                      (e->moved & 1U) ? "sys" : "", (e->moved & 4U) ? "+net" : "", (e->moved & 8U) ? "+disk" : "",
                      (e->sysAfter - e->sysBefore == 600UL) ? "(+10s)" : "");
    }
    if (b[0]) Lg("    the last moves (seconds into the window): %s", b);
}
static void ClkWindowStart(Boolean quiet)      /* TASK LEVEL: the window's counters start again from now */
{
    ClkRec *c = gClk; int i;
    if (c == NULL || c->G == 0) return;
    c->quiet = quiet ? 1UL : 0UL;
    c->winSysBg = c->winBgGen = c->winBgNet = c->winBgDisk = 0;
    c->winUser = c->winNet = c->winDisk = 0;
    c->winFirstBg = c->winLastBg = c->winMinGap = c->winMaxGap = c->winLastUser = 0;
    for (i = 0; i < kClkN; i++) c->last[i] = *(volatile UInt32 *)(c->G + kClkOff[i]);   /* (moves before now: not ours) */
    gClkWinSamples0 = c->samples;
    c->winSince = kLowTicks;
}
static void ClkInstall(void)                   /* TASK LEVEL, once, from the first pass (after SnhInstall) */
{
    CFragConnectionID conn = 0; Ptr mainAddr = NULL, p, blk; Str255 errName; CFragSymbolClass cl = 0; OSErr e;
    UInt32 G = *(volatile UInt32 *)0x00000D18UL, a = 0, b = 0, api = 0; int i, k, bad = -1; Boolean ok = false; ClkRec *c;
    gClkTried = true;
    e = GetSharedLibrary("\pNativePowerMgrLib", kPowerPCCFragArch, kReferenceCFrag, &conn, &mainAddr, errName);
    if (e == noErr) {
        p = NULL; if (FindSymbol(conn, "\pGetLastActivity", &p, &cl) == noErr && p != NULL) gClkGetLast = (ClkGetLastFn)p;
        p = NULL; if (FindSymbol(conn, "\pGetSleepTimeout", &p, &cl) == noErr && p != NULL) gClkSleepTO = (ClkByteFn)p;
        p = NULL; if (FindSymbol(conn, "\pGetDimmingTimeout", &p, &cl) == noErr && p != NULL) gClkDimTO = (ClkByteFn)p;
        p = NULL; if (FindSymbol(conn, "\pIsAutoSlpControlDisabled", &p, &cl) == noErr && p != NULL) gClkAutoOff = (ClkBoolFn)p;
    }
    if (G < 0x1000UL || gClkGetLast == NULL) {
        Lg("!! uwake14: no PmgrBase (%08lX) or no GetLastActivity (%d): Mac OS's activity clocks are not watched",
           (unsigned long)G, (int)e);
        return;
    }
    for (i = 0; i < kClkN && bad < 0; i++) {
        for (k = 0; k < 3; k++) {                                  /* a clock can move between the reads: three tries */
            a = *(volatile UInt32 *)(G + kClkOff[i]); api = ClkApi((short)i, &ok); b = *(volatile UInt32 *)(G + kClkOff[i]);
            if (ok && a == b && api == a) break;
        }
        if (k == 3) bad = i;
    }
    if (bad >= 0) {
        Lg("!! uwake14: activity clock %d is not at PmgrBase+%03X (GetLastActivity %s %08lX, memory %08lX): not watched "
           "(a power manager other than the analysed ones)", bad, (unsigned)kClkOff[bad], ok ? "says" : "FAILED,",
           (unsigned long)api, (unsigned long)a);
        ClkLogNow("at boot");
        return;
    }
    gClkG = G;
    blk = NewPtrSysClear((Size)(sizeof(ClkRec) + 4096));
    if (blk == NULL) { Lg("!! uwake14: no memory for the activity-clock record: not watched"); ClkLogNow("at boot"); return; }
    c = (ClkRec *)(((UInt32)blk + 4095UL) & ~4095UL);             /* one page: a QEMU memory dump finds it whole */
    c->magic[0] = 0x55573134UL; c->magic[1] = 0x434C4B31UL;       /* 'UW14' 'CLK1' */
    for (i = 0; i < kClkN; i++) c->last[i] = *(volatile UInt32 *)(G + kClkOff[i]);
    c->winSince = kLowTicks;
    gClk = c;
    __asm__ volatile ("sync" ::: "memory");
    c->G = G;                                                      /* publish last: the heartbeat samples from here */
    Lg("  uwake14: Mac OS's activity clocks found at PmgrBase %08lX +1A/+188/+18C/+1E (each checked against "
       "NativePowerMgrLib's GetLastActivity); the heartbeat watches them every 250 ms (record at %08lX)",
       (unsigned long)G, (unsigned long)c);
    ClkLogNow("at boot");
}

/* ---- uwake15: the request recorder's task-level side: install the patch, name the posters, log the window ---- */
static Boolean gSpyTried;
static UInt32  gSpyWinSince;
static UInt32 SpySysLim(UInt32 *base);         /* (below) */
static void SpyInstall(void)                   /* TASK LEVEL, once, from the first pass */
{
    UniversalProcPtr o;
    gSpyTried = true;
    o = NGetTrapAddress(0xA05E, OSTrap);                           /* _NMInstall */
    if (o == NULL) { Lg("!! uwake15: _NMInstall's address read NULL: requests are not recorded"); return; }
    gSpyOrig = o;
    gSpyWinSince = kLowTicks;
    /* the procedure pointer set in code, as the companion's trap patches and NewRoutineDescriptor do */
    gSpyRD.routineRecords[0].procDescriptor = (ProcPtr)SpyNMInstall;
    __asm__ volatile ("sync" ::: "memory");
    NSetTrapAddress((UniversalProcPtr)&gSpyRD, 0xA05E, OSTrap);
    {
        UInt32 sz, lim = SpySysLim(&sz);
        Lg("  uwake15: _NMInstall patched (the one it replaced: %08lX): every Notification Manager request on this Mac is "
           "recorded by its owner, and the requests that move Mac OS's system clock are named (system heap %08lX to %08lX)",
           (unsigned long)o, (unsigned long)sz, (unsigned long)lim);
    }
}
static Boolean SpyReadable(UInt32 a, UInt32 len)   /* an address we may read: RAM, or the Mac OS ROM image */
{
    UInt32 top = gActTop ? gActTop : gLRam;     /* uwake16: the memory map's top once known ('lram' overstates it) */
    if (a + len < a) return false;
    if (a >= 0x3000UL && a + len <= top) return true;
    return (a >= 0xFFC00000UL && a + len <= 0xFFFFFFF0UL);
}
static UInt32 SpySysLim(UInt32 *base)            /* SysZone (LM 0x2A6) and its bkLim (the zone's first long); 0 = unreadable */
{
    UInt32 sz = *(volatile UInt32 *)0x02A6UL;                      /* (it can sit below 0x3000: no SpyReadable here) */
    *base = sz;
    if (sz < 0x1000UL || sz >= gLRam || (sz & 1UL)) return 0;
    return *(volatile UInt32 *)sz;
}
static const char *SpyWhere(UInt32 a)
{
    UInt32 sz, lim = SpySysLim(&sz);
    if (a >= 0xFFC00000UL) return "the Mac OS ROM";
    if (lim && a >= sz && a < lim) return "the system heap";
    return "RAM outside the system heap";
}
static Boolean SpyTbName(UInt32 code, char *out, size_t max)   /* TASK LEVEL: a PowerPC function's traceback name */
{
    UInt32 p, q, tboff, len, i;
    if ((code & 3UL) || !SpyReadable(code, 4) || max < 2) return false;
    for (p = code; p < code + 0x10000UL && SpyReadable(p, 96); p += 4) {
        const volatile UInt8 *b = (const volatile UInt8 *)p;
        if (*(const volatile UInt32 *)p != 0 || b[4] != 0) continue;     /* a zero word, then table version 0 */
        if (!(b[6] & 0x20) || !(b[7] & 0x40)) continue;                   /* it has the offset back and a name */
        q = p + 12;
        if (b[10] || (b[11] >> 1)) q += 4;                                /* parameter info */
        tboff = *(const volatile UInt32 *)q; q += 4;
        if (b[6] & 0x10) q += 4;                                          /* interrupt-handler mask */
        if (b[6] & 0x08) { UInt32 nc = *(const volatile UInt32 *)q; if (nc > 16) continue; q += 4 + 4 * nc; }
        if (!SpyReadable(q, 2)) continue;
        len = *(const volatile UInt16 *)q;
        if (tboff == 0 || tboff > p || p - tboff > code || len == 0 || len > 63 || !SpyReadable(q + 2, len)) continue;
        for (i = 0; i < len; i++) { UInt8 ch = *(const volatile UInt8 *)(q + 2 + i); if (ch < 32 || ch > 126) break; }
        if (i < len) continue;
        if (len > max - 1) len = (UInt32)max - 1;
        for (i = 0; i < len; i++) out[i] = (char)*(const volatile UInt8 *)(q + 2 + i);
        out[len] = 0;
        return true;
    }
    return false;
}
static Boolean Spy68kName(UInt32 a, char *out, size_t max)    /* TASK LEVEL: a 68K routine's MacsBug name */
{
    UInt32 p, nb, len, i;
    if ((a & 1UL) || !SpyReadable(a, 2) || max < 2) return false;
    for (p = a; p < a + 0x1000UL && SpyReadable(p, 72); p += 2) {
        UInt16 w = *(const volatile UInt16 *)p; UInt8 b;
        if (w == 0x4E75 || w == 0x4ED0) nb = p + 2;                       /* RTS, JMP (A0) */
        else if (w == 0x4E74) nb = p + 4;                                  /* RTD #n */
        else continue;
        b = *(const volatile UInt8 *)nb;
        if (b >= 0x80 && b <= 0x9F) {                                      /* $80 + length, or $80 then a length byte */
            len = b & 0x1FU; nb++;
            if (len == 0) { len = *(const volatile UInt8 *)nb; nb++; }
        } else if (b >= 0xC1 && b <= 0xFA) {                               /* the old fixed 8: first char's high bit set */
            len = 8;
        } else continue;
        if (len == 0 || len > 63) continue;
        for (i = 0; i < len; i++) {
            UInt8 ch = (UInt8)(*(const volatile UInt8 *)(nb + i) & (i == 0 && len == 8 ? 0x7F : 0xFF));
            if (ch < 32 || ch > 126) break;
        }
        if (i < len) continue;
        if (len > max - 1) len = (UInt32)max - 1;
        for (i = 0; i < len; i++) out[i] = (char)(*(const volatile UInt8 *)(nb + i) & (i == 0 ? 0x7F : 0xFF));
        out[len] = 0;
        return true;
    }
    return false;
}
static void SpyRespName(UInt32 r, char *out, size_t max)       /* TASK LEVEL: what the response routine is */
{
    char nm[64]; UInt32 code = 0;
    if (r == 0) { snprintf(out, max, "none"); return; }
    if (r == 0xFFFFFFFFUL) { snprintf(out, max, "-1 (removed once shown)"); return; }
    /* a routine descriptor (_MixedModeMagic): MixedMode.h's layout, a 12-byte header (magic, version, flags, a long
       and two bytes reserved, routineCount) then routineRecords[0]: procInfo +12, ISA +17, routineFlags +18,
       procDescriptor +20 (for PowerPC a transition vector: code, TOC). QEMU caught the first try reading +13/+16. */
    if (SpyReadable(r, 24) && *(const volatile UInt16 *)r == 0xAAFEU) {
        UInt8 isa = (UInt8)(*(const volatile UInt8 *)(r + 17) & 0x0FU);           /* ISA (the RTA is the high nibble) */
        UInt16 fl = *(const volatile UInt16 *)(r + 18);
        UInt32 pd = *(const volatile UInt32 *)(r + 20);
        if (isa == 1 && !(fl & 0x0021U) && SpyReadable(pd, 4)) {                  /* PowerPC, absolute, a ProcPtr */
            code = *(const volatile UInt32 *)pd;
            if (SpyTbName(code, nm, sizeof nm))
                snprintf(out, max, "%08lX: PowerPC %s, code %08lX in %s", (unsigned long)r, nm, (unsigned long)code, SpyWhere(code));
            else snprintf(out, max, "%08lX: PowerPC (no name), code %08lX in %s", (unsigned long)r, (unsigned long)code, SpyWhere(code));
            return;
        }
        /* 68K: a MacsBug name follows a routine's RTS; the first one after the entry is its own only if the routine
           has a name and no earlier RTS, so it is labelled as what it is */
        if (isa == 0 && Spy68kName(pd, nm, sizeof nm)) {
            snprintf(out, max, "%08lX: 68K at %08lX in %s, the next MacsBug name after it %s", (unsigned long)r,
                     (unsigned long)pd, SpyWhere(pd), nm);
            return;
        }
        snprintf(out, max, "%08lX: a routine descriptor (ISA %u, flags %04X) to %08lX in %s", (unsigned long)r,
                 (unsigned)isa, (unsigned)fl, (unsigned long)pd, SpyWhere(pd));
        return;
    }
    if (Spy68kName(r, nm, sizeof nm))
        snprintf(out, max, "%08lX: 68K in %s, the next MacsBug name after it %s", (unsigned long)r, SpyWhere(r), nm);
    else snprintf(out, max, "%08lX: 68K (no MacsBug name within 4 KB) in %s", (unsigned long)r, SpyWhere(r));
}
static void SpyRefCon(UInt32 v, char *out, size_t max)
{
    UInt8 c0 = (UInt8)(v >> 24), c1 = (UInt8)(v >> 16), c2 = (UInt8)(v >> 8), c3 = (UInt8)v;
    if (c0 >= 32 && c0 < 127 && c1 >= 32 && c1 < 127 && c2 >= 32 && c2 < 127 && c3 >= 32 && c3 < 127)
        snprintf(out, max, "'%c%c%c%c' (%08lX)", c0, c1, c2, c3, (unsigned long)v);
    else snprintf(out, max, "refCon %08lX", (unsigned long)v);
}
static void SpyWindowStart(void)               /* TASK LEVEL: the per-window counts start again (identities stay) */
{
    UInt32 i;
    for (i = 0; i < kSpyN; i++) { gSpy[i].n = 0; gSpy[i].pushes = 0; gSpy[i].quiet = 0; gSpy[i].minGap = gSpy[i].maxGap = 0; }
    gSpyCalls = gSpyPushes = gSpyFull = gSpyQuiet = 0;
    gSpyWinSince = kLowTicks;
}
static void SpyLogWindow(const char *what, Boolean withRing)    /* TASK LEVEL */
{
    UInt32 n = gSpyN, i, since = gSpyWinSince; char rc[32], rs[200], when[96];
    if (gSpyOrig == NULL) { Lg("  uwake15 %s: (Notification Manager requests were not recorded: see the boot lines)", what); return; }
    if (n > kSpyN) n = kSpyN;
    Lg("  uwake15 %s: %lu Notification Manager request(s) in the window, from %lu poster(s) seen since boot%s; requests "
       "moved the system clock %lu time(s); uwake21: %lu silent request(s) not counted as activity", what,
       (unsigned long)gSpyCalls, (unsigned long)n, gSpyFull ? " (the table was full: some not recorded)" : "",
       (unsigned long)gSpyPushes, (unsigned long)gSpyQuiet);
    for (i = 0; i < n; i++) {
        const SpyPoster *p = &gSpy[i];
        if (p->n == 0) continue;
        SpyRefCon(p->refCon, rc, sizeof rc);
        SpyRespName(p->resp, rs, sizeof rs);
        if (p->n == 1) snprintf(when, sizeof when, "once, %lu s into the window", (unsigned long)((p->first - since) / 60UL));
        else snprintf(when, sizeof when, "%lu.%lu to %lu.%lu s apart (first %lu s into the window, last %lu s)",
                      (unsigned long)(p->minGap / 60UL), (unsigned long)(p->minGap % 60UL / 6UL),
                      (unsigned long)(p->maxGap / 60UL), (unsigned long)(p->maxGap % 60UL / 6UL),
                      (unsigned long)((p->first - since) / 60UL), (unsigned long)((p->last - since) / 60UL));
        Lg("    #%lu %s%s: %lu request(s), %s; moved the system clock %lu time(s); not counted as activity %lu time(s)",
           (unsigned long)(i + 1), rc, (p->str || p->snd || p->icon || p->mark) ? ", a notice the user sees" : ", silent",
           (unsigned long)p->n, when, (unsigned long)p->pushes, (unsigned long)p->quiet);
        Lg("       response %s; NMRec %08lX in %s", rs, (unsigned long)p->rec, SpyWhere(p->rec));
    }
    if (withRing && gClk != NULL && gClk->G != 0) {               /* each system-clock move in the window, and its cause */
        const volatile ClkRec *c = gClk; UInt32 ev = c->events, j, k, cs = c->winSince, shown = 0; char b[240]; int m = 0;
        UInt32 first = (ev > kClkRingN) ? ev - kClkRingN : 0;
        b[0] = 0;
        for (j = first; j < ev; j++) {
            const volatile ClkEv *e = &c->ev[j % kClkRingN];
            if (!(e->moved & 1U) || (SInt32)(e->tick - cs) < 0) continue;
            k = e->who;
            m += snprintf(b + m, sizeof b - (size_t)m, "%s+%lu%s", m ? " " : "", (unsigned long)((e->tick - cs) / 60UL),
                          k ? "" : "?");
            if (k) m += snprintf(b + m, sizeof b - (size_t)m, "#%lu", (unsigned long)k);
            shown++;
            if (m > (int)sizeof b - 24) { Lg("    system-clock moves (s into the window, #poster; ? = no request): %s", b); b[0] = 0; m = 0; }
        }
        if (m) Lg("    system-clock moves (s into the window, #poster; ? = no request): %s", b);
        if (shown == 0) Lg("    (no system-clock move in the window's part of the ring)");
    }
}

/* ---- uwake16: the activity hook's task-level side: install it, name the routes, log the window, and measure which
   File Manager calls report activity ---- */
static Boolean gActTried;
static UInt32  gActWinSince, gActSlot;
static UInt32 *gActTramp;
static UInt32 ActMapTop(void)                  /* TASK LEVEL: the end of the logical RAM the memory map covers without a
                                                  gap from 64 KB up (the FW800: 5FF70000); 0 = no map */
{
    UInt32 top = 0x10000UL, i, n = gRunN; Boolean grew = true;
    if (n == 0) return 0;
    if (n > kRunMax) n = kRunMax;
    while (grew) {
        grew = false;
        for (i = 0; i < n; i++)
            if (gRunL[i] <= top && gRunLen[i] > top - gRunL[i]) { top = gRunL[i] + gRunLen[i]; grew = true; }
    }
    return top;
}
static void ActInstall(void)                   /* TASK LEVEL, once, from the first pass (after ClkInstall and the map) */
{
    UInt32 *tv, toc, a, found = 0, nFound = 0, code = 0, *t, hv; char nm[64];
    gActTried = true;
    gActReadStack = gRamReadable;
#ifdef UWAKE_QTEST_READVM
    /* ⚠ QEMU fault injection only: read the stack although VM is on. q16K2 and q16K3 FROZE ~30 s after the simulated
       wake with this on, q16N1 (the same build without it) did not: a report from inside a disk request (DelaySystemIdle,
       UpdateSystemActivity) whose push made the hook touch unheld memory there; the page-in needs the busy driver. So
       with VM on the hook never reads above its callers' frames, hardware or QEMU; with VM off nothing can page. */
    gActReadStack = true;
#endif
    if (gRamReadable) {                        /* VM off: only what the memory map found mapped */
        gActTop = ActMapTop();
        if (gActTop == 0) {
            gActReadStack = false;
            Lg("!! uwake16: no memory map: the hook reads no RAM above its own frame (the entry is still recorded)");
        }
    } else gActTop = gLRam;                    /* VM on: the logical space is backed throughout */
    if (gClkGetLast == NULL || gClk == NULL || gClk->G == 0) {
        Lg("!! uwake16: Mac OS's activity clocks were not found (see the uwake14 lines): activity reports are not recorded");
        return;
    }
    tv = (UInt32 *)(void *)gClkGetLast;        /* a NativePowerMgrLib export's vector: its code, then NPM's TOC */
    if (!SpyReadable((UInt32)tv, 8)) {
        Lg("!! uwake16: GetLastActivity's vector %08lX is unreadable: activity reports are not recorded", (unsigned long)tv);
        return;
    }
    toc = tv[1];
    /* PMIdleUpdate's own vector, kept in the TOC: a pair (code, this TOC) whose code's traceback name is PMIdleUpdate.
       Exactly one is expected (d9e42f48: TOC+0x24); none, or more than one, and nothing is changed. */
    for (a = toc - 0x4000UL; a < toc + 0x4000UL; a += 4) {
        UInt32 cw;
        if (!SpyReadable(a, 8) || *(volatile UInt32 *)(a + 4UL) != toc) continue;
        cw = *(volatile UInt32 *)a;
        if ((cw & 3UL) || !SpyReadable(cw, 4)) continue;
        if (SpyTbName(cw, nm, sizeof nm) && strcmp(nm, "PMIdleUpdate") == 0 && nFound++ == 0) { found = a; code = cw; }
    }
    if (nFound != 1) {
        Lg("!! uwake16: %lu transition vector(s) to PMIdleUpdate in NativePowerMgrLib's TOC (%08lX, 16 KB either side "
           "searched; exactly one was expected): the activity routine is NOT hooked, reports are not recorded",
           (unsigned long)nFound, (unsigned long)toc);
        return;
    }
    t = (UInt32 *)NewPtrSys(32);
    if (t == NULL) { Lg("!! uwake16: no memory for the trampoline: the activity routine is NOT hooked"); return; }
    hv = (UInt32)(void *)ActHook;              /* our vector: ActHook's code and OUR TOC */
    t[0] = 0x3D800000UL | (hv >> 16);          /* lis   r12, hv@h       (NPM's glue enters with r2 = NPM's TOC) */
    t[1] = 0x618C0000UL | (hv & 0xFFFFUL);     /* ori   r12, r12, hv@l */
    t[2] = 0x800C0000UL;                       /* lwz   r0, 0(r12) */
    t[3] = 0x804C0004UL;                       /* lwz   r2, 4(r12)       our TOC */
    t[4] = 0x7C0903A6UL;                       /* mtctr r0 */
    t[5] = 0x4E800420UL;                       /* bctr                   (LR still returns into NPM) */
    t[6] = t[7] = 0;
    MakeDataExecutable(t, 24);
    gActTramp = t;
    gActOrig[0] = code; gActOrig[1] = toc;
    gActWinSince = kLowTicks;
    gActSlot = found;
    __asm__ volatile ("sync" ::: "memory");
    *(volatile UInt32 *)found = (UInt32)t;     /* THE switch: one aligned word, which NPM's glue reads on every call */
    __asm__ volatile ("sync" ::: "memory");
    Lg("  uwake16: Mac OS's activity routine now goes through Sleep Assistant: PMIdleUpdate (code %08lX in %s), its vector "
       "at %08lX (TOC %+ld), a trampoline at %08lX, ActHook, the original", (unsigned long)code, SpyWhere(code),
       (unsigned long)found, (long)(found - toc), (unsigned long)t);
    Lg("  uwake16: each report that moves the system clock is recorded with who sent it; RAM read below %08lX%s",
       (unsigned long)gActTop, gActReadStack ? (gRamReadable ? " (the memory map's top)" :
       " (virtual memory is on: QEMU check build, the stack is read anyway)")
       : " (virtual memory is on: the stack above the PowerPC callers is not read)");
}
static void QuietLog(void)                     /* TASK LEVEL, once, right after ActInstall (uwake21) */
{
#ifdef UWAKE_QTEST_NOQUIET
    Lg("!! uwake21 OFF (the QEMU control build): a silent Notification Manager request counts as activity, as in Mac OS");
#else
    if (gSpyOrig != NULL && gActSlot != 0)
        Lg("  uwake21: a silent Notification Manager request (no mark, icon, sound or text: a driver's way to task level) "
           "no longer counts as system activity: its _IdleUpdate stops at the activity hook, so a driver that posts one "
           "every few seconds (the USB 2.0 driver's keepalive) no longer holds off idle sleep");
    else
        Lg("!! uwake21: NOT in effect (%s): a silent Notification Manager request still counts as activity",
           gSpyOrig == NULL ? "_NMInstall is not patched" : "the activity routine is not hooked");
#endif
}
static void ActName(UInt32 a, char *out, size_t max)       /* TASK LEVEL: a PowerPC return address */
{
    char nm[64];
    if (a == 0) { snprintf(out, max, "(none)"); return; }
    if (!(a & 3UL) && SpyTbName(a, nm, sizeof nm)) {
        if (a >= 0xFFC00000UL) snprintf(out, max, "%08lX in %s (ROM +%06lX)", (unsigned long)a, nm, (unsigned long)(a - 0xFFC00000UL));
        else snprintf(out, max, "%08lX in %s (%s)", (unsigned long)a, nm, SpyWhere(a));
    } else if (a >= 0xFFC00000UL) snprintf(out, max, "%08lX (no name; ROM +%06lX)", (unsigned long)a, (unsigned long)(a - 0xFFC00000UL));
    else snprintf(out, max, "%08lX (no name; %s)", (unsigned long)a, SpyWhere(a));
}
static void ActEntName(UInt32 lr, char *out, size_t max)   /* TASK LEVEL: which entry into the power manager */
{
    char nm[64];
    if (!SpyTbName(lr, nm, sizeof nm)) { snprintf(out, max, "the power manager at %08lX (no name)", (unsigned long)lr); return; }
    if (strcmp(nm, "__PowerMgrOpDispatch") == 0) snprintf(out, max, "the $A285 trap (_IdleUpdate, __PowerMgrOpDispatch)");
    else if (strcmp(nm, "__PowerDispatch") == 0) snprintf(out, max, "_PowerDispatch selector 1 (__PowerDispatch)");
    else if (strcmp(nm, "PMIdleDelay") == 0) snprintf(out, max, "DelaySystemIdle (PMIdleDelay)");
    else snprintf(out, max, "%s", nm);
}
static const char *ActTrapName(UInt16 w)
{
    if (w == 0xA285U) return "_IdleUpdate";
    if (w == 0xA09FU) return "_PowerDispatch";
    if (w == 0xA09EU) return "the $A09E trap";
    switch (w & 0xF8FFU) {
    case 0xA000U: return "_Open";
    case 0xA001U: return "_Close";
    case 0xA002U: return "_Read";
    case 0xA003U: return "_Write";
    case 0xA00AU: return "_OpenRF";
    case 0xA00CU: return "_GetFileInfo";
    case 0xA060U: return (w & 0x0200U) ? "_HFSDispatch" : "_FSDispatch";
    }
    return "a trap";
}
static void ActName68k(UInt32 a, UInt16 w, char *out, size_t max)   /* TASK LEVEL: 68K code above the report */
{
    char nm[64]; const char *tn = ActTrapName(w);
    if (a >= 0xFFC00000UL)
        snprintf(out, max, "%08lX, after %s ($%04X), in the ROM (+%06lX)", (unsigned long)a, tn, (unsigned)w, (unsigned long)(a - 0xFFC00000UL));
    else if (Spy68kName(a, nm, sizeof nm))
        snprintf(out, max, "%08lX, after %s ($%04X), in %s; the next MacsBug name after it: %s", (unsigned long)a, tn, (unsigned)w, SpyWhere(a), nm);
    else snprintf(out, max, "%08lX, after %s ($%04X), in %s (no MacsBug name within 4 KB)", (unsigned long)a, tn, (unsigned)w, SpyWhere(a));
}
static void ActCodeBytes(const char *what, UInt32 k, UInt32 a)   /* TASK LEVEL: the 16 bytes before a return address and
                                                                    the 8 after, to find unnamed code in its file offline */
{
    char b[160]; int m; UInt32 p;
    if ((a & 1UL) || a < 16UL || !SpyReadable(a - 16UL, 24)) return;
    m = snprintf(b, sizeof b, "      %s %lu, %08lX:", what, (unsigned long)k, (unsigned long)a);
    for (p = a - 16UL; p < a + 8UL; p += 2) {
        if (p == a) m += snprintf(b + m, sizeof b - (size_t)m, " |");
        m += snprintf(b + m, sizeof b - (size_t)m, "%s%04X", ((p - a) & 3UL) ? "" : " ",
                      (unsigned)*(const volatile UInt16 *)p);
    }
    Lg("%s", b);
}
static void ActLogRaw(const ActSig *s, UInt32 route)   /* TASK LEVEL: a route's details, once: the code around its
                                                          unnamed return addresses (task-level reads of code: fine with
                                                          VM on too), then the stack at its first move if one was kept */
{
    const UInt32 *w; UInt32 k, j, z = 0; char b[120], nm[64]; int m;
    Lg("    R%lu's unnamed code (16 bytes before each return address | 8 after), to be found in its file offline:",
       (unsigned long)route);
    for (k = 1; k < s->nCh && k < 16; k++) {
        if (s->ch[k] <= 1UL || (!(s->ch[k] & 3UL) && SpyTbName(s->ch[k], nm, sizeof nm))) continue;
        ActCodeBytes("PowerPC caller", k, s->ch[k]);
    }
    for (k = 0; k < s->n68; k++) ActCodeBytes("68K", k, s->a68[k]);
    if (s->raw == 0) return;
    w = gActRaw[s->raw - 1];
    Lg("    R%lu's stack at its first move (r1 %08lX; %u bytes as words, offsets from r1; all-zero lines left out):",
       (unsigned long)route, (unsigned long)s->sp, (unsigned)kActRawB);
    for (k = 0; k < kActRawB / 4; k += 8) {
        Boolean allz = true;
        for (j = 0; j < 8; j++) if (w[k + j]) allz = false;
        if (allz) { z++; continue; }
        m = snprintf(b, sizeof b, "        +%03lX", (unsigned long)(k * 4UL));
        for (j = 0; j < 8; j++) m += snprintf(b + m, sizeof b - (size_t)m, " %08lX", (unsigned long)w[k + j]);
        Lg("%s", b);
    }
    if (z) Lg("        (%lu all-zero line(s) left out)", (unsigned long)z);
}
static void ActLogRawPending(void)             /* TASK LEVEL: every route's details not yet written (uwake17: also at the
                                                  first pass after a wake: uwake16A's log was copied before the stop) */
{
    UInt32 n = gActSigN, i;
    if (n > kActSigN) n = kActSigN;
    for (i = 0; i < n; i++) {
        ActSig *s = &gActSig[i];
        if (s->sig == 0 || s->rawShown) continue;      /* (rawShown: its details, code bytes and any stack copy) */
        ActLogRaw(s, i + 1);
        s->rawShown = 1;
    }
}
static void ActReset(void)                     /* TASK LEVEL, after a logged stop: the routes and copies start again */
{
    UInt32 i;
    gActSigN = 0x7FFFFFFFUL;                   /* claims fail meanwhile (the hook counts them as not recorded) */
    __asm__ volatile ("sync" ::: "memory");
    for (i = 0; i < kActSigN; i++) gActSig[i].sig = 0;
    gActRawN = 0;
    __asm__ volatile ("sync" ::: "memory");
    gActSigN = 0;
}
static void ActWindowStart(void)               /* TASK LEVEL: the per-window counts start again (routes stay) */
{
    UInt32 i, n = gActSigN;
    if (n > kActSigN) n = kActSigN;
    for (i = 0; i < n; i++) { gActSig[i].pushes = 0; gActSig[i].minGap = gActSig[i].maxGap = 0; }
    for (i = 0; i < kActEntN; i++) memset(gActEnt[i].n, 0, sizeof gActEnt[i].n);
    for (i = 0; i < 6; i++) gActCalls[i] = 0;
    gActPushes = gActFull = gActUserMoves = gActQuiet = 0;
    gActWinSince = kLowTicks;
}
static void ActLogWindow(const char *what, Boolean raw)   /* TASK LEVEL; raw: write the stack copies not yet written
                                                             (the pass's stops, not the sleep hook: it keeps that short) */
{
    UInt32 n = gActSigN, ne = gActEntN, i, k, since = gActWinSince, tot; char b[200], e[96], when[112]; int m;
    if (gActSlot == 0) { Lg("  uwake16 %s: (activity reports were not recorded: see the boot lines)", what); return; }
    if (n > kActSigN) n = kActSigN;
    if (ne > kActEntN) ne = kActEntN;
    for (tot = 0, i = 0; i < 6; i++) tot += gActCalls[i];
    Lg("  uwake16 %s: Mac OS's activity routine was called %lu time(s) in the window (overall %lu, keyboard/mouse %lu, "
       "network %lu, disk %lu, idle %lu, other %lu); %lu call(s) moved the system clock, %lu of them keyboard/mouse "
       "reports (counted, no routes)%s", what, (unsigned long)tot,
       (unsigned long)gActCalls[0], (unsigned long)gActCalls[1], (unsigned long)gActCalls[2], (unsigned long)gActCalls[3],
       (unsigned long)gActCalls[4], (unsigned long)gActCalls[5], (unsigned long)gActPushes, (unsigned long)gActUserMoves,
       gActFull ? "; !! the route table was full: those moves are marked * below, unnamed" : "");
    Lg("    uwake21: %lu report(s) from silent Notification Manager requests not passed on in the window (%lu since boot)",
       (unsigned long)gActQuiet, (unsigned long)gActQuietAll);
    for (i = 0; i < ne; i++) {                 /* every call, by its entry into the power manager */
        const ActEnt *en = &gActEnt[i]; UInt32 c = 0;
        for (k = 0; k < 6; k++) c += en->n[k];
        if (c == 0 || en->lr == 0) continue;
        ActEntName(en->lr, e, sizeof e);
        Lg("    by %s: %lu (overall %lu, keyboard/mouse %lu, network %lu, disk %lu, idle %lu)", e, (unsigned long)c,
           (unsigned long)en->n[0], (unsigned long)en->n[1], (unsigned long)en->n[2], (unsigned long)en->n[3],
           (unsigned long)en->n[4]);
    }
    for (i = 0; i < n; i++) {                  /* every route that moved the system clock in the window */
        ActSig *s = &gActSig[i];
        static const char *const kTy[6] = { "overall", "keyboard/mouse", "network", "disk", "idle", "out-of-range" };
        if (s->sig == 0 || s->pushes == 0) continue;
        ActEntName(s->lr, e, sizeof e);
        if (s->pushes == 1) snprintf(when, sizeof when, "once, %lu s into the window", (unsigned long)((s->first - since) / 60UL));
        else snprintf(when, sizeof when, "%lu to %lu s apart (first %lu s into the window, last %lu s)",
                      (unsigned long)(s->minGap / 60UL), (unsigned long)(s->maxGap / 60UL),
                      (unsigned long)((s->first - since) / 60UL), (unsigned long)((s->last - since) / 60UL));
        Lg("    R%lu: moved the system clock %lu time(s) (%lu since boot), %s: %s reports, by %s", (unsigned long)(i + 1),
           (unsigned long)s->pushes, (unsigned long)s->total, when, kTy[s->type < 6 ? s->type : 5], e);
        if (s->nCh == 0 || s->ch[0] != s->lr)
            Lg("       !! the back chain's first return address (%08lX) is not the entry's (%08lX): the walk below may be off",
               (unsigned long)(s->nCh ? s->ch[0] : 0), (unsigned long)s->lr);
        for (k = 1; k < s->nCh; k++) {
            if (s->ch[k] == 1UL) { Lg("       PowerPC caller %lu: (a Mixed Mode switch frame: the caller beyond it ran 68K)", (unsigned long)k); continue; }
            ActName(s->ch[k], b, sizeof b);
            Lg("       PowerPC caller %lu: %s", (unsigned long)k, b);
        }
        for (k = 0; k < s->n68; k++) {
            ActName68k(s->a68[k], s->w68[k], b, sizeof b);
            Lg("       68K: %s (stack +%u)", b, (unsigned)s->o68[k]);
        }
        if (s->n68 == 0) Lg("       68K: no return address after an activity or File Manager trap in the %u bytes above%s",
                            (unsigned)kActRawB, gActReadStack ? "" : " (not read: virtual memory is on)");
        if (!s->rawShown && !raw) Lg("       (its code bytes%s follow at the first pass after the wake)",
                                     s->raw ? " and stack copy" : "");
    }
    if (raw) ActLogRawPending();               /* the details not yet written, whichever window they came from */
    if (gClk != NULL && gClk->G != 0) {        /* each system-clock move the heartbeat saw, and the route timed to it */
        const volatile ClkRec *c = gClk; UInt32 ev = c->events, j, cs = c->winSince, shown = 0;
        UInt32 first = (ev > kClkRingN) ? ev - kClkRingN : 0;
        m = 0; b[0] = 0;
        for (j = first; j < ev; j++) {
            const volatile ClkEv *ce = &c->ev[j % kClkRingN];
            if (!(ce->moved & 1U) || (SInt32)(ce->tick - cs) < 0) continue;
            m += snprintf(b + m, sizeof b - (size_t)m, "%s+%lu", m ? " " : "", (unsigned long)((ce->tick - cs) / 60UL));
            if (ce->act == 0xFFU) m += snprintf(b + m, sizeof b - (size_t)m, "*");       /* the hook's, unnamed */
            else if (ce->act == 0xFEU) m += snprintf(b + m, sizeof b - (size_t)m, "k");  /* keyboard/mouse */
            else if (ce->act) m += snprintf(b + m, sizeof b - (size_t)m, "R%u", (unsigned)ce->act);
            else m += snprintf(b + m, sizeof b - (size_t)m, "?");
            shown++;
            if (m > (int)sizeof b - 24) {
                Lg("    system-clock moves (s into the window; Rn = the route above; * = through the hook, table full; ? = not through the hook): %s", b);
                b[0] = 0; m = 0;
            }
        }
        if (m) Lg("    system-clock moves (s into the window; Rn = the route above; * = through the hook, table full; ? = not through the hook): %s", b);
        if (shown == 0) Lg("    (no system-clock move in the window's part of the ring)");
    }
}
/* Which File Manager calls report activity? The Bluetooth strip module (bluetooth/csm/bt_csm.c, Refresh) calls
   FindFolder, FSMakeFSSpec, PBGetCatInfoSync, FSpOpenDF, FSRead and FSClose every 5 s while SBSafeToAccessStartupDisk
   says yes, which stops when the hard disk spins down: the FW800 reporter's shape, IF those calls report. Apple's 1994
   Device Manager reported overall activity on every _Read and _Write, but the FW800's 68K ROM has no $A285 left except
   NMInstall's, so it is measured here: each call ten times, the activity routine's calls counted by type. Task level,
   inside one pass (our own pump posts nothing until the pass ends). */
static void ActTestLine(const char *what, const UInt32 *c0)
{
    UInt32 d[6], s = 0; int q;
    for (q = 0; q < 6; q++) { d[q] = gActCalls[q] - c0[q]; s += d[q]; }
    Lg("  uwake16 self-test: 10 x %s: %lu activity report(s) (overall %lu, keyboard/mouse %lu, network %lu, disk %lu, "
       "idle %lu)", what, (unsigned long)s, (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2],
       (unsigned long)d[3], (unsigned long)d[4]);
}
static void ActFileTest(void)                  /* TASK LEVEL, once, right after ActInstall hooked the routine */
{
    FSSpec sp; FInfo fi; CInfoPBRec pb; Str63 nm; short vr, ref, i, fv; long dir, fd, n; char buf[16]; UInt32 c0[6]; int q;
    if (gActSlot == 0) return;
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vr, &dir) != noErr
        || FSMakeFSSpec(vr, dir, "\pSleep Assistant Log", &sp) != noErr) {
        Lg("  uwake16 self-test: our own log file was not found: the File Manager test did not run");
        return;
    }
    Lg("  uwake16 self-test: which File Manager calls report activity (each made 10 times on our own log file)");
    for (q = 0; q < 6; q++) c0[q] = gActCalls[q];
    for (i = 0; i < 10; i++) (void)FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &fv, &fd);
    ActTestLine("FindFolder (the Preferences folder)", c0);
    for (q = 0; q < 6; q++) c0[q] = gActCalls[q];
    for (i = 0; i < 10; i++) (void)FSMakeFSSpec(vr, dir, "\pSleep Assistant Log", &sp);
    ActTestLine("FSMakeFSSpec", c0);
    for (q = 0; q < 6; q++) c0[q] = gActCalls[q];
    for (i = 0; i < 10; i++) (void)FSpGetFInfo(&sp, &fi);
    ActTestLine("FSpGetFInfo", c0);
    for (q = 0; q < 6; q++) c0[q] = gActCalls[q];
    for (i = 0; i < 10; i++) {
        BlockMoveData(sp.name, nm, (Size)(sp.name[0] + 1));
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = nm; pb.hFileInfo.ioVRefNum = sp.vRefNum; pb.hFileInfo.ioDirID = sp.parID;
        pb.hFileInfo.ioFDirIndex = 0;
        (void)PBGetCatInfoSync(&pb);
    }
    ActTestLine("PBGetCatInfoSync", c0);
    for (q = 0; q < 6; q++) c0[q] = gActCalls[q];
    for (i = 0; i < 10; i++) if (FSpOpenDF(&sp, fsRdPerm, &ref) == noErr) (void)FSClose(ref);
    ActTestLine("FSpOpenDF then FSClose", c0);
    if (FSpOpenDF(&sp, fsRdPerm, &ref) == noErr) {
        for (q = 0; q < 6; q++) c0[q] = gActCalls[q];
        for (i = 0; i < 10; i++) { n = (long)sizeof buf; if (SetFPos(ref, fsFromStart, 0) == noErr) (void)FSRead(ref, &n, buf); }
        ActTestLine("SetFPos then FSRead (16 bytes, the file's first block)", c0);
        (void)FSClose(ref);
    } else Lg("  uwake16 self-test: the log file would not open for reading: no FSRead test");
}

static const char *NmWork(UInt32 t)            /* TASK LEVEL: what the pass still has to do (NULL: nothing) */
{
    if (!gDupChecked) return "the check for a second copy";
    if (!gSnhTried) return "the put-away hook";
    if (!gClkTried) return "the activity clocks";
    if (!gSpyTried) return "the request recorder";
    if (!gActTried) return "the activity hook";
    if (!gMapDone) return "the memory map";
#ifdef UWAKE_PAINT
    if (!gScr.ok && t - gBootTicks < 120UL * 60UL) return "the screen";
#endif
    if (gWakeN != gWakeLogged) return "a wake to report";
    if (gRecOn) return "a wake being recorded";
#ifdef UWAKE_MODE_A
    if (gKwReq != NULL && !gKwStuck && gNecN > 0 && (gKwOn || gKwWant || gKwSurveys == 0)) return "the keyboard-wake survey";
#endif
#ifdef UWAKE_QTEST
#ifdef UWAKE_MODE_A
    if (t - gBootTicks < 100UL * 60UL) return "QEMU: the scheduled tests";
#else
    if (gQPhase < 2) return "QEMU: the simulated sleep and wake";
#endif
#endif
    return NULL;
}
static void NmStop(void)                       /* TASK LEVEL: the end of a pass with nothing left to do */
{
    char ts[32];
    gNmStopped = true; gNmStops++;
    if (gNmStops <= 5 || gNmStops % 20UL == 0) {
        LgStamp(ts, sizeof ts);
        Lg("  uwake14 %s: nothing left for the task-level pass: it stops (#%lu, %lu passes since boot) until the put-away "
           "hook arms it at the next sleep", ts, (unsigned long)gNmStops, (unsigned long)gNmPasses);
        ClkLogWindow("before the stop");
        SpyLogWindow("before the stop", false);
        ActLogWindow("before the stop", true);
        ClkLogNow("at the stop");
#ifdef UWAKE_QTEST_PUMP
        QtPumpLog();
#endif
    }
    LogFlush();
    ClkWindowStart(true);                      /* (after the flush: its own disk write is not counted in the window) */
    SpyWindowStart();
    ActWindowStart();
    if (gNmStops <= 5 || gNmStops % 20UL == 0) ActReset();   /* (logged: the quiet window starts with a free table) */
}
static void NmRestart(void)                    /* TASK LEVEL: the put-away hook, at every sleep attempt */
{
    OSErr e; char ts[32]; Boolean say;
    if (gNM == NULL || !gNmStopped) return;
    gNmRestarts++;
    say = (gNmRestarts <= 5 || gNmRestarts % 20UL == 0);
    if (say) { LgStamp(ts, sizeof ts); ClkLogWindow(ts); SpyLogWindow(ts, true); ActLogWindow(ts, false); ClkLogNow("at this sleep"); }
    ClkWindowStart(false);
    SpyWindowStart();
    ActWindowStart();
    gNmStopped = false;
    (void)NMRemove(gNM);
    e = NMInstall(gNM);
    if (e != noErr) {
        gNmStopped = true;
        Lg("!! uwake14: the task-level pass could not be armed for after this sleep (NMInstall %d): no report of it", (int)e);
    } else if (say) Lg("  uwake14: the task-level pass is armed again, for after this sleep (#%lu)", (unsigned long)gNmRestarts);
    LogFlush();
}
#ifdef UWAKE_QTEST
/* ⚠ QEMU ONLY (uwake16). QEMU has no put-away hook, so nothing would run a pass at the end of the quiet window to log
   who reported activity in it. At 560 s after boot, if the pass is stopped, the heartbeat posts it once, through
   SpyNMInstall: the _NMInstall patch's own route, already any-level and covered by the audit, exactly as an interrupt-
   level NMInstall would go. That pass finds nothing to do, stops, and logs the window. */
static volatile Boolean gQtActPosted;
static void QtActPost(void)                    /* 68K INTERRUPT LEVEL (the heartbeat) */
{
    if (gQtActPosted || !gNmStopped || gNM == NULL || gSpyOrig == NULL || kLowTicks - gBootTicks < 560UL * 60UL) return;
    gQtActPosted = true;
    (void)SpyNMInstall(0xA05EU, gNM);
}
#ifdef UWAKE_QTEST_PUMP
/* ⚠ QEMU ONLY (uwake21): a stand-in for the released USB 2.0 driver's keepalive (fw1rel task_work_arm: a silent request
   every ~2 s, posted at interrupt level, one in flight, removed by its own response). From the heartbeat, in the quiet
   window (the pass stopped, 300 to 550 s after boot), through SpyNMInstall, the patch's own route, as QtActPost posts:
   the audit keeps the trap out of interrupt-level code, and the driver's trap call arrives in this same function, which
   chains to the original _NMInstall and its _IdleUpdate. At 450 s, once, a request WITH an Application-menu mark: a
   notice the user sees, the positive control,
   which must still move the system clock. QtActPost's pass at 560 s logs the window: the pump by refCon 'QPMP', the
   control 'QMRK'. With UWAKE_QTEST_NOQUIET (the control build) the pump must move the clock every 10 s instead. */
static NMRecPtr         gQtPumpNm, gQtMarkNm;
static volatile Boolean gQtPumpIn, gQtMarkDone;
static volatile UInt32  gQtPumpPosts, gQtPumpResps, gQtPumpErr, gQtPumpLast;
static pascal void QtPumpResp(NMRecPtr r)      /* TASK LEVEL (the Notification Manager calls it) */
{
    (void)NMRemove(r);
    if (r == gQtPumpNm) { gQtPumpResps++; gQtPumpIn = false; }
}
static void QtPump(void)                       /* 68K INTERRUPT LEVEL (the heartbeat) */
{
    UInt32 t = kLowTicks - gBootTicks;
    if (gQtPumpNm == NULL || gSpyOrig == NULL || !gNmStopped || t < 300UL * 60UL || t > 550UL * 60UL) return;
    if (!gQtMarkDone && t >= 450UL * 60UL) {
        gQtMarkDone = true;
        if (SpyNMInstall(0xA05EU, gQtMarkNm) != noErr) gQtPumpErr++;
    }
    if (gQtPumpIn || kLowTicks - gQtPumpLast < 120UL) return;    /* every 2 s, one in flight */
    gQtPumpLast = kLowTicks;
    gQtPumpIn = true;
    if (SpyNMInstall(0xA05EU, gQtPumpNm) == noErr) gQtPumpPosts++;
    else { gQtPumpIn = false; gQtPumpErr++; }
}
static void QtPumpArm(void)                    /* TASK LEVEL, once, at install (ArmNM's way: the UPP in the system zone) */
{
    NMUPP upp; THz oldZone = GetZone();
    SetZone(SystemZone());
    upp = NewNMUPP((NMProcPtr)QtPumpResp);
    SetZone(oldZone);
    gQtPumpNm = (NMRecPtr)NewPtrSysClear((Size)sizeof(NMRec));
    gQtMarkNm = (NMRecPtr)NewPtrSysClear((Size)sizeof(NMRec));
    if (upp == NULL || gQtPumpNm == NULL || gQtMarkNm == NULL) {
        gQtPumpNm = NULL; Lg("!! QTEST uwake21: the stand-in pump could not be set up"); return;
    }
    gQtPumpNm->qType = nmType; gQtPumpNm->nmResp = upp; gQtPumpNm->nmRefCon = 0x51504D50L;   /* 'QPMP': silent */
    gQtMarkNm->qType = nmType; gQtMarkNm->nmResp = upp; gQtMarkNm->nmRefCon = 0x514D524BL;   /* 'QMRK' */
    gQtMarkNm->nmMark = 1;                                                                   /* a notice the user sees */
    Lg("!! QEMU TEST BUILD (UWAKE_QTEST_PUMP): a silent request every 2 s from the heartbeat, 300 to 550 s after boot "
       "(the USB 2.0 driver's keepalive, stood in); one with an Application-menu mark at 450 s (the control)");
}
static void QtPumpLog(void)                    /* TASK LEVEL: with the window's log */
{
    if (gQtPumpNm == NULL) return;
    Lg("  QTEST uwake21: the stand-in pump posted %lu request(s), %lu answered, %lu refused; the marked one %s",
       (unsigned long)gQtPumpPosts, (unsigned long)gQtPumpResps, (unsigned long)gQtPumpErr,
       gQtMarkDone ? "posted" : "NOT posted");
}
#endif
/* ⚠ QEMU ONLY. QEMU's power manager is not the analysed one, so the hook is not installed there: the hook's two uwake14
   steps, driven once inside a pass. NmRestart on a pass marked stopped logs the window since the clocks were found,
   during which the pass RAN: the control (expect background reports ~10 s apart: our own NMInstalls). Then the device
   check with the survey's signature changed: it must choose the functions the survey chose. */
static void QtNmTest(void)
{
    LgNow("=== QTEST uwake14: the put-away hook's steps, driven directly (QEMU only)");
    gNmStopped = true;
    NmRestart();
#ifdef UWAKE_MODE_A
    {
        UInt32 keepSig = gKwSig; Boolean keepFn[kMaxNec], decided, same = true; short i;
        memcpy(keepFn, gKwFn, sizeof keepFn);
        gKwSig ^= 0x5A5A5A5AUL;
        decided = KwHookCheck();
        for (i = 0; i < kMaxNec; i++) if (gKwFn[i] != keepFn[i]) same = false;
        Lg("  QTEST uwake14: the hook's device check %s; the Name Registry alone %s the last survey's choice",
           decided ? "decided" : "did NOT decide", same ? "AGREES with" : "DIFFERS from");
        gKwSig = keepSig; memcpy(gKwFn, keepFn, sizeof gKwFn);
    }
#endif
    LogFlush();
}
#endif

static pascal void NmPass(NMRecPtr nm)
{
    UInt32 t = kLowTicks;
    gNmPasses++;
    if (gRecOn) {                         /* task level after a wake: when it first runs again, and any silence */
        UInt32 now = RecMs();
        if (!gTaskSeen) { gTaskSeen = true; Ev(0xFF, kEvTaskFirst, 0, now); }
        else if (now - gTaskLast > 500UL) Ev(0xFF, kEvTaskGap, 0, now - gTaskLast);
        gTaskLast = now;
        /* uwake9: the recorder's watchdog (uwake8B's timer stopped for good at +15.16 s) */
        if (gRecPasses != gWdPasses) { gWdPasses = gRecPasses; gWdMs = now; }
        else if (now - gWdMs > 500UL && gRearmN < 20) {
            gRearmN++;
            Ev(0xFF, kEvRecStall, (UInt16)gRecStep, gRecPassMs);
            Lg("  !! uwake9: the recorder had stopped: its last pass began at +%lu ms and reached step %02lX%s; the Time "
               "Manager heartbeat ran %lu time(s) since; restarting it (#%lu, at +%lu ms)", (unsigned long)gRecPassMs,
               (unsigned long)gRecStep, gRecStep == 5 ? " (it had re-armed its timer, which never fired)" :
               gRecStep == 6 ? " (QEMU test: dropped on purpose)" : " (it never finished)",
               (unsigned long)(gHbN - gHbAtPass), (unsigned long)gRearmN, (unsigned long)now);
            LogFlush();
            RecRearm(); gWdMs = now;
        }
#ifdef UWAKE_MODE_B
        RcvTask();                        /* uwake9: fn1's failed-add recovery, at task level */
#endif
        ValidatePass();                   /* uwake6: the translations the recorder may read through (GetPhysical) */
#ifdef UWAKE_QTEST
        if (!gQtdDone) QtdSelfCheck();
#endif
    }
#ifdef UWAKE_MODE_A
    KwPass(t);                            /* uwake11: the keyboard-wake survey (one USB request per pass at most) */
#endif
    if (t - gNmLast >= 60UL || gNmPasses == 1) {
        gNmLast = t;
#ifdef UWAKE_PAINT
        if (!gScr.ok) { ScrInit(); if (gScr.ok) Lg("  screen: %d x %d, %d bpp, base %08lX (the status row is at the bottom-left)",
                                                gScr.w, gScr.h, gScr.depth, gScr.base); }
#endif
        if (gInstalled && !gSnhTried) SnhInstall();              /* the put-away before sleep (servers + USB volumes) */
        if (gInstalled && !gClkTried) ClkInstall();              /* uwake14: Mac OS's activity clocks, watched */
        if (gInstalled && !gSpyTried) SpyInstall();              /* uwake15: every Notification Manager request, recorded */
        if (!gDupChecked) DupCheck();                            /* a second copy (an old file name) stood down? */
        if (gInstalled && !gMapDone) MapMemory();                /* uwake7: the memory map, once, before any wake */
        if (gInstalled && !gActTried) {                          /* uwake16: every activity report, by who (after */
            ActInstall();                                        /* the map: its top bounds the hook's reads) */
            QuietLog();                                          /* uwake21: in effect only with both hooks in */
            LogFlush();                                          /* (creates the log file the self-test reads) */
            ActFileTest();
        }
        if (gWakeN != gWakeLogged) {      /* a wake not yet reported: when the recorder ends, or if it never does */
            Boolean write = false; const char *why = NULL;
            if (gNmWakeSeen != gWakeN) {
                gNmWakeSeen = gWakeN; gNmWakeTicks = t; gNmPassesSeen = gRecPasses; gNmStall = 0;
#ifdef UWAKE_MODE_A
                KwLogEarly();             /* uwake11: what woke it, on disk at once */
#endif
                ActLogRawPending();       /* uwake17: the quiet window's routes in detail, at once too */
            }
            if (gRecDone || (!gRecOn && gRecN > 0)) write = true;
            else if (gRecOn) {
                if (gRecPasses == gNmPassesSeen) {
                    if (++gNmStall >= 5) { write = true; why = "the recorder STALLED (no pass for 5 s while on)"; }
                } else { gNmPassesSeen = gRecPasses; gNmStall = 0; }
                if (!write && t - gNmWakeTicks > (kRecEndMs / 1000UL + 20UL) * 60UL) {   /* the window + 20 s (uwake5: 6 min) */
                    write = true; why = "20 s past the recording window the recorder had not ended";
                }
            }
            if (write) {
                gWakeLogged = gWakeN;
#ifdef UWAKE_PAINT
                ScrInit();
#endif
#ifdef UWAKE_MODE_A
                UeBack();
                gKwWant = true;           /* uwake11: survey again after the report (did the arming hold through it?) */
#endif
                if (why) Lg("!! %s: writing what it has (passes %lu, events %lu)", why, (unsigned long)gRecPasses, (unsigned long)gRecN);
                LogWake();
            }
        }
#ifdef UWAKE_QTEST
        if (gInstalled && !gSnhQDone && t - gBootTicks > 60UL * 60UL) { gSnhQDone = true; SnhQTest(); QtNmTest(); }
        if (gInstalled) {
#ifdef UWAKE_MODE_A
            static Boolean qDone;
            if (!qDone && t - gBootTicks > 90UL * 60UL) { qDone = true; QSimCycleA(); }
#else
            if (gQPhase == 0 && t - gBootTicks > 90UL * 60UL) { gQPhase = 1; gQTicks = t; QSimSleepB(); }
            else if (gQPhase == 1 && t - gQTicks > 3UL * 60UL) { gQPhase = 2; QSimWakeB(); }
#endif
        }
#endif
        LogFlush();
    }
    if (nm) {                             /* uwake14: re-armed only while there is work (each NMInstall is activity) */
        const char *w = NmWork(t);
        (void)NMRemove(nm);
#ifndef UWAKE_QTEST                       /* (QEMU: no hook there; its memory dump reads the quiet window instead) */
        if (w == NULL && gSnhG == 0) {
            w = "no put-away hook";
            if (!gNmNoHookSaid) {
                gNmNoHookSaid = true;
                Lg("!! uwake14: no put-away hook on this Mac (its power manager is not the analysed one), so nothing would "
                   "arm the task-level pass again for a wake: it keeps running, and that holds off idle sleep");
                LogFlush();
            }
        }
#endif
        if (w != NULL) (void)NMInstall(nm);
        else NmStop();
    }
}

static void ArmNM(void)
{
    NMUPP upp; THz oldZone = GetZone();
    SetZone(SystemZone());
    upp = NewNMUPP((NMProcPtr)NmPass);
    SetZone(oldZone);
    if (upp == NULL) { Lg("!! NewNMUPP failed: no task-level pass (no wake report this boot)"); return; }
    gNM = (NMRecPtr)NewPtrSysClear((Size)sizeof(NMRec));
    if (gNM == NULL) { Lg("!! NMRec allocation failed"); return; }
    gNM->qType = nmType; gNM->nmResp = upp; gNM->nmRefCon = 0x5557414BL;   /* 'UWAK' */
    Lg("  task-level pass armed (NMInstall %d)", (int)NMInstall(gNM));
}

/* ============================================================== finding the NEC (NECQuiet's, verbatim) */
static unsigned long AaplAddress0(const RegEntryID *id)
{
    unsigned char b[32];
    RegPropertyValueSize sz = sizeof(b);
    if (RegistryPropertyGet((RegEntryID *)id, (RegPropertyName *)"AAPL,address", b, &sz) != noErr || sz < 4)
        return 0;
    return ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) | ((unsigned long)b[2] << 8) | b[3];
}
static unsigned long RegU32(RegEntryID *e, const char *name, Boolean *ok)
{
    UInt32 v = 0; RegPropertyValueSize sz = sizeof(v);
    *ok = (RegistryPropertyGet(e, (RegPropertyName *)name, &v, &sz) == noErr && sz >= 4);
    return (unsigned long)v;
}
static Boolean ReadConfig(UsbFn *f)
{
    short i; Boolean ok = true;
    for (i = 0; i < 64; i++) {
        UInt32 v = 0xFFFFFFFFUL;
        if (ExpMgrConfigReadLong(&f->id, (LogicalAddress)(unsigned long)(i * 4), &v) != noErr) { ok = false; v = 0xFFFFFFFFUL; }
        f->pre[i] = (unsigned long)v;
    }
    return ok;
}
static void FindUsbFunctions(void)
{
    RegEntryIter cookie; RegEntryID entry; Boolean done = false;
    gFnN = 0;
    if (RegistryEntryIterateCreate(&cookie) != noErr) return;
    for (;;) {
        Boolean okv, okd, okc; unsigned long vid, did, cls;
        RegistryEntryIDInit(&entry);
        if (RegistryEntryIterate(&cookie, kRegIterContinue, &entry, &done) != noErr || done) {
            RegistryEntryIDDispose(&entry); break;
        }
        vid = RegU32(&entry, "vendor-id", &okv);
        did = RegU32(&entry, "device-id", &okd);
        cls = RegU32(&entry, "class-code", &okc);
        if (okv && (vid == 0x1033UL || (okc && (cls >> 8) == 0x0C03UL)) && gFnN < kMaxFns) {
            UsbFn *f = &gFn[gFnN];
            unsigned char rb[24]; RegPropertyValueSize rs = sizeof(rb);
            RegistryEntryIDInit(&f->id);
            if (RegistryEntryIDCopy(&entry, &f->id) == noErr) {
                f->vid = vid; f->did = okd ? did : 0; f->cls = okc ? cls : 0;
                f->nec = NecVendor(vid, cls, okc);
                if (RegistryPropertyGet(&entry, (RegPropertyName *)"reg", rb, &rs) == noErr && rs >= 4)
                    f->reg0 = ((unsigned long)rb[0] << 24) | ((unsigned long)rb[1] << 16) |
                              ((unsigned long)rb[2] << 8) | rb[3];
                else f->reg0 = 0;
                f->preOK = false;
                gFnN++;
            }
        }
        RegistryEntryIDDispose(&entry);
    }
    RegistryEntryIterateDispose(&cookie);
}
static void SetUpNec(void)
{
    short i, j;
    gNecN = 0;
    for (i = 0; i < gFnN && gNecN < kMaxNec; i++) {
        UsbFn *f = &gFn[i]; NecFn *n;
        f->preOK = ReadConfig(f);
        if (!f->nec || !f->preOK) continue;
        n = &gNec[gNecN++];
        memset(n, 0, sizeof(*n));
        n->fi = i;
        n->ohci = ((f->cls & 0xFFFFFFUL) == 0x0C0310UL);
        n->pmOff = PMCapOff(f->pre);
        n->armWhy = 9; n->armPm0 = n->armPm1 = n->wkPm = 0xFFFFUL;   /* uwake10: no demand seen yet */
        n->kwWhy = 9; n->kwCtl0 = n->kwCtl1 = n->kwRh0 = n->kwRh1 = n->kwIst0 = 0xFFFFFFFFUL;   /* uwake11 */
        n->kwWkCtl = n->kwWkIst = n->kwWkRh = n->kwEnRh = 0xFFFFFFFFUL; n->kwQRdMs = -1;
        for (j = 0; j < 4; j++) n->kwP0[j] = n->kwWkP[j] = 0xFFFFFFFFUL;
        n->hcdSlp = n->hcdWake = n->hcdEn = 0xFFFFFFFFUL;
        n->enRev = n->enIen = n->enCtl = n->enIst = 0xFFFFFFFFUL;
        n->ehWhy = n->ewWhy = 9; n->ehPm0 = n->ehPm1 = n->ewPm = 0xFFFFUL; n->eeIntr = 0xFFFFFFFFUL;   /* uwake20 */
    }
    for (i = 0; i < gNecN; i++)                              /* function 0 first */
        for (j = (short)(i + 1); j < gNecN; j++)
            if (((gFn[gNec[j].fi].reg0 >> 8) & 7UL) < ((gFn[gNec[i].fi].reg0 >> 8) & 7UL)) {
                NecFn t = gNec[i]; gNec[i] = gNec[j]; gNec[j] = t;
            }
    gNecReady = (gNecN > 0);
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; UsbFn *f = &gFn[n->fi];
        if (!n->ohci) continue;
        n->base = AaplAddress0(&f->id);
        if (n->base == 0 || (n->base & 0xFFFUL) != (f->pre[4] & 0xFF0UL)) { n->base = 0; continue; }
        if (!CfgDecodes(n)) continue;
        if ((Swap32(*(volatile unsigned long *)n->base) & 0xFFUL) != 0x10UL) continue;
        ReadOhci(n, &n->launch);
        n->baseOK = n->launch.ok;
    }
    /* uwake19: the EHCI, when the ROM shows it. ehBase = AAPL,address when it agrees with BAR0; its registers are read
       at boot only if the boot config already decodes memory (a driver, or the firmware, turned decode on); otherwise
       the install decides from the config alone (decode off = nobody can be running it). uwake20: ehBase is kept even
       when decode is off at boot, so an EHCI a driver starts after us is still read at the demand (CfgDecodes first). */
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; UsbFn *f = &gFn[n->fi];
        if ((f->cls & 0xFFFFFFUL) != 0x0C0320UL) continue;
        n->ehci = true;
        n->ehBase = AaplAddress0(&f->id);
        if (n->ehBase == 0 || (n->ehBase & 0xFFFUL) != (f->pre[4] & 0xFF0UL)) { n->ehBase = 0; continue; }
        if (!(CfgW(f->pre, 0x04) & 0x0002UL) || !CfgDecodes(n)) continue;
        ReadEhci(n, &n->ehBoot);
    }
}

#ifdef UWAKE_MODE_A
typedef OSStatus (*HandlerFn)(UInt32 message, void *param, UInt32 refCon, RegEntryID *regEntryID);
typedef OSStatus (*AddPrioFn)(RegEntryID *regID, HandlerFn handler, UInt32 refCon, char *deviceType, UInt32 priority);

static Ptr ResolveNPM(const char *sym)
{
    static const unsigned char *libs[2] = { "\pNativePowerMgrLib", "\pPowerMgrLib" };
    short i, k; Str255 pname;
    pname[0] = 0; for (k = 0; sym[k]; k++) pname[++pname[0]] = (unsigned char)sym[k];
    for (i = 0; i < 2; i++) {
        CFragConnectionID conn = 0; Ptr mainAddr = NULL, sa = NULL; Str255 errName; CFragSymbolClass cls = 0;
        if (GetSharedLibrary(libs[i], kPowerPCCFragArch, kReferenceCFrag, &conn, &mainAddr, errName) != noErr) continue;
        if (FindSymbol(conn, pname, &sa, &cls) == noErr && sa != NULL) return sa;
    }
    return NULL;
}
static Boolean LookupNode(const char *path, RegEntryID *out);
static RegEntryID gNodeRestore, gNodeEnable;
static Boolean InstallModeA(void)        /* TASK LEVEL (kInitialize) */
{
    RegEntryID devices; AddPrioFn add; OSStatus err;
    add = (AddPrioFn)ResolveNPM("PMAddPrioritizedDevicePowerHandler");
    if (add == NULL) { Lg("!! PMAddPrioritizedDevicePowerHandler not found: nothing installed"); return false; }
    if (!LookupNode("Devices", &devices)) { Lg("!! no Devices node"); return false; }
    err = RegistryCStrEntryCreate(&devices, (const RegCStrPathName *)"UWAK-restore", &gNodeRestore);
    if (err == noErr) err = add(&gNodeRestore, RestoreHandler, 0, (char *)"uwak-restore", 250UL);
    Lg("restore handler (priority 250, before Apple's NEC handlers): %ld", (long)err);
    err = RegistryCStrEntryCreate(&devices, (const RegCStrPathName *)"UWAK-enable", &gNodeEnable);
    if (err == noErr) err = add(&gNodeEnable, EnableHandler, 0, (char *)"uwak-enable", 350UL);
    Lg("enable handler (priority 350, after them): %ld", (long)err);
    RegistryEntryIDDispose(&devices);
    return true;
}
#endif

static Boolean LookupNode(const char *path, RegEntryID *out)
{
    return RegistryCStrEntryLookup(NULL, (const RegCStrPathName *)path, out) == noErr;
}

static void InstallHandlers(void)          /* TASK LEVEL (kInitialize, at boot) */
{
    short i; long lram = 0, vm = 0; char ts[32];
    gBootTicks = kLowTicks;
    LgStamp(ts, sizeof ts);
    Lg("");
    Lg("=== Sleep Assistant %s (%s%s), boot at %s: Apple's own USB stack across the deep sleep of a G4 with the on-board "
       "NEC ===", kUwVer, kUwTag, kUwVariant, ts);
    Lg("Mode %s.", kUwMode);
#ifdef UWAKE_PAINT
    Lg("!! DEV BUILD (UWAKE_PAINT): a status row is painted at the bottom-left of the screen. For troubleshooting only: "
       "never ship this build.");
#endif
#ifdef UWAKE_QTEST
    Lg("!! QEMU TEST BUILD (UWAKE_QTEST): QEMU's OHCI stands in for the NEC; a simulated sleep and wake run 90 s after boot.");
#endif
    if (Gestalt('lram', &lram) == noErr) gLRam = (UInt32)lram; else gLRam = 0x10000000UL;
    gRamReadable = !(Gestalt('vm  ', &vm) == noErr && (vm & 1L));
    Lg("RAM %lu MB, virtual memory %s", (unsigned long)(gLRam >> 20), gRamReadable ? "off" : "ON (the HCCA's done head will not be read)");
    FindUsbFunctions();
    SetUpNec();
    Lg("USB functions in the Name Registry: %d; NEC functions: %d", gFnN, gNecN);
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; UsbFn *f = &gFn[n->fi];
        Lg("  NEC %04lX:%04lX fn %ld class %06lX reg %08lX: cmd %04lX BAR0 %08lX PM cap %02X E0 %08lX E4 %08lX int %08lX",
           f->vid, f->did, (f->reg0 >> 8) & 7UL, f->cls & 0xFFFFFFUL, f->reg0, CfgW(f->pre, 0x04), f->pre[4],
           n->pmOff, f->pre[0xE0 >> 2], f->pre[0xE4 >> 2], f->pre[0x3C >> 2]);
        if (n->ohci) { Lg("    OHCI at %08lX, registers %s", n->base, n->baseOK ? "OK" : "NOT USABLE (no register restore for it)");
                       if (n->baseOK) LogRegs("at boot", &n->launch); }
    }
    if (gNecN == 0 || !gNecReady) { Lg("!! no NEC: nothing installed"); LogFlush(); return; }
    /* Both modes assume APPLE'S driver runs the NEC's OHCI. Path A's ROM UIM marks fn0 'AAPL,ohci-guard' and polls with
       MIE off; Apple's sets MIE (0x8000007B). Either sign of Path A -> install nothing (the ROM was not swapped). */
    for (i = 0; i < gNecN; i++) {
        NecFn *n = &gNec[i]; UsbFn *f = &gFn[n->fi]; RegPropertyValueSize sz = 0;
        if (!n->ohci) continue;
        if (RegistryPropertyGetSize(&f->id, (RegPropertyName *)"AAPL,ohci-guard", &sz) == noErr) {
            Lg("!! NEC fn %ld carries AAPL,ohci-guard: this is Path A's ROM, not the FW800's original. Nothing installed: "
               "put the original Mac OS ROM in the System Folder.", (f->reg0 >> 8) & 7UL);
            LogFlush(); return;
        }
        if (n->baseOK && !(n->launch.intEn & kIntMIE)) {
            Lg("!! NEC fn %ld's OHCI runs with MIE off (InterruptEnable %08lX): not Apple's driver. Nothing installed.",
               (f->reg0 >> 8) & 7UL, n->launch.intEn);
            LogFlush(); return;
        }
    }
    /* Our USB 2.0 ROM makes the NEC's EHCI visible (the stock firmware keeps it hidden: fn0's E4 bit 0) and our EHCI
       registers through the Expert's LoadUIMForEntry, so Apple's teardown would finalize it and CLOSE its connection
       under live references (its disk driver, 'Eusb', NM records), and the reload (device_type "usb" only) would never
       bring it back (desk reading, 2026-11-12). Until that is handled: an EHCI function present -> install nothing. */
#ifdef UWAKE_MODE_B
    for (i = 0; i < gFnN; i++)
        if ((gFn[i].cls & 0xFFFFFFUL) == 0x0C0320UL) {
            Lg("!! an EHCI function (%04lX:%04lX, reg %08lX) is in the Name Registry: this is a USB 2.0 ROM. This test does "
               "not support our EHCI yet. Nothing installed: use the FW800's original Mac OS ROM.", gFn[i].vid, gFn[i].did,
               gFn[i].reg0);
            LogFlush(); return;
        }
#else
    /* ★ uwake19 (the user's scope, 2026-11-13: "it should fix sleep on both the stock ROM and the EHCI ROM, and the
       presence of the EHCI activator extension shouldn't be required for it to function"). The rule above was mode B's:
       Apple's teardown would close our EHCI's connection. Mode A tears nothing down, so a visible EHCI that NO DRIVER RUNS
       is no reason to stand down -- its ports are the companion OHCIs', which Apple's driver runs and this extension
       carries through the sleep. Decided from the HARDWARE, not from a marker our own driver sets, so it also holds on a
       USB 2.0 G4 whose firmware shows the EHCI with no OS 9 driver at all (the Mac mini).
       ★ uwake20 (step 2, 2026-11-17): a RUNNING EHCI (our USB 2.0 driver, started by its activator) no longer stops the
       install either: the demand suspends and halts it before the NEC is armed (EhciDemand), which is what Path A's
       combined ROM lacked when it hung (an EHCI running into the deep sleep, 2026-11-08), and the wake resumes it. The
       decision is taken again at every demand from the live registers, so an EHCI started after boot is handled too. */
    for (i = 0; i < gFnN; i++) {
        UsbFn *f = &gFn[i]; NecFn *n = NULL; short k;
        if ((f->cls & 0xFFFFFFUL) != 0x0C0320UL) continue;
        for (k = 0; k < gNecN; k++) if (gNec[k].fi == i) n = &gNec[k];
        if (n == NULL) {
            Lg("  an EHCI that is not the NEC's (%04lX:%04lX, reg %08lX): not this extension's to handle; left alone",
               f->vid, f->did, f->reg0);
            continue;
        }
        if (!(CfgW(f->pre, 0x04) & 0x0002UL)) {
            Lg("  NEC EHCI fn %ld: memory decode OFF at boot (cmd %04lX), so no driver runs it: its ports are the OHCIs'.",
               (f->reg0 >> 8) & 7UL, CfgW(f->pre, 0x04));
            continue;
        }
        if (n->ehBase == 0 || !n->ehBoot.ok) {
            Lg("!! NEC EHCI fn %ld decodes memory (cmd %04lX) but its registers could not be read (AAPL,address %08lX, "
               "BAR0 %08lX, HCIVERSION/CAPLENGTH word %08lX): whether a driver runs it is unknown. Nothing installed.",
               (f->reg0 >> 8) & 7UL, CfgW(f->pre, 0x04), AaplAddress0(&f->id), f->pre[4], n->ehBoot.cap);
            LogFlush(); return;
        }
        LogEhci("at boot", &n->ehBoot);
        if (EhciRunning(&n->ehBoot))
            Lg("  NEC EHCI fn %ld is RUNNING (a USB 2.0 driver started it: our MDD G4 EHCI extension, or another), "
               "CONFIGFLAG %lu. uwake20: at each sleep demand its enabled ports are suspended and it is halted (PCI D3hot) "
               "before the NEC is armed; at the wake it runs again and the ports are resumed. Installing.",
               (f->reg0 >> 8) & 7UL, n->ehBoot.cfg & 1UL);
        else
            Lg("  NEC EHCI fn %ld: shown by this ROM, stopped, and routing no port to itself: no driver runs it, so its "
               "ports are the OHCIs'. Installing.", (f->reg0 >> 8) & 7UL);
    }
#endif
    /* (uwake20: the line said "the FW800's original ROM is in", which the USB 2.0 ROM printed too) */
    Lg("Apple's driver runs the NEC's OHCI (MIE on, no Path A mark): not Path A's ROM.");
    (void)UslLocate();                    /* the add-lock readings (diagnostic; off unless verified) */
#ifdef UWAKE_MODE_A
    if (!InstallModeA()) { LogFlush(); return; }
    KwInstall();                          /* uwake11: USBDeviceRequest, the NEC functions' registry paths */
#else
    if (!InstallPbMode()) { LogFlush(); return; }
#endif
    gInstalled = true;
    gHbUPP = NewTimerUPP(HeartbeatTask);
    if (gHbUPP) { memset(&gHbTask, 0, sizeof gHbTask); gHbTask.tmAddr = gHbUPP; InsXTime((QElemPtr)&gHbTask); PrimeTime((QElemPtr)&gHbTask, 250); }
    Lg("Time Manager heartbeat: %s", gHbUPP ? "on" : "FAILED");
    Lg("uwake14: the task-level pass (a Notification Manager request) now runs only while it has work: Mac OS counts every "
       "request as system activity, and a pass that never stopped kept the Mac from ever taking an idle sleep.");
    ArmNM();
#ifdef UWAKE_QTEST_PUMP
    QtPumpArm();
#endif
#ifdef UWAKE_PAINT
    Lg("Readings after a wake: the status row at the bottom-left (12 x 12 cells), then this log (written ~%lu min after the wake).",
       (unsigned long)(kRecEndMs / 60000UL));
#else
    Lg("Readings after a wake: this log (written ~%lu min after the wake).", (unsigned long)(kRecEndMs / 60000UL));
#endif
    LogFlush();
}

/* ============================================================== native-driver shape */
typedef struct { UInt8 len; char s[31]; } DStr31;
typedef struct { UInt8 a, b, c, d; } DNumVer;
typedef struct { OSType category, type; DNumVer version; } DServiceInfo;
typedef struct {
    OSType sig; UInt32 descVersion; DStr31 nameInfoStr; DNumVer typeVersion;
    UInt32 driverRuntime; DStr31 driverName; UInt32 reserved[8];
    UInt32 nServices; DServiceInfo service0;
} ResidentDriverDesc;
ResidentDriverDesc TheDriverDescription = {
    0x6d74656aUL /* 'mtej' */, 0,
    { 11, "uwak,resdnt" }, { 1, 0, 0x20, 1 },
    0x00000003UL,
    { 12, "FW800USBWake" }, { 0,0,0,0,0,0,0,0 }, 1,
    { 0x6e647276UL /*'ndrv'*/, 0x6d697363UL /*'misc'*/, { 1, 0, 0x20, 1 } }
};

/* uwake6: ONE copy. A copy under an old file name ("FW800 USB Wake" / "FW800USBWake") left in Extensions beside the
   renamed "FW800 Sleep Assistant" would install a second resident driver, a second sleep-procedure wrapper attempt and a
   second put-away hook. The first copy to load owns Devices:FW800USBWake (its node, created below); a later copy finds
   it, counts itself there in "uwake,stood-down", and installs nothing. The running copy logs the count (DupCheck). */
#define kDupProp "uwake,stood-down"
/* Called by the 68K INIT via Mixed Mode: make this fragment resident, which fires DoDriverIO(kInitialize). */
void InstallMe(void)
{
    Handle h; Ptr img; Size sz; RegEntryID devices, node, prior; DriverRefNum ref = 0; OSErr err;
    RegistryEntryIDInit(&prior);
    if (LookupNode("Devices:FW800USBWake", &prior)) {
        UInt32 n = 0; RegPropertyValueSize n4 = sizeof n;
        /* The count is only a message for the log: if recording it fails, standing down still happens (unchecked on
           purpose, nothing depends on it). */
        if (RegistryPropertyGet(&prior, (RegPropertyName *)kDupProp, &n, &n4) == noErr) {
            n++; (void)RegistryPropertySet(&prior, (RegPropertyName *)kDupProp, &n, sizeof n);
        } else {
            n = 1; (void)RegistryPropertyCreate(&prior, (RegPropertyName *)kDupProp, &n, sizeof n);
        }
        RegistryEntryIDDispose(&prior);
        return;
    }
    RegistryEntryIDDispose(&prior);
    h = Get1Resource('PPC ', 128);
    if (h == NULL) return;
    sz = GetHandleSize(h);
    img = NewPtrSys(sz);
    if (img == NULL) return;
    HLock(h); BlockMoveData(*h, img, sz); HUnlock(h);
    (void)HoldMemory(img, sz);                 /* keep the resident code off the VM pager */
    if (!LookupNode("Devices", &devices)) { DisposePtr(img); return; }
    err = RegistryCStrEntryCreate(&devices, (const RegCStrPathName *)"FW800USBWake", &node);
    RegistryEntryIDDispose(&devices);
    if (err != noErr) { DisposePtr(img); return; }
    err = InstallDriverFromMemory(img, (long)sz, "\pFW800USBWake", &node, 48, 127, &ref);
    if (err != noErr) DisposePtr(img);
}

OSErr DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                 IOCommandContents contents, IOCommandCode code, IOCommandKind kind)
{
    OSErr err = noErr;
    (void)spaceID; (void)contents;
    switch (code) {
        case kInitializeCommand: InstallHandlers(); err = noErr; break;
        case kOpenCommand: case kCloseCommand:
        case kFinalizeCommand: case kSupersededCommand:
        case kReplaceCommand: case kKillIOCommand:  err = noErr; break;
        case kReadCommand: case kWriteCommand:      err = ioErr; break;
        case kControlCommand: err = noErr; break;
        case kStatusCommand:  err = statusErr; break;
        default:              err = paramErr; break;
    }
    if (kind & kImmediateIOCommandKind) return err;
    return (OSErr)IOCommandIsComplete(cmdID, (short)err);
}
