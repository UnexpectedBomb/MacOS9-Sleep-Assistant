#!/usr/bin/env python3
"""scrub-build-user.py <built.bin> <published.bin> -- copy a built Sleep Assistant for publication, with the
build machine's account name taken out of it.

WHY: Retro68's prebuilt newlib carries __FILE__ strings from its own sources, such as
    /Users/<builduser>/Retro68/gcc/newlib/libc/stdlib/dtoa.c
and they are linked into the PowerPC code. They are inert (libc error paths this code never calls), but on a
project published under a handle they put the builder's account name into a downloadable file.

HOW: the name is replaced by a neutral word of EXACTLY the same length, so nothing in the file moves: MacBinary II
checksums only its 128-byte header, and neither the resource fork nor the PEF container carries a checksum. The
account name is taken from the system (or SCRUB_TOKEN), never written down here. The output is verified: the same
size, the same header, the name gone, and no other byte changed. It writes a new file and never touches the input.
"""
import getpass
import os
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)
src, dst = sys.argv[1], sys.argv[2]
user = os.environ.get('SCRUB_TOKEN') or getpass.getuser()
if len(user) < 3:
    sys.exit('scrub-build-user: implausible account name %r, refusing' % user)
neutral = ('builduser' * 8)[:len(user)]
token = ('/Users/%s/' % user).encode('ascii')
repl = ('/Users/%s/' % neutral).encode('ascii')

data = open(src, 'rb').read()
n = data.count(token)
out = data.replace(token, repl)
changed = sum(1 for a, b in zip(data, out) if a != b)
if len(out) != len(data) or out[:128] != data[:128] or token in out or changed != n * len(user):
    sys.exit('scrub-build-user: verification failed (size %d -> %d, %d occurrence(s), %d byte(s) changed)'
             % (len(data), len(out), n, changed))
tmp = dst + '.tmp'
with open(tmp, 'wb') as f:
    f.write(out)
os.replace(tmp, dst)
print('scrub-build-user: %s -> %s: %d occurrence(s) of the account name, %d byte(s) changed, size %d unchanged'
      % (src, dst, n, changed, len(out)))
