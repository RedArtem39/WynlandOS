# WynlandOS

> A custom x86_64 operating system built entirely from scratch — its own
> UEFI bootloader, kernel ("**Canopy Kernel**"), and a real multi-process
> GUI compositor ("**Zerp**"). No Linux kernel underneath, no libc
> borrowed wholesale — a from-scratch OS that runs real, unmodified
> userspace software (curl, nano, CMake, pkg-config...) via its own
> Linux-numbered syscall ABI and a musl cross-toolchain.

## Status

This is well past "hello world kernel" territory: real per-process address
space isolation, a real multi-process compositor, real networking with a
real TLS stack, and a growing set of genuinely working ported userspace
software — verified end-to-end in QEMU each time (serial logs *and*
screenshots, not just "it compiles").

### Working

- **Kernel core**: UEFI boot → own bootloader (no GRUB), GDT/IDT/IRQ,
  physical + virtual memory management, kernel heap.
- **Real per-process isolation**: every process gets its own page table
  (not a shared address space), its own fd table, a real UID model with
  password-gated elevation (`sudo`-equivalent) enforced against FAT32's
  readonly attribute.
- **Real process primitives**: `fork()` (eager full address-space copy),
  `execve()` with real argv/envp, `dup`/`dup2`, pipes, a real PTY
  subsystem (master/slave, termios, window size) — the same building
  blocks a real terminal emulator needs.
- **Real wall-clock time**: the CMOS RTC is read at boot for a genuine
  Unix epoch, and the system's timezone is auto-detected via IP
  geolocation at boot (no location ever hardcoded in source) and exposed
  to every process via `TZ`.
- **Zerp — a real multi-process tiling compositor**, itself an ordinary
  Ring-3 process, not kernel code:
  - Every client (file manager, terminal, Qt6 apps) is a genuinely
    separate OS process, spawned dynamically at runtime, tiled with a
    simple dwindle-style layout.
  - Zero-copy shared-memory pixel transport + small pipe messages for
    control (damage rects, input routing, spawn/close).
  - A real terminal (`zerp_term.elf`) that can launch and drive a truly
    interactive program through a real PTY — `nano <file>` runs genuine
    GNU nano, with a from-scratch VT100 interpreter rendering its actual
    screen output into the terminal's tile.
  - A real from-scratch PNG decoder (`zerp_png.h`, real inflate/Paeth
    filtering) — `cat image.png` renders it inline.
- **Qt6, ported and running as a real Zerp client**: a custom QPA
  platform plugin backs Qt onto Zerp's own shared-memory/pipe protocol
  (not X11, not Wayland), with real FreeType-rendered text (not
  placeholder glyph boxes) and real keyboard/mouse input routing.
- **Real networking**: a real virtio-net driver up through a real TCP/IP
  stack (ARP, ICMP, UDP, DHCP, DNS, TCP with retransmission) — wired all
  the way to userspace `socket()`/`connect()`/`read()`/`write()`, so
  ordinary programs' own networking code works unmodified.
- **A real, unmodified `curl` runs on WynlandOS** — real DNS resolution,
  a real TCP handshake, a real TLS 1.3 handshake (LibreSSL), and a real
  `200 OK` HTTP response from an actual server on the internet.
- **A real userspace dev toolchain, ported and working**: `pkg-config`
  (pkgconf), a full **CMake** (which also runs *natively* on WynlandOS,
  not just cross-compiling for it), real runtime `dlopen()`/`dlsym()`,
  `zlib`, `LibreSSL` (real SHA256/AES via EVP).
- **Wynlang** — a small scripting language with its own VM
  (`kernel/wynlang.c`, `kernel/wynvm.cpp`).

### In progress / dormant

- **Upstream Hyprland** (`gui/HAPRYLAND/`) is still auto-launched at boot
  alongside Zerp but still crashes on startup (a pre-existing, contained
  crash, not a regression) — this parallel effort is dormant while Zerp
  is the actively-developed compositor.
- **`git`** is next on the porting list, followed by a package manager,
  then a from-scratch minimal browser engine (not a Chromium/Firefox
  port — see the technical plan for why), then `zsh`/`bash` last.

## Build Requirements

