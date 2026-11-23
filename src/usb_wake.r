/* FW800 Sleep Assistant packaging (the "FW800 USB Wake" experiment, renamed 2026-11-12 at the user's request): the 68K
 * INIT code resource (usb_wake_init.flt) + the embedded PowerPC resident-driver fragment (usb_wake_resident.pef) as a
 * 'PPC ' (128) resource. The INIT loads it via Get1Resource('PPC ',128) -> GetMemFragment to call InstallMe; InstallMe
 * re-loads the same resource and InstallDriverFromMemory's it as the resident driver (NECQuiet's vehicle). Internal
 * names stay "FW800USBWake" (the driver, its Name Registry node): a second copy under an old file name must see the
 * first one's node and stand down. The file's name is set on the MacBinary by scripts/set-custom-icon.py --name.
 *
 * ⚠ A 'vers' string is a Pascal string: Rez silently cuts it at 255 bytes (uwake5B's long string lost its tail, "Log:"
 * included). check-vers.py fails the build on a 255-byte string. */
#include "Retro68.r"
#include "Types.r"

type 'INIT' {
	RETRO68_CODE_TYPE
};

resource 'INIT' (128, locked) {
	dontBreakAtEntry, $$read("usb_wake_init.flt");
};

data 'PPC ' (128) {
	$$read("usb_wake_resident.pef")
};

/* The file's own Finder icon: the family at kCustomIconResource (-16455), the user's design (Pi share, "custom icons/
   FW800 Sleep Assistant", its sidecar md5 fb53c9c6), turned into Rez source by scripts/custom-icon-to-r.py and
   committed so the build never needs the share. The other half, the hasCustomIcon Finder flag, is set on the
   MacBinary by scripts/set-custom-icon.py. ⚠ Rez does not track this #include: the build script reruns Rez every time. */
#include "sleep_assistant_icon.r"

#ifdef UWAKE_MODE_B
resource 'vers' (1, "Sleep Assistant") {
	0x01,
	0x00,
	development,
	0x01,
	verUS,
	"1.0d1",
	"1.0d1 (uwake17B) EXPERIMENT, mode B: Apple's own USB stack across the deep sleep (Apple's PowerBook sleep mode), USB disks and servers put away before sleep, and a recorder. Log: System Folder:Sleep Assistant Log. Original Mac OS ROM only."
};

resource 'vers' (2, "Sleep Assistant") {
	0x01,
	0x00,
	development,
	0x01,
	verUS,
	"1.0d1",
	"Sleep Assistant B (experiment)"
};
#elif defined(UWAKE_PAINT)
/* uwake22, the DEV build (status row painted): never shipped, and Get Info says so. */
resource 'vers' (1, "Sleep Assistant") {
	0x01,
	0x00,
	development,
	0x00,
	verUS,
	"1.0 DEV",
	"1.0 DEV (uwake22A): paints a status row at the bottom-left of the screen. For troubleshooting only, never ship. Log: System Folder:Sleep Assistant Log."
};

resource 'vers' (2, "Sleep Assistant") {
	0x01,
	0x00,
	development,
	0x00,
	verUS,
	"1.0 DEV",
	"Sleep Assistant 1.0 DEV"
};
#else
/* ★ 1.0 (user, 2026-11-17: "go ahead with 1.0"), uwake22A. A release shows the plain version and nothing else, as
   AirPort's do (Get Info's Version line = vers 1's long string, Extensions Manager = vers 1's short and vers 2's long,
   Apple System Profiler = the numeric bytes); the build is in the log's banner ("Sleep Assistant 1.0 (uwake22A)"). The
   dev variant above keeps the status row. */
resource 'vers' (1, "Sleep Assistant") {
	0x01,
	0x00,
	release,
	0x00,
	verUS,
	"1.0",
	"1.0"
};

resource 'vers' (2, "Sleep Assistant") {
	0x01,
	0x00,
	release,
	0x00,
	verUS,
	"1.0",
	"Sleep Assistant 1.0"
};
#endif
