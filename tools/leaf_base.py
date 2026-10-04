#!/usr/bin/env python3
"""WynlandOS - /var/lib/leaf/base: the packages the system image comes with.

leaf installs Ubuntu packages; the ones whose files the image already has
(staged by tools/stage_*.sh from .deb caches, or copied from the build
host's own libraries) must count as installed, or every install would
drag in a second libc. This lists them as "name version":
  - every .deb in the staging caches (~/.cache/wynland-*/debs),
  - the host package of every host file a manifest ships (dpkg -S).

usage: tools/leaf_base.py OUT MANIFEST...
"""
import glob
import os
import subprocess
import sys

out_path, manifests = sys.argv[1], sys.argv[2:]
pkgs = {}

for deb in glob.glob(os.path.expanduser("~/.cache/wynland-*/debs/*.deb")):
    r = subprocess.run(["dpkg-deb", "-f", deb, "Package", "Version"], capture_output=True, text=True)
    f = dict(l.split(": ", 1) for l in r.stdout.splitlines() if ": " in l)
    if "Package" in f:
        pkgs[f["Package"]] = f.get("Version", "0")

host_files = set()
for m in manifests:
    for line in open(m):
        f = line.split()
        if len(f) >= 3 and f[0] in ("F", "Fo") and f[2].startswith("/usr/"):
            host_files.add(f[2])
            # a library is named by its soname symlink in dpkg's database
            host_files.add(os.path.realpath(f[2]))

if host_files:
    r = subprocess.run(["dpkg", "-S", *sorted(host_files)], capture_output=True, text=True)
    names = set()
    for line in r.stdout.splitlines():
        owner = line.split(": ", 1)[0]
        for n in owner.split(", "):
            names.add(n.split(":")[0])
    if names:
        q = subprocess.run(["dpkg-query", "-W", "-f", "${Package} ${Version}\n", *sorted(names)],
                           capture_output=True, text=True)
        for line in q.stdout.splitlines():
            n, v = line.split()
            pkgs.setdefault(n, v)

with open(out_path, "w") as f:
    for n in sorted(pkgs):
        f.write(f"{n} {pkgs[n]}\n")
print(f"  LEAF       {len(pkgs)} packages come with the image")
