#!/bin/bash
################################################################################
#  WynlandOS - QEMU Launch Script (Linux/WSL2)
################################################################################

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
OVMF="$SCRIPT_DIR/tools/ovmf/OVMF.fd"
DISK="$BUILD_DIR/wynland.img"

# Check prerequisites
if [ ! -f "$OVMF" ]; then
    echo "ERROR: OVMF firmware not found at $OVMF"
    echo "Run: ./setup.sh"
    exit 1
fi

if [ ! -f "$DISK" ]; then
    echo "ERROR: Disk image not found. Run: make"
    exit 1
fi

# Parse arguments
DEBUG=""
EXTRA_ARGS=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --debug|-d)
            DEBUG="-S -s -d int,cpu_reset"
            echo "Debug mode: connect GDB to localhost:1234"
            shift
            ;;
        --no-net)
            EXTRA_ARGS="$EXTRA_ARGS -net none"
            shift
            ;;
        *)
            EXTRA_ARGS="$EXTRA_ARGS $1"
            shift
            ;;
    esac
done

# Default network: virtio-net with user-mode networking
NET_ARGS="-device virtio-net-pci,netdev=net0 -netdev user,id=net0"
if [[ "$EXTRA_ARGS" == *"-net none"* ]]; then
    NET_ARGS=""
fi

echo "╔══════════════════════════════════════╗"
echo "║      WynlandOS - Starting QEMU      ║"
echo "╚══════════════════════════════════════╝"
echo ""

qemu-system-x86_64              \
    -machine q35                \
    -cpu qemu64                 \
    -accel kvm                  \
    -accel tcg                  \
    -m 256M                     \
    -bios "$OVMF"              \
    -drive file="$DISK",format=raw \
    -vga none                  \
    -device virtio-gpu-pci,disable-legacy=off,disable-modern=on \
    $NET_ARGS                   \
    -serial stdio              \
    -no-reboot                 \
    -no-shutdown               \
    $DEBUG                     \
    $EXTRA_ARGS
