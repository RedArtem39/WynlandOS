#!/bin/bash
################################################################################
#  WynlandOS - Development Environment Setup
#  
#  Supports: Ubuntu/Debian (native or WSL2)
#  
#  This script installs all required build tools and downloads OVMF firmware.
################################################################################

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVMF_DIR="$SCRIPT_DIR/tools/ovmf"

echo -e "${CYAN}"
echo "╔══════════════════════════════════════════╗"
echo "║     WynlandOS - Environment Setup        ║"
echo "╚══════════════════════════════════════════╝"
echo -e "${NC}"

# ── Check OS ──────────────────────────────────────────────────────────────────

if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo -e "${GREEN}[OK]${NC} Detected: $PRETTY_NAME"
else
    echo -e "${RED}[!]${NC} Cannot detect OS. This script targets Ubuntu/Debian."
    exit 1
fi

# ── Install packages ─────────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}[1/3]${NC} Installing build tools..."
echo ""

sudo apt update -qq

PACKAGES=(
    # Core build tools
    build-essential
    nasm
    
    # UEFI bootloader compiler (PE/COFF output)
    gcc-mingw-w64-x86-64
    
    # Disk image tools (FAT32 without root)
    mtools
    dosfstools
    
    # QEMU emulator
    qemu-system-x86
    
    # OVMF UEFI firmware
    ovmf
    
    # Debugging
    gdb
    
    # Version control
    git
)

sudo apt install -y "${PACKAGES[@]}"

echo ""
echo -e "${GREEN}[OK]${NC} All packages installed."

# ── Setup OVMF firmware ──────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}[2/3]${NC} Setting up OVMF UEFI firmware..."

mkdir -p "$OVMF_DIR"

# Try common OVMF locations
OVMF_PATHS=(
    "/usr/share/OVMF/OVMF_CODE.fd"
    "/usr/share/ovmf/OVMF.fd"
    "/usr/share/qemu/OVMF.fd"
    "/usr/share/edk2/ovmf/OVMF_CODE.fd"
)

OVMF_FOUND=false
for ovmf_path in "${OVMF_PATHS[@]}"; do
    if [ -f "$ovmf_path" ]; then
        cp "$ovmf_path" "$OVMF_DIR/OVMF.fd"
        echo -e "${GREEN}[OK]${NC} OVMF firmware copied from $ovmf_path"
        OVMF_FOUND=true
        break
    fi
done

if [ "$OVMF_FOUND" = false ]; then
    # Try to find it anywhere
    OVMF_FOUND_PATH=$(find /usr/share -name "OVMF*.fd" 2>/dev/null | head -1)
    if [ -n "$OVMF_FOUND_PATH" ]; then
        cp "$OVMF_FOUND_PATH" "$OVMF_DIR/OVMF.fd"
        echo -e "${GREEN}[OK]${NC} OVMF firmware copied from $OVMF_FOUND_PATH"
    else
        echo -e "${RED}[!]${NC} OVMF firmware not found!"
        echo "    Please manually place OVMF.fd in $OVMF_DIR/"
    fi
fi

# ── Verify installation ──────────────────────────────────────────────────────

echo ""
echo -e "${YELLOW}[3/3]${NC} Verifying installation..."
echo ""

TOOLS=(
    "gcc:System C compiler"
    "x86_64-w64-mingw32-gcc:UEFI bootloader compiler (mingw)"
    "nasm:Assembler"
    "ld:Linker"
    "mformat:FAT32 image tool (mtools)"
    "qemu-system-x86_64:QEMU x86_64 emulator"
    "gdb:GNU Debugger"
)

ALL_OK=true
for tool_entry in "${TOOLS[@]}"; do
    IFS=':' read -r tool desc <<< "$tool_entry"
    if which "$tool" > /dev/null 2>&1; then
        echo -e "  ${GREEN}✓${NC} $desc ($tool)"
    else
        echo -e "  ${RED}✗${NC} $desc ($tool) - NOT FOUND"
        ALL_OK=false
    fi
done

# Check OVMF
if [ -f "$OVMF_DIR/OVMF.fd" ]; then
    echo -e "  ${GREEN}✓${NC} OVMF firmware"
else
    echo -e "  ${RED}✗${NC} OVMF firmware - NOT FOUND"
    ALL_OK=false
fi

echo ""
if [ "$ALL_OK" = true ]; then
    echo -e "${GREEN}╔══════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  ✓  All tools installed successfully!    ║${NC}"
    echo -e "${GREEN}║                                          ║${NC}"
    echo -e "${GREEN}║  Next steps:                             ║${NC}"
    echo -e "${GREEN}║    make          - Build WynlandOS       ║${NC}"
    echo -e "${GREEN}║    make run      - Run in QEMU           ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════╝${NC}"
else
    echo -e "${RED}[!] Some tools are missing. Please install them manually.${NC}"
    exit 1
fi
