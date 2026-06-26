# WynlandOS

> A custom x86_64 operating system built entirely from scratch.

## Features (Planned)

- **UEFI Bootloader** — Custom bootloader, no GRUB dependency
- **Monolithic Kernel** — Memory management, multitasking, syscalls
- **FAT32 Filesystem** — Read/write with VFS abstraction
- **Network Stack** — virtio-net driver + full TCP/IP stack
- **Package Manager** — `wynpkg` with custom `.wpkg` format
- **Desktop Environment** — Hybrid macOS + Hyprland (tiling + floating, dock, blur, animations)

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
├── kernel/            # Kernel core
├── drivers/           # Device drivers
│   ├── video/         #   Framebuffer
│   ├── input/         #   Keyboard, mouse
│   ├── net/           #   virtio-net
│   ├── fs/            #   FAT32
│   └── pci/           #   PCI bus
├── lib/               # Shared kernel library
├── gui/               # Desktop environment
├── pkg/               # Package manager
├── include/wynland/   # Shared headers
├── tools/ovmf/        # UEFI firmware
├── Makefile           # Build system
├── setup.sh           # Toolchain setup
└── run.sh             # QEMU launcher
```

## Architecture

```
┌─────────────────────────────────────────────┐
│              User Applications              │
├─────────────────────────────────────────────┤
│         WynlandDE (Desktop Env)             │
│    ┌──────────┬───────────┬────────────┐    │
│    │ Tiling   │ Floating  │ Dock/Panel │    │
│    │ WM       │ WM        │ Widgets    │    │
│    └──────────┴───────────┴────────────┘    │
├─────────────────────────────────────────────┤
│              System Calls                   │
├─────────────────────────────────────────────┤
│                  Kernel                     │
│  ┌─────────┬──────────┬──────────────────┐  │
│  │ Process │ Memory   │ VFS / FAT32      │  │
│  │ Mgmt    │ Mgmt     │ Filesystem       │  │
│  └─────────┴──────────┴──────────────────┘  │
│  ┌─────────┬──────────┬──────────────────┐  │
│  │ PCI     │ virtio   │ Framebuffer /    │  │
│  │ Driver  │ -net     │ Input Drivers    │  │
│  └─────────┴──────────┴──────────────────┘  │
├─────────────────────────────────────────────┤
│           UEFI Bootloader                   │
├─────────────────────────────────────────────┤
│              Hardware (x86_64)              │
└─────────────────────────────────────────────┘
```

## License

This project is for educational purposes.
