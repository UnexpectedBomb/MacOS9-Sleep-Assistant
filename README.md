# MacOS9-Sleep-Assistant

A **Mac OS 9 system extension that lets a G4 Mac with an NEC USB 2.0 controller sleep and wake properly.** On the Power Mac G4 "Mirrored Drive Doors" FireWire 800 (PowerMac3,6), a Mac OS 9 deep sleep used to end
in a hang and/or a dead keyboard and mouse. With Sleep Assistant installed, Special > Sleep, Energy Saver's idle sleep
and the power key all work: a key on the USB keyboard wakes the Mac, the keyboard and mouse keep working, and USB
disks stay mounted.

Apple never supported Mac OS 9 on the FW800 MDD. Its USB ports are on a separate NEC chip that loses its state in
deep sleep, and Apple's stock OS 9 USB driver does not know how to handle this correctly. Sleep Assistant prepares the chip before the sleep so that it keeps its state, turns on USB remote wakeup
so a key can wake the Mac, and checks the chip again at the wake before handing it back to Apple's driver.

> **Status: 1.0.1. Its sleep and wake code, unchanged since 1.0, works on real hardware.** Tested on a Power Mac G4
> MDD FireWire 800 running Mac OS 9.2.2, with the
> stock Mac OS ROM and with the USB 2.0 ROM from [MacOS9-USB2-EHCI](https://github.com/UnexpectedBomb/MacOS9-USB2-EHCI),
> alongside AirPort Extreme, Bluetooth and the FireWire 800 fix. It is a hobby project, offered as-is; see
> [Known issues](#known-issues).

## What it does

At every sleep (chosen from the Special menu, Energy Saver's idle sleep, or the tower's front power button):

- **Puts mounted server volumes away first** (AppleShare), so the Mac does not wake to a dead connection. USB disks
  stay mounted. When Mac OS is going to refuse an idle sleep anyway (for example while File Sharing is on), nothing
  is put away.
- **Arms the NEC controller to keep its state** through the deep sleep, the way Apple's own USB keeps power on the
  earlier G4s.
- **Turns on USB remote wakeup** on the controller the keyboard is on, so a key press wakes the Mac. A Bluetooth
  controller can wake it too, if you allow that in the Bluetooth control panel.
- **If the USB 2.0 driver is running,** suspends its ports and halts it cleanly, and resumes it in place at the wake,
  so a USB 2.0 disk stays mounted too.

At every wake it puts back anything the sleep did lose, then hands the controllers back to Apple's driver.

It also lets **Energy Saver's idle sleep** work with the USB 2.0 driver installed: the driver's background keepalive
no longer counts as "activity".

Sleep Assistant writes a log called `Sleep Assistant Log` to the System Folder, about six minutes after each wake. It records
what happened at sleep and wake; send this log if you are reporting a problem. It grows by roughly 100
to 200 KB per wake, and it keeps itself under 10 MB: when it nears 10 MB, the next startup or wake removes its oldest
part, so it always holds the most recent 9 to 10 MB. Delete it whenever you like and a new one is started.

## Compatible G4 Macs

Sleep Assistant does not make a Mac sleep; it just keeps the NEC USB chip working through sleep, preventing a crash.
It can help a G4 only if:

1) Mac OS 9 can sleep that Mac's processor
2) The Mac's USB is on the NEC chip.

| Mac | Processor | NEC USB 2.0 | Sleep Assistant |
|---|---|---|---|
| Power Mac G4 MDD FireWire 800 (PowerMac3,6) | 7455 | yes | **tested, works** |
| iMac G4 15-inch 1.0 GHz, 17- and 20-inch 1.25 GHz (2003, PowerMac6,3) | 7445 (7455 family) | yes | may work, **untested**: reports welcome |
| AlBook G4 12-inch 867 MHz (PowerBook6,1), 17-inch 1.0 GHz (PowerBook5,1) | 7455 | no (USB 1.1) | nothing for it to do |
| AlBook G4 (Sep 2003 onwards), iBook G4, eMac 1.25/1.42 GHz, Mac mini G4 | 7447, 7457 or 7447A | yes | **DO NOT USE** (below) |

Mac OS 9's processor sleep code does not know the 7447, 7457 or 7447A. On those Macs, a sleep crashes (or Energy Saver
reports missing software) before USB is ever involved; that is why the Mac Mini's OS 9 installer removes Energy Saver.
Sleep Assistant cannot fix that (for now), so leave it out on those Macs. I am currently working on a sleep fix for the G4 Mini specifically, which I'm hoping will unlock many other G4 machines if successful. Stay tuned!

