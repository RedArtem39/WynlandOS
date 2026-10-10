#!/usr/bin/env python3
"""
WynlandOS - ext2 root-partition image builder (host-side, root-free).

Builds a plain-file ext2 filesystem image and populates it from a
manifest, exactly mirroring mtools' no-sudo convention so `make image`
keeps working for whoever runs it:

    1. mke2fs -q -F -t ext2 -b 4096 <out>          (plain file target)
       Confirmed via dumpe2fs that this alone produces precisely
       {filetype, sparse_super, large_file} -- exactly the feature set
       drivers/fs/ext2.c's whitelist accepts. No -O tuning needed.
    2. debugfs -w -f <script>                       (mkdir / write cmds)
    3. verification pass: `stat` every attempted destination and fail
       the build loudly if any required file is missing. This exists
       because `debugfs write` to a destination whose parent directory
       doesn't exist fails COMPLETELY SILENTLY (no stdout/stderr at
       all) -- caught for real in Phase 20e when /usr/bin and /etc/ssl
       were missing from the manifest and five binaries silently never
       landed.

Manifest format (one entry per line, '#' starts a comment):
    D  /dir/path              create directory (parents must be listed too)
    F  /dest/path host/src    copy host file to image path (REQUIRED --
                              missing source or failed write = build error)
    Fo /dest/path host/src    optional variant: skip silently if the host
                              source doesn't exist
    S  /dest/path target      symbolic link (e.g. S /bin usr/bin)

Usage: python3 build_ext2_image.py <out.img> <size_mb> <manifest.txt>
"""

import os
import subprocess
import sys


def run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def main() -> None:
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)

    out_img = os.path.abspath(sys.argv[1])
    size_mb = int(sys.argv[2])
    manifest_path = sys.argv[3]

    dirs: list[str] = []
    required: list[tuple[str, str]] = []
    optional: list[tuple[str, str]] = []

    perms: list[tuple[str, int, int]] = []
    links: list[tuple[str, str]] = []
    nodes: list[tuple[str, str, int, int]] = []
    with open(manifest_path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split(None, 2)
            kind = parts[0]
            if kind == "D" and len(parts) == 2:
                dirs.append(parts[1])
            elif kind == "P" and len(parts) == 3:
                # P <path> <uid> <octal mode incl. type>, e.g. P /tmp 0 41777
                uid, mode = parts[2].split()
                perms.append((parts[1], int(uid), int(mode, 8)))
            elif kind == "S" and len(parts) == 3:
                links.append((parts[1], parts[2]))
            elif kind == "N" and len(parts) == 3:
                # N /dev/null c 1 3 -- a device node (its number in the inode)
                t, major, minor = parts[2].split()
                nodes.append((parts[1], t, int(major), int(minor)))
            elif kind in ("F", "Fo") and len(parts) == 3:
                (required if kind == "F" else optional).append((parts[1], parts[2]))
            else:
                print(f"ERROR: bad manifest line: {raw.rstrip()}", file=sys.stderr)
                sys.exit(1)

    # 1. fresh filesystem on a plain file
    if os.path.exists(out_img):
        os.unlink(out_img)
    with open(out_img, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)
    print(f"  EXT2       mke2fs {out_img} ({size_mb} MB)")
    run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096", out_img])

    # 2. populate via debugfs script (mkdir first, then writes)
    cmds = [f"mkdir {d}" for d in dirs]
    skipped = 0

    def add_writes(pairs: list[tuple[str, str]], is_required: bool) -> None:
        nonlocal skipped
        for dest, src in pairs:
            if not os.path.exists(src):
                if is_required:
                    print(f"ERROR: required manifest source not found: {src}",
                          file=sys.stderr)
                    sys.exit(1)
                skipped += 1
                continue
            cmds.append(f"write {os.path.abspath(src)} {dest}")

    add_writes(required, True)
    add_writes(optional, False)
    for dest, target in links:
        cmds.append(f"symlink {dest} {target}")
    for dest, t, major, minor in nodes:
        # debugfs makes the node in its working directory, named by the last part
        parent, name = os.path.split(dest)
        cmds += [f"cd {parent or '/'}", f"mknod {name} {t} {major} {minor}", "cd /",
                 f"sif {dest} uid 0", f"sif {dest} gid 0", f"sif {dest} mode 0{0o20000 | 0o666:o}"]

    # Ownership and modes: everything root's, directories 0755, programs
    # (ELF or #! script) 0755, other files 0644 -- sources on a Windows
    # drive all read as 0777, which made every file in the image
    # world-writable. "P" lines then set the exceptions (/tmp, the user's
    # home).
    def is_elf(src: str) -> bool:
        try:
            with open(src, "rb") as h:
                head = h.read(4)
                return head == b"\x7fELF" or head[:2] == b"#!"
        except OSError:
            return False
    for d in dirs:
        cmds += [f"sif {d} uid 0", f"sif {d} gid 0", f"sif {d} mode 040755"]
    for dest, src in required + optional:
        if os.path.exists(src):
            mode = "0100755" if is_elf(src) else "0100644"
            cmds += [f"sif {dest} uid 0", f"sif {dest} gid 0", f"sif {dest} mode {mode}"]
    for path, uid, mode in perms:
        cmds += [f"sif {path} uid {uid}", f"sif {path} gid {uid}", f"sif {path} mode 0{mode:o}"]

    script_path = out_img + ".debugfs.cmds"
    with open(script_path, "w", encoding="utf-8") as f:
        f.write("\n".join(cmds) + "\n")
    run(["debugfs", "-w", "-f", script_path, out_img])
    os.unlink(script_path)

    # 3. loud post-write verification (silent-failure safety net)
    verify_script = out_img + ".verify.cmds"
    with open(verify_script, "w", encoding="utf-8") as f:
        for dest, _ in required + optional:
            f.write(f"stat {dest}\n")
    vproc = subprocess.run(["debugfs", "-f", verify_script, out_img],
                           text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
    os.unlink(verify_script)

    joined = vproc.stdout
    missing = [dest for dest, _ in required
               if "File not found" in _chunk_for(joined, dest)]

    if missing:
        print("\nERROR: required files missing from ext2 image after population:",
              file=sys.stderr)
        for d in missing:
            print(f"  {d}", file=sys.stderr)
        print("(usual cause: parent directories missing from the manifest's D list)",
              file=sys.stderr)
        sys.exit(1)

    n_written = len(cmds) - len(dirs)
    print(f"  EXT2       populated: {len(dirs)} dirs, {n_written} files written"
          + (f", {skipped} optional skipped" if skipped else ""))


def _chunk_for(joined: str, dest: str) -> str:
    """Extract debugfs output following a `stat <dest>` command."""
    marker = f"stat {dest}"
    start = joined.find(marker)
    if start == -1:
        return "File not found"
    return joined[start:start + 400]


if __name__ == "__main__":
    main()