| Tool | Purpose |
|------|---------|
| `gcc` | Kernel C compiler |
| `x86_64-w64-mingw32-gcc` | UEFI bootloader compiler |
| `nasm` | x86_64 assembler |
| `mtools` | FAT32 image creation |
| `qemu-system-x86_64` | Emulator |
| `ovmf` | UEFI firmware for QEMU |

Userspace ports (curl, CMake, LibreSSL, nano, ...) are built separately
against a musl cross-toolchain — see the technical plan file for exact
build commands per port.

## Quick Start

### 1. Setup (Ubuntu / WSL2)

```bash
chmod +x setup.sh
./setup.sh
```

### 2. Build

```bash
make
```

### 3. Run

```bash
make run
```

### 4. Debug

```bash
make debug
# In another terminal:
gdb -ex "target remote localhost:1234" build/kernel.elf
```

## Project Structure

```
WynlandOs/
├── boot/               # UEFI bootloader
├── kernel/             # Canopy Kernel: mm, sched, syscalls, ELF loader,
│                       #   real RTC/timezone (rtc.c), per-process
│                       #   isolation (process.c), Wynlang VM
├── drivers/            # Device drivers
│   ├── video/          #   Framebuffer / virtio-gpu
│   ├── input/          #   Keyboard, mouse
│   ├── net/            #   virtio-net, ARP/ICMP/UDP/DHCP/DNS/TCP
│   ├── fs/              #   FAT32 (long filenames)
│   └── pci/             #   PCI bus
├── include/wynland/    # Shared kernel headers
├── zerp*.c/.h          # Zerp: the real multi-process compositor, its
│                       #   client library, and demo/regression clients
├── qt_qpa/             # Qt6 platform plugin -- a real Zerp client, not
│                       #   a raw-framebuffer app
├── gui/                # Legacy Ring-0 desktop env + dormant Hyprland
│                       #   port (see Status above)
├── lib/                # Shared kernel library
├── pkg/                # wynpkg package manager (+ vendored quickjs)
├── test_*.c            # Permanent regression binaries (fork, TCP, DNS,
│                       #   dlopen, RTC/timezone, ...)
├── external_src/       # Downloaded upstream source for ported userspace
│                       #   tools (curl, LibreSSL, zlib, CMake, nano,
│                       #   ncurses, pkgconf) -- gitignored, not vendored
├── tools/               # musl cross-toolchain wrappers, OVMF firmware
├── Makefile             # Build system
├── setup.sh             # Toolchain setup
└── run.sh               # QEMU launcher
```

## Architecture

```
┌───────────────────────────────────────────────────────┐
│  Real Ring-3 userspace: curl, nano, CMake, pkg-config,  │
│  Zerp clients (file manager, terminal, Qt6 apps)         │
├───────────────────────────────────────────────────────┤
│         Zerp -- real multi-process compositor            │
│  ┌───────────┬────────────┬──────────────────────────┐  │
│  │ Dwindle   │ Zero-copy  │ Real PTY-backed terminal   │  │
│  │ tiling    │ SHM + pipe │ (fork/dup2/execve, VT100)  │  │
│  └───────────┴────────────┴──────────────────────────┘  │
├───────────────────────────────────────────────────────┤
│    Ring 3 / Linux-numbered syscalls (real ELF64 ABI)     │
├───────────────────────────────────────────────────────┤
│                    Canopy Kernel                          │
│  ┌───────────┬────────────┬──────────────────────────┐  │
│  │ Per-proc  │ Real TCP/  │ VFS / FAT32               │  │
│  │ isolation │ IP + DNS   │ + real RTC/timezone        │  │
│  │ + fork()  │            │                            │  │
│  └───────────┴────────────┴──────────────────────────┘  │
│  ┌───────────┬────────────┬──────────────────────────┐  │
│  │ PCI       │ virtio-net │ virtio-gpu / Input Drivers │  │
│  │ Driver    │            │                            │  │
│  └───────────┴────────────┴──────────────────────────┘  │
├───────────────────────────────────────────────────────┤
│                 UEFI Bootloader                            │
├───────────────────────────────────────────────────────┤
│                  Hardware (x86_64)                          │
└───────────────────────────────────────────────────────┘
```

## License

This project is for educational purposes.
