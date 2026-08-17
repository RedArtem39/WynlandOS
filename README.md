# WynlandOS

> A custom x86_64 operating system built entirely from scratch.

## Status

Booting, multitasking, and running real Ring 3 userspace applications with
a real GUI stack — this is well past "hello world kernel" territory.
Roughly, in order of how deep it goes:

- **Working**
  - UEFI boot → kernel handoff (own bootloader, no GRUB)
  - GDT/IDT/IRQ, physical + virtual memory management, kernel heap
  - Preemptive multitasking and multithreading (`SYS_clone`, TLS FS-base
    context switching)
  - Ring 3 execution: full ELF64 loader, Linux-numbered syscall
    dispatcher (`execve`, `mmap`/`munmap`, `pread64`, `sigaction`,
    `sigprocmask`, `getrandom`, `nanosleep`, `clock_gettime`, ...)
  - VFS with a FAT32 backend (long filenames included)
  - virtio-gpu, VirtIO 1.0-compliant, plus a software cursor and
    subpixel-AA font rendering (Segoe UI-style, variable width)
  - `wynpkg` package manager (custom `.wpkg` format) — already
    implemented, not just designed
  - **Qt6 is ported and running in Ring 3** — a custom QPA platform
    plugin (`qt_qpa/qwynlandfb*`) backs Qt's framebuffer directly, real
    Qt6 widgets render on real hardware/QEMU output, no X11/Wayland
    underneath. This is what the "macOS Tahoe style" shell (genie
    minimize animation, Control Center, titlebars) actually runs on.
  - Wynlang — a small scripting language with its own VM
    (`kernel/wynlang.c`, `kernel/wynvm.cpp`), `.wyn` scripts for stuff
    like `browser.wyn`, `hello.wyn`, `wynui.wyn`
- **In progress / broken**
  - **Porting upstream Hyprland is blocked on Aquamarine** (Hyprland's
    rendering/backend library): at runtime, Hyprland's build/startup does
    a pkg-config lookup for `aquamarine.pc` and the VFS lookup fails
    across every searched path (`/usr/lib/pkgconfig`,
    `/usr/local/lib/pkgconfig`, `/usr/lib64/pkgconfig` — see
    `serial_new.log`). Aquamarine itself isn't wired into WynlandOS's
    library/pkg-config search path yet, so Hyprland can't find its own
    dependency even though the headers are vendored under
    `include/aquamarine/`. There's also a from-scratch, WynlandOS-native
    WM under `gui/hyprland/` (config/desktop/layout/render/managers) as
    a parallel, non-upstream approach to the same tiling+floating idea.
  - Network stack: virtio-net driver exists under `drivers/net/`; full
    TCP/IP stack on top of it isn't done.

## Build Requirements

| Tool | Purpose |
|------|---------|
| `gcc` | Kernel C compiler |
| `x86_64-w64-mingw32-gcc` | UEFI bootloader compiler |
| `nasm` | x86_64 assembler |
| `mtools` | FAT32 image creation |
| `qemu-system-x86_64` | Emulator |
| `ovmf` | UEFI firmware for QEMU |

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
├── boot/              # UEFI bootloader
├── kernel/            # Kernel core (mm, sched, syscalls, ELF loader,
│                      #   Wynlang VM: wynlang.c / wynvm.cpp)
├── drivers/           # Device drivers
│   ├── video/         #   Framebuffer / virtio-gpu
│   ├── input/         #   Keyboard, mouse
│   ├── net/           #   virtio-net
│   ├── fs/            #   FAT32
│   └── pci/           #   PCI bus
├── lib/               # Shared kernel library
├── gui/               # Desktop environment
│   ├── HAPRYLAND/     #   Vendored upstream Hyprland source (port in progress)
│   └── hyprland/      #   From-scratch WynlandOS-native WM (parallel approach)
├── qt_qpa/             # Custom Qt6 platform plugin (qwynlandfb) -- Qt's
│                       #   window onto WynlandOS's own framebuffer
├── qtbase/             # Qt6 source, built against WynlandOS's toolchain
├── pkg/                # wynpkg package manager (+ vendored quickjs)
├── *.wyn               # Wynlang scripts (hello.wyn, browser.wyn, wynui.wyn, ...)
├── include/wynland/    # Shared headers
├── include/aquamarine/ # Vendored Aquamarine headers (Hyprland's render
│                       #   backend lib -- not yet wired into the pkg-config
│                       #   search path, see Status above)
├── tools/ovmf/         # UEFI firmware
├── Makefile            # Build system
├── setup.sh            # Toolchain setup
└── run.sh              # QEMU launcher
```

## Architecture

```
┌─────────────────────────────────────────────┐
│   User Applications (Qt6 widgets, Wynlang)   │
├─────────────────────────────────────────────┤
│         WynlandDE (Desktop Env)              │
│  ┌──────────┬───────────┬────────────────┐  │
│  │ Tiling   │ Floating  │ Dock/Panel     │  │
│  │ WM       │ WM        │ Widgets        │  │
│  └──────────┴───────────┴────────────────┘  │
│  Qt6 QPA plugin (qwynlandfb) ── Hyprland     │
│  renders straight to the         port        │
│  native framebuffer, no        (blocked on   │
│  X11/Wayland underneath        Aquamarine)   │
├─────────────────────────────────────────────┤
│         Ring 3 / Syscalls (Linux-numbered)   │
├─────────────────────────────────────────────┤
│                  Kernel                      │
│  ┌─────────┬──────────┬──────────────────┐  │
│  │ Process │ Memory   │ VFS / FAT32      │  │
│  │ + clone │ Mgmt     │ Filesystem       │  │
│  └─────────┴──────────┴──────────────────┘  │
│  ┌─────────┬──────────┬──────────────────┐  │
│  │ PCI     │ virtio   │ virtio-gpu /     │  │
│  │ Driver  │ -net     │ Input Drivers    │  │
│  └─────────┴──────────┴──────────────────┘  │
├─────────────────────────────────────────────┤
│           UEFI Bootloader                    │
├─────────────────────────────────────────────┤
│              Hardware (x86_64)               │
└─────────────────────────────────────────────┘
```

## License

This project is for educational purposes.
