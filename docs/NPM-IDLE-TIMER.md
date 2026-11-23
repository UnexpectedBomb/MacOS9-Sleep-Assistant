# Why only the display sleeps: every NMInstall is "activity" to OS 9's system-sleep timer (read 2026-11-14)

Read from the FW800's own ROM: the 68K toolbox (`Mac68KROM` of the stock `Mac OS ROM`, dumped with Elliot Nunn's
tbxi tools) and NativePowerMgrLib (md5 `d9e42f48`, function names from the PPC traceback tables, disassembled with
capstone). Apple's own source for the 68K side (the System 7.1-era tree in Elliot Nunn's `boot3`):
`Toolbox/NotificationMgr/NotificationMgr.c` (`_NMInstall`) and `OS/PowerMgr/PowerMgr.a`.

```
NMInstall (Mac68KROM +0x04AD40 = NotificationMgr.c _NMInstall; its history: "<3> 6/14/91 SAM (HJR) Added a call to
          IdleUpdate() on _NMInstall calls to wake the system up when NM procs want to run")
  if (GetOSTrapAddress(0xA085) != GetToolTrapAddress(0xA89F)) _IdleUpdate();
          bytes: a346 2d48fffc 303ca89f a746 202efffc b1c0 6702 a285 426c0006 7000 29400008 (nmFlags = 0, nmPrivate = 0)
NativePowerMgrLib InstallPMgrOpTrap: trap 0x85 (PMgrOp) IS implemented, so the test passes on every NMInstall
__PowerMgrOpDispatch (+0x099B0), by trap word: $A085 PMgrOp; $A285 IdleUpdate -> PMIdleUpdate(0) = OverallAct
          (glue 0x19210, the same callee as UpdateSystemActivity(type) and PowerDispatch selector 1); $A485; $A685
PMIdleUpdate(type) (+0x02000):
  nothing if G+0x468 bit 0 (set by DoHDSpinDown only WHILE it runs, cleared at its end) or type outside 0..4
  1 (user), 4:  overall clock = Ticks (unless it is already later; G+0x4E1 / G+0xBF suppress); type 1 also sets the
                user clock and calls CheckForUnDim
  0, 2, 3:      at most once per G+0xB0 = 600 ticks (10 s, InitPowerMgrGlobals; skipped while last + 600 >= Ticks),
                unless IgnoreAnythingButUsrActivityTime() (as compiled it compares an OSErr: false in practice) or
                G+0x4E3 (type 2) / G+0x4E2 (type 3):  overall clock = min(overall, Ticks) + 600
                type 2 also sets the network clock; type 3 sets the disk clock = Ticks, G+0x54 |= 4
Clocks (PMGetSetActivityTicks; G = PmgrBase, LM 0x0D18): overall G+0x1A, user G+0x188, network G+0x18C, disk G+0x1E
CheckSleepTimer (+0x08A38): PMHasActivityExpired(0): the system sleeps when Ticks > overall + timeout (the timeout
                byte x 900 ticks: 15 s units); then G+0x2FA = 1 (the raw idle type our put-away hook reads) and the request
CheckDimTimer (+0x08898): PMGetActivityDuration(1): the display timer counts USER activity only
ResetDimSleepHDTimers (+0x087D0): dim timer <- user clock, sleep timer <- overall clock, disk timer <- disk clock
```

