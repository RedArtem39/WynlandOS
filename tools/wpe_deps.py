#!/usr/bin/env python3
"""WynlandOS - fetch WPE WebKit and the libraries it needs.

WPE WebKit is not in Ubuntu 26.04; Debian sid has it (same glibc, 2.43).
The WPE packages come from sid; every library they need that the host
does not have is looked up by soname in sid's Contents index and fetched:
from Ubuntu when Ubuntu has a package of that name that provides it (the
rest of the image is Ubuntu's, so its ABI matches), else from sid. Repeat
until ldd finds everything and no symbol version is missing.

usage: tools/wpe_deps.py <cache dir> <extra ld path>...   (inside WSL)
Unpacks into <cache>/root; prints what it fetched.
"""
import gzip, os, re, subprocess, sys, urllib.request

MIRROR = "https://deb.debian.org/debian"
SID = MIRROR + "/dists/sid/main"
TOP = ["libwpewebkit-2.0-1", "libwpewebkit-2.0-dev", "libwpe-1.0-1", "libwpe-1.0-dev"]
MULTIARCH = "usr/lib/x86_64-linux-gnu"

cache = sys.argv[1]
extra_paths = sys.argv[2:]
root = os.path.join(cache, "root")
debs = os.path.join(cache, "debs")
os.makedirs(root, exist_ok=True)
os.makedirs(debs, exist_ok=True)


def fetch(url, path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        print(f"  WPE        fetch {url.rsplit('/', 1)[-1]}", flush=True)
        tmp = path + ".part"
        with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
        os.rename(tmp, path)


# sid indexes
pk_path = os.path.join(cache, "Packages")
if not os.path.exists(pk_path):
    xz = pk_path + ".xz"
    fetch(SID + "/binary-amd64/Packages.xz", xz)
    subprocess.run(["xz", "-dk", xz], check=True)
packages, depends = {}, {}
for para in open(pk_path, encoding="utf-8", errors="replace").read().split("\n\n"):
    m = re.search(r"^Package: (\S+)", para, re.M)
    f = re.search(r"^Filename: (\S+)", para, re.M)
    if m and f:
        packages[m.group(1)] = f.group(1)
        depends[m.group(1)] = set()
        for d in re.findall(r"^(?:Pre-)?Depends: (.*)$", para, re.M):
            depends[m.group(1)] |= set(re.findall(r"(?:^|[,|]\s*)([a-z0-9][a-z0-9.+-]+)", d))

ct_path = os.path.join(cache, "Contents-amd64.gz")
fetch(SID + "/Contents-amd64.gz", ct_path)
soname_pkgs = {}
with gzip.open(ct_path, "rt", encoding="utf-8", errors="replace") as f:
    for line in f:
        if not line.startswith(MULTIARCH + "/lib") and not line.startswith("lib/x86_64-linux-gnu/lib"):
            continue
        path, _, locs = line.rstrip("\n").rpartition(" ")
        name = os.path.basename(path.strip())
        for loc in locs.split(","):
            soname_pkgs.setdefault(name, []).append(loc.rsplit("/", 1)[-1])

# the names sid's own packages ask for (libjpeg62-turbo, not libjpeg62)
wanted = set(TOP)
for _ in range(3):
    for p in list(wanted):
        wanted |= depends.get(p, set())


def soname_pkg(so):
    cands = soname_pkgs.get(so, [])
    for c in cands:
        if c in wanted:
            return c
    return cands[0] if cands else None

fetched = set(os.listdir(debs))


def have_deb(pkg):
    return any(d.startswith(pkg + "_") for d in fetched)


def unpack(path):
    subprocess.run(["dpkg-deb", "-x", path, root], check=True)


def get_sid(pkg):
    fn = packages[pkg]
    path = os.path.join(debs, os.path.basename(fn))
    fetch(f"{MIRROR}/{fn}", path)
    fetched.add(os.path.basename(path))
    unpack(path)
    print(f"  WPE        {pkg} (sid)", flush=True)


def get_ubuntu(pkg, soname):
    """Ubuntu's package of that name, if it has the soname."""
    r = subprocess.run(["apt-get", "download", pkg], cwd=debs, capture_output=True, text=True)
    if r.returncode != 0:
        return False
    new = [d for d in os.listdir(debs) if d.startswith(pkg + "_") and d not in fetched]
    if not new:
        return False
    path = os.path.join(debs, new[0])
    files = subprocess.run(["dpkg-deb", "-c", path], capture_output=True, text=True).stdout
    if "/" + soname not in files:
        os.remove(path)
        return False
    fetched.add(new[0])
    unpack(path)
    print(f"  WPE        {pkg} (ubuntu)", flush=True)
    return True


# what earlier runs fetched, then the WPE packages themselves
for d in sorted(fetched):
    if d.endswith(".deb"):
        unpack(os.path.join(debs, d))
for p in TOP:
    if not have_deb(p):
        get_sid(p)


def elfs():
    out = []
    for base in (os.path.join(root, MULTIARCH), os.path.join(root, "lib/x86_64-linux-gnu")):
        for dp, _, fs in os.walk(base):
            for fn in fs:
                p = os.path.join(dp, fn)
                if os.path.islink(p) or not (".so" in fn or os.access(p, os.X_OK)):
                    continue
                with open(p, "rb") as f:
                    if f.read(4) == b"\x7fELF":
                        out.append(p)
    return out


def ld_path():
    return ":".join([os.path.join(root, MULTIARCH), os.path.join(root, "lib/x86_64-linux-gnu")] + extra_paths)


sid_forced = set()
for rnd in range(30):
    missing, version_missing = set(), set()
    env = dict(os.environ, LD_LIBRARY_PATH=ld_path())
    for e in elfs():
        r = subprocess.run(["ldd", e], capture_output=True, text=True, env=env)
        for line in (r.stdout + r.stderr).splitlines():
            m = re.match(r"\s*(\S+) => not found", line)
            if m:
                missing.add(m.group(1))
            m = re.search(r": (\S+): version `[^']+' not found", line)
            if m:
                version_missing.add(os.path.basename(m.group(1)))
    if not missing and not version_missing:
        break
    progress = False
    for so in sorted(missing):
        pkg = soname_pkg(so)
        if not pkg:
            print(f"  WPE        WARNING: no package has {so}")
            continue
        if have_deb(pkg):
            continue
        if not get_ubuntu(pkg, so):
            get_sid(pkg)
        progress = True
    # a library too old for what sid built against: take sid's
    for so in sorted(version_missing):
        pkg = soname_pkg(so)
        if pkg and pkg not in sid_forced and pkg in packages:
            sid_forced.add(pkg)
            get_sid(pkg)
            progress = True
    if not progress:
        print("  WPE        WARNING: unresolved:", sorted(missing), sorted(version_missing))
        break
# symbols nothing provides (an older ABI than WPE was built for)
env = dict(os.environ, LD_LIBRARY_PATH=ld_path())
undef = set()
for e in elfs():
    r = subprocess.run(["ldd", "-r", e], capture_output=True, text=True, env=env)
    for line in (r.stdout + r.stderr).splitlines():
        if "undefined symbol" in line and "libWPEInjectedBundle" not in e:
            undef.add(line.strip()[:160])
for u in sorted(undef)[:30]:
    print("  WPE        undefined:", u)
print("  WPE        dependencies resolved")
