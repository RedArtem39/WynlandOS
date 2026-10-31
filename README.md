<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/brand/logo-dark.png">
    <img src="docs/brand/logo.png" alt="WynlandOS" width="640">
  </picture>
</p>

<p align="center">
  <b>An x86_64 operating system written from scratch</b> — its own UEFI bootloader,
  its own kernel, its own init and its own GPU desktop — that runs real Linux
  software unmodified: WebKit, GStreamer, PipeWire, Qt 6, Python, Node.js, fish.
</p>

---

WynlandOS is not a Linux distribution. There is no Linux kernel underneath:
the **Canopy kernel** is our own, and it speaks the Linux x86_64 system call
ABI well enough that prebuilt programs from Ubuntu and Debian — glibc and
all — load and run on it as they are. It is developed in QEMU, where
automatic tests boot the OS and drive the real programs, and it boots on one
real laptop (an Intel Tiger Lake machine) from a USB stick.

## What it does today

**Desktop — Zerp 2.0.** Our Wayland compositor, written in Qt Quick and drawn
on the GPU: virtio-gpu + virgl in QEMU, the Intel GPU on the laptop. Like
Hyprland it draws only windows — panels, launchers and notices are the
shell's: tiling, or an infinite canvas per workspace (Alt+G) with windows
dragged with Alt and thrown on springs, the view panned and zoomed; windows
that pop in and out on Hyprland's curve, real rounded corners, shadows and
a turning gradient border on the focused one.
xdg-shell, layer-shell, linux-dmabuf and global shortcuts; X11 programs
through Xwayland (`xrun xterm`). It answers Hyprland's IPC, so shells made
for Hyprland run on it.

