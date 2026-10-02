#!/bin/bash
# WynlandOS wallpaper setter (host side).
#   wsl bash tools/setwall.sh /path/to/image.png
# Copies the PNG to build/wall.png, injects it as /wall.png into the ext2
# root partition, and reassembles the disk. Any PNG <= ~512KB, any size
# (the compositor nearest-neighbor stretches it to the screen).
set -e
cd "$(dirname "$0")/.." || exit 1

if [ -z "$1" ] || [ ! -f "$1" ]; then
    echo "usage: wsl bash tools/setwall.sh <image.png>"
    exit 1
fi

# the images are where the Makefile keeps them (IMG_DIR)
IMG="${IMG_DIR:-$HOME/.cache/wynland-img}"
[ -f "$IMG/ext2_part.img" ] || IMG=build

cp "$1" build/wall.png
printf 'rm /wall.png\nwrite %s /wall.png\n' "$(pwd)/build/wall.png" | debugfs -w -f- "$IMG/ext2_part.img" > /dev/null 2>&1
dd if="$IMG/ext2_part.img" of="$IMG/wynland.img" bs=1M seek=65 conv=notrunc status=none
echo "wallpaper set. (re)launch with: wsl bash run_gui.sh"