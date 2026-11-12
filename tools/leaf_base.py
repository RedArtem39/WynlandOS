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
import re
import os
import subprocess
import sys

out_path, manifests = sys.argv[1], sys.argv[2:]
pkgs = {}
provides = {}      # their Provides fields: leaf meets virtual dependencies with them

# what the image really has: its paths, and the names of its libraries
# (they land in /lib64, /lib, /usr/lib/... -- matched by name)
shipped, shipped_names = set(), set()
for m in manifests:
    for line in open(m):
        f = line.split()
        if len(f) >= 2 and f[0] in ("F", "Fo", "S"):
            shipped.add(f[1])
            shipped_names.add(os.path.basename(f[1]))

def ships(deb):
    """a cached .deb counts only if its libraries are in the image: the
    caches also hold packages a stage downloaded and did not ship (mpv's
    libcdio19t64 was listed as the system's while libcdio.so.19 was not
    there, so leaf never installed it)"""
    r = subprocess.run(["dpkg-deb", "-c", deb], capture_output=True, text=True)
    libs, other = [], []
    for line in r.stdout.splitlines():
        p = line.split()[-1] if line and line[0] in "-l" else ""
        if " -> " in line:
            p = line.split(" -> ")[0].split()[-1]
        p = p.lstrip(".")
        # (what the image never needs: docs, and systemd's units -- pipewire
        # itself is only those, its programs are pipewire-bin's)
        if not p or p.endswith("/") or p.startswith(("/usr/share/doc", "/usr/share/man", "/usr/share/lintian", "/usr/share/bug", "/usr/share/info",
                                                     "/usr/lib/systemd", "/lib/systemd", "/usr/lib/sysusers.d", "/usr/lib/tmpfiles.d", "/usr/share/apport")):
            continue
        (libs if re.search(r"\.so(\.|$)", os.path.basename(p)) else other).append(p)
    if not libs and not other:
        return True                     # (a metapackage, nothing of its own)
    return (any(os.path.basename(p) in shipped_names for p in libs) or
            any(p in shipped or p.replace("/usr/bin/", "/bin/") in shipped for p in other))

for deb in glob.glob(os.path.expanduser("~/.cache/wynland-*/debs/*.deb")):
    r = subprocess.run(["dpkg-deb", "-f", deb, "Package", "Version", "Provides"], capture_output=True, text=True)
    f = dict(l.split(": ", 1) for l in r.stdout.splitlines() if ": " in l)
    if "Package" in f and ships(deb):
        pkgs[f["Package"]] = f.get("Version", "0")
        if f.get("Provides"):
            provides[f["Package"]] = f["Provides"]

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
        q = subprocess.run(["dpkg-query", "-W", "-f", "${Package} ${Version}\t${Provides}\n", *sorted(names)],
                           capture_output=True, text=True)
        for line in q.stdout.splitlines():
            nv, _, prov = line.partition("\t")
            n, v = nv.split()
            if n not in pkgs:
                pkgs[n] = v
                if prov.strip():
                    provides[n] = prov.strip()

# Ubuntu packages the image stands in for with its own files: base-files
# is the system's identity (/etc/os-release, /etc/issue, ...) -- WynlandOS's.
# Installed as a dependency (bash pre-depends on it) it made the system
# say "ubuntu". Counted as installed, at the host's version (the same
# release), so no dependency asks for it again.
# The same for what the system does its own way: accounts (apps/accounts;
# passwd's and base-passwd's scripts rewrite /etc/passwd and /etc/shadow),
# dpkg and the systemd helpers (leaf's shims, which these would overwrite),
# and systemd itself, which is not the init here (evince pulled in all of
# them through gnupg -> dirmngr -> adduser).
OWN = ["base-files", "base-passwd", "passwd", "adduser", "login.defs", "dpkg",
       "init-system-helpers", "systemd", "systemd-sysv"]
q = subprocess.run(["dpkg-query", "-W", "-f", "${Package} ${Version}\n", *OWN], capture_output=True, text=True)
for line in q.stdout.splitlines():
    n, v = line.split()
    pkgs[n] = v

with open(out_path, "w") as f:
    for n in sorted(pkgs):
        f.write(f"{n} {pkgs[n]}" + (f"\t{provides[n]}" if n in provides else "") + "\n")
print(f"  LEAF       {len(pkgs)} packages come with the image")