**Added 2026-11-14 (uwake15A's hardware log, then the disassembly):**

```
ONE door: every call to PMIdleUpdate goes through the glue at +0x19210 (addi r12,r2,0x24; lwz r0,0(r12); lwz r2,4(r12);
          bctr): a transition vector stored INSIDE NPM's TOC at r2+0x24 (data section shared globally, kind 2 share 4).
          13 call sites: __PowerMgrOpDispatch ($A285, type 0), UpdateSystemActivity (its argument), __PowerDispatch
          (selector 1: the type from D0's high word), PMIdleDelay (type 0; no direct callers, so through a vector:
          DelaySystemIdle's glue loads r2+0x2C, which is probably it, NOT verified), and NPM's own WeShouldDoze/HandleDoze/HandleSleep/HandleSleepOrDozeAborted
          (type 4), WakeFromSleep/EnvironInterruptCallback/WakeFromDoze (type 1). So type 0 comes ONLY from outside.
IgnoreAnythingButUsrActivityTime (+0x01EB8): min(45000, PMGetActivityDuration(type 0)) > PMHasActivityExpired(0)
DoHDSpinDown (+0x0DB6C): G+0x468 |= 1; unless G+0x129 says otherwise, G+0x54 &= ~4, WalkHardDiskQ(0) and (1) (each
          HardDiskQ element's proc), then the DISK CLOCK = 0; G+0x468 &= ~1
HardDiskPowered (+0x040CC): the disk clock != 0. A type-3 report sets it to Ticks (and G+0x54 |= 4); the spin-down zeroes it.
CheckHDTimer (+0x08B68): no spin-down while the disk clock is 0, G+0x16 is 0, or the other gate is closed; else when
          PMHasActivityExpired(3) says so, the spin-down through G+0x16
The FW800's 68K ROM (Mac68KROM) holds ONE _IdleUpdate trap word: NMInstall's (+0x04AD40). Apple's 1994 Device Manager
          called _IdleUpdate on every _Read and _Write ("this is activity, stay at full speed"); that is gone. Native
          code reaches the trap through a small glue at Mac68KROM +0x10E524 (r5 = $A285, no direct callers: exported).
```

Seen on the FW800 with uwake15A (its hardware log, 2026-11-14): after Sleep Assistant went quiet, a
reporter that is NOT a notification pushed the system clock every 11-13 s from ~4 min after boot until the hard disk
spun down (the disk clock went to 0); the idle sleep followed 600 s after the last effective push, 19 min 29 s after
boot. Each type-0 push gives back only 10 s, so a reporter slower than 10 s does not stop idle sleep, it stretches it.

**Consequence.** Each NMInstall reports overall activity. One report per 10 s counts, and it gives the system timer
10 s back. So any caller that posts at least every 10 s holds the system's idle clock almost still (it gains a tick or
two per 10 s: a 10-minute timer would need days), while the display timer, which counts only keyboard and mouse, runs
out on time. That is the FW800's symptom exactly: "only the display sleeps", and no idle sleep ever reaches the
put-away hook.

**Our Notification Manager pumps (2026-11-14):**

| component | how often it calls NMInstall | effect on idle sleep |
|---|---|---|
| Sleep Assistant, every uwake build (`NmPass`: "re-arm: one pass per event-loop turn") | every event-loop turn | blocks it on its own |
| AirPort driver (`ap_shim.c` `ApLinkPost`, `gLinkNm`) | every 5 s, also with the radio off (its log: `nm` +6 per 30-s line) | blocks it on its own (two posts are ~601.5 ticks apart, just over 600) |
| USB 2.0 EHCI (`bootmain.c` re-arms every pass once activated; `ehci_vhub.c` `gNMT` from the 8 ms heartbeat) | continuous / frequent | expected to block it (not measured) |
| Bluetooth stack (`bt_defer.c`, `bt_switch.c`) | on demand, coalesced | **measured 2026-11-15 (DBT-120, `e3883cf6`): 3 reports at connect (names, the first mouse report, the battery level), none in the next 52 min; does not hold it off** |
| CPU temperature CSM (`FireAlert`) | only the over-temperature alert | not a pump |

**Fix direction.** Post only when there is task-level work, and give a stopped pump a restart point (Sleep Assistant:
the put-away hook, at task level inside HandleSleep, can post it so the first pass comes right after the wake).
**Measure** with the four clocks above: they are plain memory, so a Time Manager task can sample them with no side
effects (a sampler that calls NMInstall would push the very clock it measures).

## Other reporters, named by uwake17A's hook (2026-11-15)

| reporter | route | effect |
|---|---|---|
| our Bluetooth strip 2.6 (fixed in idlefix1) | `ControlStripModule > Refresh > LoadDevices > FSRead`, every ~5.85 s while the disk spins | idle sleep ~19-20 min after boot |
| our Bluetooth control panel 19.7, **while open** (fixed in idlefix1, **confirmed 2026-11-16**, log `40f26013`: idle sleep 10 min 3 s after the last input with the panel open) | `StatusWrite <- main`, `_Write`, every ~15 s | ~28 min after the last input |
| **AppleShare over TCP, any mounted server volume** (Apple's) | `OTPowerUpdate` from `TNativeEndpoint::Snd` (the client's DSI tickle, `TSession::RunQueue2` from `VBLDefTask`) and from `TNativeEndpoint::Rcv` (the server's, `DoReadDSIntHdr`): `UpdateSystemActivity`, **network** | each side tickles ~every 30 s: 15 min after the last input, up to 30 when the two drift apart (`e3883cf6`: 18 min 23 s, 50 pushes) |
| an Apple system-heap file reader (unnamed) | `_Read`/`_Open`, 5 pushes 60 s apart, then it stops | up to ~50 s |

Every Open Transport send and receive is network activity (`OTSnd`/`OTRcv` -> `OTPowerUpdate`), so a mounted
AFP-over-TCP volume stretches idle sleep by Apple's own design. **Idle-sleep tests must put server volumes away
before the wait.** The same holds on AirPort, where OT traffic rides on top of whatever our driver itself reports.
