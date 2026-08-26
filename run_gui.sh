#!/bin/bash
# WynlandOS GUI launcher.
# KVM (fast, seconds-to-desktop) when /dev/kvm is available & permitted,
# otherwise falls back to TCG (slow: expect ~3-5 min of black screen).
# Serial log: build/serial.log -- tail it to watch boot progress.
cd "$(dirname "$0")" || exit 1

rm -f build/serial.log

if [ -w /dev/kvm ]; then
    echo "[run] accel: KVM (fast)"
    MACHINE="-machine q35"
    CPU="-cpu host"
    ACCEL="-accel kvm"
else
    echo "[run] accel: TCG fallback (slow, ~3-5 min to desktop)."
    echo "[run] tip: wsl -u root bash -lc 'chmod 666 /dev/kvm'   # then rerun"
    MACHINE="-machine q35,kernel-irqchip=off"
    CPU="-cpu qemu64"
    ACCEL="-accel tcg"
fi

qemu-system-x86_64 \
    $MACHINE \
    $CPU \
    $ACCEL \
    -m 512M \
    -bios tools/ovmf/OVMF.fd \
    -drive file=build/wynland.img,format=raw \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0 \
    -vga virtio \
    -display sdl \
    -serial file:build/serial.log \
    -no-reboot \
    -no-shutdown

echo "QEMU exited. Serial log: build/serial.log"
