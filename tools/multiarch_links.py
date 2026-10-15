#!/usr/bin/env python3
# WynlandOS - the image's libraries are in /lib64 (by soname); Ubuntu's
# own place for them is /usr/lib/x86_64-linux-gnu, and what was built on
# Ubuntu looks there by path: the linker scripts of libc6-dev (libc.so names
# /usr/lib/x86_64-linux-gnu/libc.so.6), gcc's libstdc++.so link. So each
# library in /lib64 gets a link there, unless a file of that name is there.
#
# usage: multiarch_links.py MANIFEST...   (appends S lines to stdout)
import sys

have, libs, dirs = set(), [], set()
for path in sys.argv[1:]:
    for line in open(path):
        p = line.split()
        if len(p) < 2:
            continue
        if p[0] == "D":
            dirs.add(p[1])
        elif p[0] in ("F", "Fo", "S"):
            have.add(p[1])
            if p[1].startswith("/lib64/") and p[1].count("/") == 2:
                libs.append(p[1][len("/lib64/"):])

out = []
for d in ("/usr/lib", "/usr/lib/x86_64-linux-gnu"):
    if d not in dirs:
        out.append("D " + d)
for name in sorted(set(libs)):
    dest = "/usr/lib/x86_64-linux-gnu/" + name
    if dest not in have:
        out.append("S %s /lib64/%s" % (dest, name))
print("\n".join(out))