**Shell — xvlet.** A Material 3 Expressive shell (its own project,
[xvlet-shell](https://github.com/RedArtem39/xvlet-shell)): bar, dock,
launcher, dashboard, quick settings, notifications, wallpapers with dynamic
colours, desktop widgets, a lock screen, sounds. The Caelestia shell runs too.

**Login.** wynlogin, the login manager: real accounts (`/etc/passwd`,
yescrypt hashes), the first account made on the first boot, skins
(`theme = NAME` in `/etc/wynland/login.conf`). `ary` makes administrators
(the `wheel` group) root with their own password; lock screens check theirs
with `chkpwd`.

**Web.** A browser on WPE WebKit 2.54 (JavaScriptCore with its JIT) in a Qt
Quick shell: vertical tabs, HTTPS, YouTube with video (H.264 through
GStreamer).

**Sound.** PipeWire 1.6 with WirePlumber and pipewire-pulse on our Intel HD
Audio driver; GStreamer 1.28 with Ubuntu's plugins; Qt Multimedia.

**Tools.** fish 4.2 by default, dash as `/bin/sh`, GNU coreutils and the
usual tools, Python 3.14 with pip and venv, Node.js 22 with npm, clang and
make, CMake, curl, nano, git. **leaf** installs more from Ubuntu's archive,
signatures and checksums checked: `leaf install htop`.

## The kernel

- UEFI boot through our own loader; a higher-half kernel; SMP on every core
  with an EEVDF scheduler (much of the kernel still under one big lock).
- Clocks: HPET/CPUID-calibrated TSC, the APIC timer as the tick (machines
  without a PIT), each core's TSC offset corrected.
- Per-process address spaces with demand paging, copy-on-write `fork()`,
  shared and file-backed `mmap`, a page cache, swap, W^X with an opt-in for
  JITs; up to 1024 open files a process.
- Threads, futexes (priority inheritance too), POSIX signals, process groups
  and sessions, PTYs with a real line discipline, `/proc` per process.
- ext2 with a write-back block cache, permissions, links and the `*at()`
  calls.
- Sandboxing as browsers build theirs: seccomp-bpf, no_new_privs,
  capabilities, chroot, user, pid, network and mount namespaces — Ubuntu's
  bubblewrap runs on it.
- Networking: IPv4, TCP, UDP, DHCP, DNS; Unix sockets with fd passing;
  `poll`, `select`, `epoll`, `eventfd`, `timerfd`, `memfd`.
- Loadable kernel modules (`insmod`, `rmmod`, `lsmod`).
- **wynrc**, an OpenRC-style init: declarative services, parallel start,
  supervision.

## Hardware

| | QEMU | the laptop (ASUS Vivobook 15, i3-1115G4) |
|---|---|---|
| boot | OVMF, disk image | USB stick, the whole system run from RAM |
| GPU | virtio-gpu, virgl (OpenGL) | Intel Gen12: an i915-compatible driver, Mesa iris (OpenGL ES 3.2) and anv (Vulkan); or the firmware framebuffer |
| storage | AHCI, NVMe | none needed (its NVMe sits behind Intel VMD: no driver) |
| USB | xHCI: keyboard, mouse, stick | xHCI: keyboard, mouse (no wheel yet), stick |
| network | virtio-net | Android phone tethering (RNDIS) |
| sound | Intel HD Audio | not checked yet |
| power | ACPI power-off and reset | ACPI power-off and reset |

Not there: Wi-Fi, Bluetooth (a stand-in on D-Bus says "no adapters"),
Ethernet cards, touchpads, sleep, battery status, IPv6, AMD and NVIDIA GPUs.
Nothing has been tried on other real machines.

## Build and run

Ubuntu 26.04 (native or WSL2) with KVM:

```bash
./setup.sh          # toolchain, QEMU, OVMF
make                # kernel, bootloader, the disk image with everything staged
make run-gl         # boot it in QEMU with virgl 3D
make live           # build/wynland-live.img: a USB stick image (dd, or Rufus in DD mode)
```

The userland (Qt, WebKit, GStreamer, PipeWire, Python, fish, coreutils) comes
from Ubuntu's and Debian's packages, fetched and staged into the image by
`tools/stage_*.sh`; `WITH_QT6=0`, `WITH_GST=0`, `WITH_WPE=0`, `WITH_BASE=0`
leave parts out. The xvlet shell is staged from `../xvlet-shell` when it is
checked out there. Pictures put in `rootfs/usr/share/wynland/wallpapers`
(beyond the ones listed in `ext2_manifest.txt`) become the image's only
wallpapers.

### Tests

`make run-gl AUTOTEST=1` boots into the test programs (`tests/`) instead of
the desktop: process lifecycle (`forktest`), the shell, tools, Python, Node,
compilers, accounts and packages (`shtest`), sandboxing (`sandboxtest`),
OpenGL on the GPU (`gltest`), KMS (`kmstest`), sound and PipeWire (`sndtest`,
`pwtest`, `pitest`), the JIT memory policy (`jittest`), GStreamer (`gsttest`)
and WebKit pages including YouTube (`wpetest`). Changes go in after they pass
on 4 cores and on 1.

## Layout

```
boot/          UEFI bootloader (and the live stick's reader)
kernel/        the Canopy kernel: memory, scheduler, syscalls, signals,
               ELF loader, PTYs, Unix sockets, futexes, procfs, swap, modules
drivers/       AHCI, NVMe, ext2; xHCI and USB (HID, storage, RNDIS);
               virtio-net and TCP/IP; virtio-gpu and Intel i915; HD Audio;
               ACPI, PCI, PS/2 input
apps/zerp2/    Zerp 2.0, the compositor (and the built-in login screen)
apps/accounts/ wynlogin, chkpwd, useradd/userdel/passwd
apps/ary/      ary, root for administrators
apps/web/      the browser (WPE WebKit in Qt Quick)
apps/files/    the file manager
apps/term/     the terminal
apps/wynrc/    the init system
apps/wynvt/    the text console
apps/leaf/     the package tool
qt_qpa/        Qt's platform plugin for Zerp
tests/         the boot-time test programs
tools/         staging of the userland, image building, hardware tools
rootfs/        files copied into the image (services, configs, wallpapers)
```

## Documentation

- [docs/syscalls.md](docs/syscalls.md): the system calls the kernel
  implements, Linux's and our own.
- [CONTRIBUTING.md](CONTRIBUTING.md): how to contribute, and the terms.

## Next

The mouse wheel on real USB mice, a Chromium-based browser, more of the
kernel out of the big lock, and more real hardware: Wi-Fi, the touchpad,
sleep and battery.

<p align="center">
  <img src="docs/brand/mascot.png" alt="the WynlandOS snail" width="200">
</p>

## License

Copyright (C) 2026 Red_Artem39

WynlandOS is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 2 of the License, or (at your option) any later
version (GPL-2.0-or-later). It is distributed WITHOUT ANY WARRANTY; see
[LICENSE](LICENSE) for the full text.

Code taken from other projects keeps its own license, kept beside it and in
[.licenses](.licenses).

The stock wallpapers are photos from Unsplash, credited in
`rootfs/usr/share/wynland/wallpapers/CREDITS`.

Third-party software staged into the disk image (Qt, WebKit, GStreamer,
PipeWire, Python, fish, GNU coreutils and the rest) is not part of this
repository and keeps its own licenses. The xvlet shell is a separate project
under the AGPL-3.0.