## Install

1. Download `SleepAssistant_1.0.1.bin` from [Releases](../../releases) and expand it with StuffIt Expander. You get
   one file, **Sleep Assistant**, with the Energy Saver icon.
2. Check its Get Info: the version reads **1.0.1**.
3. Drop it into the **Extensions** folder in the System Folder, and restart. Updating from 1.0: replace the old file
   in the Extensions folder with the new one, and restart. Your existing log carries on; if it is over 10 MB, the
   first startup removes its oldest part.
4. Sleep as usual. Wake it with a key on a USB keyboard, the power button, or (if allowed) a Bluetooth keyboard or
   mouse.

It works with or without the USB 2.0 driver, on the stock Mac OS ROM or the USB 2.0 ROM. Install one copy only; a
second copy stands down. To remove it, drag it out of the Extensions folder and restart.

## Known issues

- **FireWire 800 disks with the USB 2.0 driver running.** One LaCie FireWire 800 disk, connected FW800-to-FW800, often
  stopped responding after a wake while the USB 2.0 driver was running: Apple's "A FireWire disk has stopped
  responding" alert stayed up until its cable was unplugged and plugged back in. That disk has since failed outright,
  so this may have been the disk, but it was never seen without the USB 2.0 driver running. To avoid it, eject
  FireWire disks before sleeping, or connect them FW400. If the alert does stay, unplug the FireWire cable for a few
  seconds and plug it back in.
- **Server volumes are put away at every sleep**; mount them again after the wake.

## Versions

- **1.0.1**: the log keeps itself under 10 MB by removing its oldest part (see above); 1.0's log had no limit.
  Nothing else changed: the sleep and wake code is 1.0's.
- **1.0**: the first release.

## How it works

Sleep Assistant is a small 68K INIT that loads a resident PowerPC native driver at startup. The driver registers two
power handlers with Mac OS's Native Power Manager: one runs before Apple's USB handlers at the sleep and at the wake,
the other after them.

- **At the sleep,** after Apple's USB driver has suspended the bus, it arms each of the NEC's two USB 1.1 (OHCI)
  functions for PCI power management (PME enabled, D3hot). Armed like that, the NEC keeps its controller state and
  its port power through the deep sleep, where unarmed it loses everything. On the keyboard's function it sets the
  controller's remote-wakeup bits. A running USB 2.0 (EHCI) function gets its ports suspended, is halted and is put
  in D3hot.
- **At the wake** it brings each function back to D0 and restores its PCI configuration (the command register is
  what the sleep resets), restores the OHCI registers only if they were lost, and resumes the EHCI's ports in place.
  Apple's driver then carries on as if nothing happened.
- **Before the sleep,** a hook in the Power Manager's sleep path puts server volumes away, before Mac OS decides
  whether the sleep may go ahead.

Two notes in `docs/` explain the Mac OS side: why, with some drivers installed, only the display ever slept
([NPM-IDLE-TIMER.md](docs/NPM-IDLE-TIMER.md): every Notification Manager request counts as activity), and what
decides whether Mac OS 9 lets a sleep go ahead ([NPM-CHECKAPPLETALK.md](docs/NPM-CHECKAPPLETALK.md)).

## Building

See [BUILD.md](BUILD.md). The release file is built from this source: rebuilding it gives byte-identical code and
resources, apart from the file dates and two toolchain file paths that BUILD.md explains.

## Credits and license

Original work, released under the **MIT License**; see [LICENSE](LICENSE). Linux's `ohci-hcd` and `ehci-hcd` suspend
and resume paths were the reference for the controller steps, and Elliot Nunn's ROM tools made the Power Manager's
code readable.

The icon is Apple's Energy Saver control strip module icon. Energy Saver is a trademark of Apple Inc. This is an
independent, unofficial extension, not affiliated with or endorsed by Apple.

## Related

- [MacOS9-USB2-EHCI](https://github.com/UnexpectedBomb/MacOS9-USB2-EHCI): USB 2.0 for Mac OS 9
- [MacOS9-FW800-FireWire](https://github.com/UnexpectedBomb/MacOS9-FW800-FireWire): the FireWire 800 fix for the MDD
- [MacOS9-Airport-Extreme](https://github.com/UnexpectedBomb/MacOS9-Airport-Extreme): AirPort Extreme for Mac OS 9
- [MacOS9-Bluetooth](https://github.com/UnexpectedBomb/MacOS9-Bluetooth): Bluetooth for Mac OS 9
