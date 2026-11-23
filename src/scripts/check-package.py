#!/usr/bin/env python3
"""check-package.py <final.bin> <expected Mac name> <expected tag> [<release version>] -- check Sleep Assistant's
MacBinary before it is staged. Exit 0 = every check passed; it prints what it read either way.

  * uwake22 (1.0): with a release version (a SHIP build), Get Info must show that version and nothing else, as
    AirPort's releases do: vers 1 = "<v>" / "<v>", vers 2 = "<v>" / "Sleep Assistant <v>", the numeric bytes the
    version with stage final; the tag is then checked in the code only, and the code must not be a DEV build's.
    Without it (a dev or test build) vers 1's long string must carry the tag, as before.

  * the MacBinary II header: the internal name (what StuffIt Expander names the file: the user asked for
    "FW800 Sleep Assistant"), type INIT / creator RSED, the hasCustomIcon Finder flag (0x0400), a valid CRC-16;
  * the resource fork: 'INIT' 128, 'PPC ' 128, the whole custom icon family at -16455 (ICN#, icl4, icl8, ics#,
    ics4, ics8, icns), and 'vers' 1 + 2;
  * the 'vers' strings: a long string of exactly 255 bytes is one Rez CUT (uwake5B's lost its tail silently), so it
    fails; vers 1's long string must carry the build's tag;
  * the 'PPC ' fragment must carry the tag too (its banner string): a stale PEF under a new vers fails.
"""
import struct
import sys

WANT_ICONS = ('ICN#', 'icl4', 'icl8', 'ics#', 'ics4', 'ics8', 'icns')


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__.strip().splitlines()[0])
        return 2
    path, want_name, want_tag = sys.argv[1], sys.argv[2], sys.argv[3]
    want_rel = sys.argv[4] if len(sys.argv) == 5 else None
    d = open(path, 'rb').read()
    bad = []
    n = d[1]
    name = d[2:2 + n].decode('mac-roman')
    ftype, creator = d[65:69], d[69:73]
    flags = (d[73] << 8) | d[101]
    crc_ok = struct.unpack('>H', d[124:126])[0] == crc16(d[0:124])
    print('check-package: %s' % path)
    print('  MacBinary name %r, type %r creator %r, Finder flags 0x%04X, CRC %s'
          % (name, ftype, creator, flags, 'ok' if crc_ok else 'BAD'))
    if name != want_name:
        bad.append('the name is %r, not %r' % (name, want_name))
    if ftype != b'INIT' or creator != b'RSED':
        bad.append('type/creator %r/%r, not INIT/RSED' % (ftype, creator))
    if not flags & 0x0400:
        bad.append('hasCustomIcon (0x0400) is not set')
    if not crc_ok:
        bad.append('the MacBinary CRC is wrong (StuffIt Expander would refuse it)')

    dl = struct.unpack('>I', d[83:87])[0]
    rl = struct.unpack('>I', d[87:91])[0]
    rs = 128 + ((dl + 127) // 128) * 128
    rf = d[rs:rs + rl]
    d_off, m_off, _, m_len = struct.unpack('>IIII', rf[0:16])
    m = rf[m_off:m_off + m_len]
    tl = struct.unpack('>H', m[24:26])[0]
    nt = struct.unpack('>h', m[tl:tl + 2])[0] + 1
    res = {}
    for t in range(nt):
        e = tl + 2 + t * 8
        typ = m[e:e + 4].decode('mac-roman')
        cnt = struct.unpack('>h', m[e + 4:e + 6])[0] + 1
        rlo = struct.unpack('>H', m[e + 6:e + 8])[0]
        for k in range(cnt):
            r = tl + rlo + k * 12
            rid = struct.unpack('>h', m[r:r + 2])[0]
            dofs = struct.unpack('>I', b'\0' + m[r + 5:r + 8])[0]
            blen = struct.unpack('>I', rf[d_off + dofs:d_off + dofs + 4])[0]
            res[(typ, rid)] = rf[d_off + dofs + 4:d_off + dofs + 4 + blen]
    print('  resources: %s' % ', '.join('%s %d (%d)' % (t, i, len(b)) for (t, i), b in sorted(res.items())))
    for key in (('INIT', 128), ('PPC ', 128), ('vers', 1), ('vers', 2)):
        if key not in res:
            bad.append('no %r %d' % key)
    missing = [t for t in WANT_ICONS if (t, -16455) not in res]
    if missing:
        bad.append('the icon family lacks %s at -16455' % ', '.join(missing))
    for vid in (1, 2):
        b = res.get(('vers', vid))
        if b is None:
            continue
        sl = b[6]
        short = b[7:7 + sl].decode('mac-roman')
        ll = b[7 + sl]
        long_s = b[8 + sl:8 + sl + ll].decode('mac-roman')
        print('  vers %d: %02X %02X stage %02X rel %d, %r / (%d bytes) %r' % (vid, b[0], b[1], b[2], b[3], short, ll, long_s))
        if ll >= 255:
            bad.append('vers %d long string is %d bytes: Rez cut it' % (vid, ll))
        if want_rel is None:
            if vid == 1 and want_tag not in long_s:
                bad.append('vers 1 does not carry the tag %r' % want_tag)
            continue
        want_long = want_rel if vid == 1 else 'Sleep Assistant ' + want_rel
        if short != want_rel or long_s != want_long:
            bad.append('vers %d reads %r / %r, not the release %r / %r' % (vid, short, long_s, want_rel, want_long))
        maj, _, mnr = want_rel.partition('.')
        mnr = mnr.split('.')
        want_num = (int(maj), (int(mnr[0]) << 4) | (int(mnr[1]) if len(mnr) > 1 else 0), 0x80, 0)
        if tuple(b[0:4]) != want_num:
            bad.append('vers %d numeric %02X %02X %02X %02X, not %02X %02X %02X %02X (the release, stage final)'
                       % ((vid,) + tuple(b[0:4]) + want_num))
    # uwake11: the CODE must carry the tag too (its log banner prints kUwTag). The build script once packaged the
    # previous build's PEF after a failed compile; the vers string, from Rez, still read the new tag.
    ppc = res.get(('PPC ', 128))
    if ppc is not None:
        found = want_tag.encode('ascii') in ppc
        print('  PPC 128 carries the tag %r: %s' % (want_tag, 'yes' if found else 'NO'))
        if not found:
            bad.append("the 'PPC ' fragment does not carry the tag %r: a stale PEF" % want_tag)
        dev = b'DEV BUILD' in ppc
        print('  PPC 128 is a %s build' % ('DEV (status row painted)' if dev else 'ship (no status row)'))
        if want_rel is not None and dev:
            bad.append("a release package holds a DEV build's code (it paints the status row)")
    if bad:
        for b in bad:
            print('  FAIL: ' + b)
        return 1
    print('  all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
