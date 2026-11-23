# What decides whether OS 9 lets a sleep go ahead: NativePowerMgrLib's CheckAppleTalk (read 2026-11-14)

Read from the shipped NativePowerMgrLib (md5 `d9e42f48`, the FW800's ROM; function names from the PPC traceback tables,
disassembled with capstone). HandleSleep calls our put-away hook
(PmgrBase+0x60) at +0x168 and CheckAppleTalk at +0x1CC, so **the hook always runs before this decision**.

```
CheckAppleTalk(type)                         returns 1 = refuse (silently for idle; the alert's answer for chosen)
  if ((*(UInt8 *)0x291 & 0x0F) == 0) return 0;          AppleTalk not active (PortBUse low nibble): allow anything
  if (AutoRemounterEngaged()) return 0;                 PMGetWakeOnNetActOptions() & 8: allow anything, no checks
  switch (type):
    1, 12  (IDLE):   refuse if RemoteVolumeMounted() || SharingActive()          (silently: only the display sleeps)
    2, 13  (CHOSEN): if RemoteVolumeMounted(): AskUserIfOkayToSleep(-0x4002)     (the alert; Cancel refuses)
    6, other:        allow
SharingActive()        = FileSharingActive() || PrinterSharingActive() || WebSharingActive()
FileSharingActive()    s = ServerStatus() (_ServerDispatch A094, csCode 5; status = the pb's word at +0x1C, -50 if the
                       trap is unimplemented); v = ServerVersion() (csCode 14):
                         v == 0: active iff s == -1 && LM 0xB50 != 0 && !(LM 0xB50 & 1) && *(SInt16 *)(LM 0xB50 + 0x10) > 0
                         v == 1: active iff s == -1 || s == -2
PrinterSharingActive() Gestalt('zak ') == noErr && (response & 0x80000000)
WebSharingActive()     a process of type 'appe', creator 'wbsh' is running
ServerDispatch(pb)     only if the A094 trap is implemented (its address != _Unimplemented A89F's): pb+0x1C..0x22 = 0,
                       CallOSTrapUniversalProc(*(UPP *)0x650, 0x02671822, 0, 0xA094, pb)
```

**Consequences for the Sleep Assistant's put-away hook:**
- With AppleTalk active and File Sharing (or printer or web sharing) on, **Mac OS never takes an idle sleep**, by
  design (connected users would be cut off). Only the display sleeps. This is Apple's behaviour on every Mac.
- uwake12A put servers away for idle sleeps before this check: with sharing on it unmounted the share and Mac OS then
  refused the sleep anyway, at every idle-timer expiry (an earlier prototype's comment had warned of exactly this).
  The hook must mirror the rule above for idle sleeps: if Mac OS is going to refuse, put nothing away.
- With the Auto Remounter engaged (bit 8), Mac OS allows every sleep with no server check at all: the hook must put
  servers away itself then (uwake11A's scheduled-sleep hang).
