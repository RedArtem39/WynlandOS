################################################################################
#                         WynlandOS - Build System                             #
#                                                                              #
#  Usage:                                                                      #
#    make              - Build everything (bootloader + kernel + disk image)    #
#    make bootloader   - Build only the UEFI bootloader                        #
#    make kernel       - Build only the kernel                                 #
#    make image        - Create the disk image                                 #
#    make run          - Build and run in QEMU                                 #
#    make debug        - Build and run in QEMU with GDB server                 #
#    make clean        - Remove all build artifacts                            #
#                                                                              #
#  Requirements: gcc-mingw-w64, gcc, nasm, mtools, qemu, ovmf                 #
################################################################################

# ============================================================================
# Toolchain Configuration
# ============================================================================

# Bootloader compiler (mingw produces PE/COFF directly - native UEFI format)
CC_BOOT    = x86_64-w64-mingw32-gcc
LD_BOOT    = x86_64-w64-mingw32-ld

# Kernel compiler (system gcc works on x86_64 with freestanding flags)
CC_KERNEL  = gcc
CXX_KERNEL = g++
LD_KERNEL  = ld

# Assembler
AS         = nasm

# Utilities
OBJCOPY    = objcopy

# ============================================================================
# Directories
# ============================================================================

SRC_BOOT       = boot
SRC_KERNEL     = kernel
SRC_DRIVERS    = drivers
SRC_LIB        = lib
SRC_PKG        = pkg
SRC_GUI        = gui
INC_DIR        = include

BUILD          = build
BUILD_BOOT     = $(BUILD)/boot
BUILD_KERNEL   = $(BUILD)/kernel
BUILD_DRIVERS  = $(BUILD)/drivers
BUILD_LIB      = $(BUILD)/lib
BUILD_PKG      = $(BUILD)/pkg
BUILD_GUI      = $(BUILD)/gui
BUILD_ESP      = $(BUILD)/esp

# ============================================================================
# Compiler Flags
# ============================================================================

# Common flags for freestanding code
COMMON_FLAGS   = -ffreestanding           \
                 -fno-stack-protector      \
                 -fno-stack-check          \
                 -fno-exceptions           \
                 -mno-red-zone            \
                 -Wall -Wextra -Werror    \
                 -I$(INC_DIR)

# Bootloader: UEFI PE/COFF target
CFLAGS_BOOT    = $(COMMON_FLAGS)          \
                 -fshort-wchar            \
                 -mno-sse                 \
                 -std=c11                 \
                 -O2

LDFLAGS_BOOT   = --subsystem 10           \
                 -e EfiMain               \
                 -nostdlib                 \
                 -s

# Kernel: ELF64 freestanding
# SYSCALL_TRACE=1: log open/openat/mkdir/pipe/ioctl calls on the serial console
SYSCALL_TRACE ?= 0
CFLAGS_KERNEL  = $(COMMON_FLAGS)          \
                 -DSYSCALL_TRACE=$(SYSCALL_TRACE) \
                 -mcmodel=large           \
                 -fno-pie                 \
                 -fno-pic                 \
                 -fno-jump-tables         \
                 -nostdlib                \
                 -nostdinc                \
                 -std=c11                 \
                 -O2                      \
                 -MMD -MP

CXXFLAGS_KERNEL = $(COMMON_FLAGS)         \
                  -MMD -MP                \
                  -mcmodel=large          \
                  -fno-pie                \
                  -fno-pic                \
                  -fno-jump-tables        \
                  -fno-rtti               \
                  -fno-exceptions         \
                  -nostdlib               \
                  -nostdinc               \
                  -std=c++17              \
                  -O2

LDFLAGS_KERNEL = -T $(SRC_KERNEL)/linker.ld \
                 -nostdlib                  \
                 -z max-page-size=0x1000

ASFLAGS        = -f elf64 -g -I kernel/

# ============================================================================
# Source Files
# ============================================================================

# Bootloader sources
BOOT_C_SRC     = $(wildcard $(SRC_BOOT)/*.c)
BOOT_OBJ       = $(patsubst $(SRC_BOOT)/%.c, $(BUILD_BOOT)/%.o, $(BOOT_C_SRC))

# Kernel sources (C + CPP + ASM)
KERNEL_C_SRC   = $(wildcard $(SRC_KERNEL)/*.c)
KERNEL_CPP_SRC = $(wildcard $(SRC_KERNEL)/*.cpp)
KERNEL_ASM_SRC = $(wildcard $(SRC_KERNEL)/*.asm)
KERNEL_C_OBJ   = $(patsubst $(SRC_KERNEL)/%.c, $(BUILD_KERNEL)/%.o, $(KERNEL_C_SRC))
KERNEL_CPP_OBJ = $(patsubst $(SRC_KERNEL)/%.cpp, $(BUILD_KERNEL)/%.o, $(KERNEL_CPP_SRC))
KERNEL_ASM_OBJ = $(patsubst $(SRC_KERNEL)/%.asm, $(BUILD_KERNEL)/%.o, $(KERNEL_ASM_SRC))

# Driver sources
DRIVER_C_SRC   = $(shell find $(SRC_DRIVERS) -name '*.c' 2>/dev/null)
DRIVER_ASM_SRC = $(shell find $(SRC_DRIVERS) -name '*.asm' 2>/dev/null)
DRIVER_C_OBJ   = $(patsubst $(SRC_DRIVERS)/%.c, $(BUILD_DRIVERS)/%.o, $(DRIVER_C_SRC))
DRIVER_ASM_OBJ = $(patsubst $(SRC_DRIVERS)/%.asm, $(BUILD_DRIVERS)/%.o, $(DRIVER_ASM_SRC))

# Lib sources
LIB_C_SRC      = $(wildcard $(SRC_LIB)/*.c)
LIB_OBJ        = $(patsubst $(SRC_LIB)/%.c, $(BUILD_LIB)/%.o, $(LIB_C_SRC))

# Package manager sources
PKG_C_SRC      = $(wildcard $(SRC_PKG)/*.c)
PKG_OBJ        = $(patsubst $(SRC_PKG)/%.c, $(BUILD_PKG)/%.o, $(PKG_C_SRC))

# GUI sources
GUI_C_SRC      = $(wildcard $(SRC_GUI)/*.c)
GUI_CPP_SRC    = $(wildcard $(SRC_GUI)/*.cpp $(SRC_GUI)/hyprland/*.cpp)
GUI_OBJ        = $(patsubst $(SRC_GUI)/%.c, $(BUILD_GUI)/%.o, $(GUI_C_SRC))
GUI_CPP_OBJ    = $(patsubst $(SRC_GUI)/%.cpp, $(BUILD_GUI)/%.o, $(GUI_CPP_SRC))

# QuickJS sources
QUICKJS_DIR    = pkg/quickjs
QUICKJS_SRC    = $(QUICKJS_DIR)/quickjs.c \
                 $(QUICKJS_DIR)/dtoa.c \
                 $(QUICKJS_DIR)/libregexp.c \
                 $(QUICKJS_DIR)/libunicode.c \
                 $(QUICKJS_DIR)/cutils.c \
                 $(QJS_COMPAT)/compat.c \
                 $(QJS_COMPAT)/quickjs_binding.c
# our libc shim + kernel binding for QuickJS (pkg/quickjs is upstream's submodule)
QJS_COMPAT     = pkg/quickjs-compat
QUICKJS_OBJ    = $(patsubst $(QJS_COMPAT)/%.c, $(BUILD_GUI)/quickjs_%.o, $(patsubst $(QUICKJS_DIR)/%.c, $(BUILD_GUI)/quickjs_%.o, $(QUICKJS_SRC)))

# All kernel-side objects
KERNEL_ALL_OBJ = $(KERNEL_ASM_OBJ) $(KERNEL_C_OBJ) $(KERNEL_CPP_OBJ) $(DRIVER_C_OBJ) $(DRIVER_ASM_OBJ) $(LIB_OBJ) $(PKG_OBJ) $(GUI_OBJ) $(GUI_CPP_OBJ) $(QUICKJS_OBJ)

# ============================================================================
# Output Files
# ============================================================================

BOOTLOADER_EFI = $(BUILD)/BOOTX64.EFI
KERNEL_ELF     = $(BUILD)/kernel.elf
# The 2 GB disk images live in the WSL filesystem when building under WSL:
# on /mnt/c every write the VM makes goes through the Windows file bridge
# (seconds per burst) and debugfs/e2fsck are slow there too.
IMG_DIR       ?= $(if $(wildcard /mnt/wslg),$(HOME)/.cache/wynland-img,$(BUILD))
$(shell mkdir -p $(IMG_DIR))
DISK_IMAGE     = $(IMG_DIR)/wynland.img
OVMF_FW        = tools/ovmf/OVMF.fd

# Two-partition disk layout (real MBR): a small FAT32 ESP holding only
# what UEFI firmware needs (BOOTX64.EFI + kernel.elf), plus the ext2
# root partition populated from ext2_manifest.txt. All 1MiB-aligned.
# ESP must stay >= 64MB: below ~65525 clusters real OVMF firmware
# refuses the FAT32 volume that mtools still accepts (Phase 20a finding).
ESP_SIZE_MB    = 64
TOTAL_IMG_MB   = 2048
ESP_PART_IMG   = $(IMG_DIR)/esp_part.img
EXT2_PART_IMG  = $(IMG_DIR)/ext2_part.img
MBR_SECT       = $(BUILD)/mbr.bin
EXT2_MANIFEST  = ext2_manifest.txt

# ============================================================================
# Targets
# ============================================================================

.PHONY: all bootloader kernel image run run-gl debug clean dirs check-tools

# Default target
all: check-tools $(DISK_IMAGE)
	@echo ""
	@echo "======================================"
	@echo "  WynlandOS build complete!"
	@echo "  Run 'make run' to launch in QEMU"
	@echo "======================================"

# Create build directories
dirs:
	@mkdir -p $(BUILD_BOOT)
	@mkdir -p $(BUILD_KERNEL)
	@mkdir -p $(BUILD_DRIVERS)
	@mkdir -p $(BUILD_LIB)
	@mkdir -p $(BUILD_PKG)
	@mkdir -p $(BUILD_GUI)
	@mkdir -p $(BUILD_ESP)/EFI/BOOT

# ---------- Bootloader ----------

bootloader: dirs $(BOOTLOADER_EFI)

$(BUILD_BOOT)/%.o: $(SRC_BOOT)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(BOOT)   $<"
	@$(CC_BOOT) $(CFLAGS_BOOT) -c $< -o $@

$(BOOTLOADER_EFI): $(BOOT_OBJ)
	@echo "  LD(BOOT)   $@"
	@$(LD_BOOT) $(LDFLAGS_BOOT) -o $@ $^
	@echo "  => BOOTX64.EFI created"

# ---------- Kernel ----------

kernel: dirs $(KERNEL_ELF)

$(BUILD_KERNEL)/%.o: $(SRC_KERNEL)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(KERN)   $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) -c $< -o $@

$(BUILD_KERNEL)/%.o: $(SRC_KERNEL)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX(KERN)  $<"
	@$(CXX_KERNEL) $(CXXFLAGS_KERNEL) -c $< -o $@

$(BUILD_KERNEL)/%.o: $(SRC_KERNEL)/%.asm $(SRC_KERNEL)/fpu.inc
	@mkdir -p $(dir $@)
	@echo "  AS(KERN)   $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Driver objects
$(BUILD_DRIVERS)/%.o: $(SRC_DRIVERS)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(DRV)    $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) -c $< -o $@

$(BUILD_DRIVERS)/%.o: $(SRC_DRIVERS)/%.asm
	@mkdir -p $(dir $@)
	@echo "  AS(DRV)    $<"
	@$(AS) $(ASFLAGS) $< -o $@

# Lib objects
$(BUILD_LIB)/%.o: $(SRC_LIB)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(LIB)    $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) -c $< -o $@

# Package manager objects
$(BUILD_PKG)/%.o: $(SRC_PKG)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(PKG)    $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) -c $< -o $@

$(BUILD_GUI)/%.o: $(SRC_GUI)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(GUI)    $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) -I$(SRC_GUI) -c $< -o $@

$(BUILD_GUI)/%.o: $(SRC_GUI)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX(GUI)   $<"
	@$(CXX_KERNEL) $(CXXFLAGS_KERNEL) -I$(SRC_GUI) -c $< -o $@

QJS_CFLAGS = -I$(QJS_COMPAT)/include -I$(QJS_COMPAT) -Ipkg/quickjs -include $(QJS_COMPAT)/compat.h

$(BUILD_GUI)/quickjs_%.o: $(QJS_COMPAT)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(QJS)    $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) $(QJS_CFLAGS) -DCONFIG_VERSION=\"2024-01-13\" -DCONFIG_BIGNUM=0 -Wno-unused-parameter -Wno-unused-variable -Wno-implicit-fallthrough -Wno-return-type -Wno-sign-compare -Wno-unused-function -Wno-maybe-uninitialized -Wno-unused-but-set-variable -Wno-format -Wno-int-conversion -c $< -o $@

$(BUILD_GUI)/quickjs_%.o: $(QUICKJS_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(QJS)    $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) $(QJS_CFLAGS) -DCONFIG_VERSION=\"2024-01-13\" -DCONFIG_BIGNUM=0 -Wno-unused-parameter -Wno-unused-variable -Wno-implicit-fallthrough -Wno-return-type -Wno-sign-compare -Wno-unused-function -Wno-maybe-uninitialized -Wno-unused-but-set-variable -Wno-format -Wno-int-conversion -c $< -o $@

$(KERNEL_ELF): $(KERNEL_ALL_OBJ)
	@echo "  LD(KERN)   $@"
	@$(LD_KERNEL) $(LDFLAGS_KERNEL) -o $@ $^
	@echo "  => kernel.elf created"

$(BUILD)/test.bin: test.c
	@mkdir -p $(BUILD)
	@echo "  CC(USER)   $<"
	@gcc -nostdlib -nostartfiles -nodefaultlibs -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -no-pie -Ttext 0x40000000 -o $(BUILD)/test.elf $<
	@objcopy -j .text -j .rodata -j .data -O binary $(BUILD)/test.elf $@

$(BUILD)/test_cpp.bin: test_cpp.cpp
	@mkdir -p $(BUILD)
	@echo "  CXX(USER)  $<"
	@g++ -I$(INC_DIR) -nostdlib -nostartfiles -nodefaultlibs -fno-rtti -fno-exceptions -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -no-pie -Ttext 0x40000000 -std=c++17 -o $(BUILD)/test_cpp.elf $<
	@objcopy -j .text -j .rodata -j .data -O binary $(BUILD)/test_cpp.elf $@

$(BUILD)/test_dev.bin: test_dev.cpp
	@mkdir -p $(BUILD)
	@echo "  CXX(USER)  $<"
	@g++ -I$(INC_DIR) -nostdlib -nostartfiles -nodefaultlibs -fno-rtti -fno-exceptions -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -no-pie -Ttext 0x40000000 -std=c++17 -o $(BUILD)/test_dev.elf $<
	@objcopy -j .text -j .rodata -j .data -O binary $(BUILD)/test_dev.elf $@

$(BUILD)/test_qt.bin: test_qt.cpp
	@mkdir -p $(BUILD)
	@echo "  CXX(USER)  $<"
	@g++ -I$(INC_DIR) -nostdlib -nostartfiles -nodefaultlibs -fno-rtti -fno-exceptions -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -no-pie -Ttext 0x40000000 -std=c++17 -o $(BUILD)/test_qt.elf $<
	@objcopy -j .text -j .rodata -j .data -O binary $(BUILD)/test_qt.elf $@

$(BUILD)/t_clone.bin: t_clone.c
	@mkdir -p $(BUILD)
	@echo "  CC(USER)   $<"
	@gcc -nostdlib -nostartfiles -nodefaultlibs -fno-pie -fno-pic -fno-stack-protector -mno-red-zone -no-pie -Ttext 0x40000000 -o $(BUILD)/t_clone.elf $<
	@objcopy -j .text -j .rodata -j .data -O binary $(BUILD)/t_clone.elf $@

$(BUILD)/interp.elf: interp.c
	@mkdir -p $(BUILD)
	@echo "  CC(USER)   $< (Interpreter)"
	@gcc -shared -fPIC -nostdlib -nostartfiles -nodefaultlibs -Wl,-e,_start -o $@ $<

$(BUILD)/main_dynamic.elf: main_dynamic.c
	@mkdir -p $(BUILD)
	@echo "  CC(USER)   $< (Dynamic Main)"
	@gcc -nostdlib -nostartfiles -nodefaultlibs -fPIE -pie -Wl,-dynamic-linker,/lib/ld-dummy.so -o $@ $<

$(BUILD)/test_qt_real.elf: test_qt_real.cpp
	@mkdir -p $(BUILD)
	@echo "  CXX(USER)  $< (Real Qt6 App)"
	@tools/x86_64-linux-musl-cross/bin/x86_64-linux-musl-g++ -O2 -DQT_NO_DEBUG -fPIE -pie -Ibuild_qt_headers/include -Iinclude -Lbuild/lib -lQt6Widgets -lQt6Gui -lQt6DBus -lQt6Core -Wl,-rpath,/lib -o $@ $<

# Framebuffer diagnostic utility — statically linked, no Qt dependency
$(BUILD)/test_raw_fb.elf: test_raw_fb.c
	@mkdir -p $(BUILD)
	@echo "  CC(USER)   $< (FB diagnostic)"
	@tools/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc -static -O2 -o $@ $<

# AF_UNIX / SCM_RIGHTS / memfd test: a plain HOST-gcc glibc binary,
# dynamically linked against the /lib64 glibc the image already ships
# (same as the other /lib64 software). PIE: the loader places ET_DYN
# images above the shared kernel range. Run from the shell: exec /test_afunix.elf
$(BUILD)/test_afunix.elf: test_afunix.c
	@mkdir -p $(BUILD)
	@echo "  CC(GLIBC)  $< (AF_UNIX test)"
	@gcc -O2 -Wall -Wno-unused-result -fPIE -pie -o $@ $<

# Zerp compositor + its default clients: freestanding, no libc (see zerp.c).
ZERP_ELFS = $(BUILD)/zerp.elf $(BUILD)/zerp_files.elf $(BUILD)/zerp_term.elf
ZERP_HDRS = zerp_syscalls.h zerp_protocol.h zerp_client.h zerp_entry.h zerp_png.h zerp_font.h zerp_vt100.h
$(ZERP_ELFS): $(BUILD)/%.elf: %.c $(ZERP_HDRS)
	@mkdir -p $(BUILD)
	@echo "  CC(USER)   $< (Zerp)"
	@tools/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc -nostdlib -static -no-pie -fno-pie -mcmodel=large -O2 $(ZERP_CFLAGS) -Wl,-Ttext-segment=0x340000000000 -o $@ $<

# virgl end-to-end test: a host-built glibc binary against the /lib64 Mesa
# the image already ships (EGL/GBM/GLESv2) -- see tests/gltest.c.
$(BUILD)/gltest.elf: tests/gltest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, virgl test)"
	@gcc -O2 -o $@ $< -lEGL -lGLESv2 -lgbm

$(BUILD)/kmstest.elf: tests/kmstest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, KMS + GPU presentation test)"
	@gcc -O2 -I/usr/include/libdrm -o $@ $< -ldrm -lgbm -lEGL -lGLESv2

$(BUILD)/jittest.elf: tests/jittest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, JIT memory policy)"
	@gcc -O2 -o $@ $<

$(BUILD)/gsttest.elf: tests/gsttest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, GStreamer check)"
	@gcc -O2 -o $@ $<

$(BUILD)/wpetest.elf: tests/wpetest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, WPE WebKit check)"
	@gcc -O2 -o $@ $<

$(BUILD)/sndtest.elf: tests/sndtest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, sound check)"
	@gcc -O2 -o $@ $< -lm

$(BUILD)/shtest.elf: tests/shtest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, shell + coreutils check)"
	@gcc -O2 -o $@ $<

$(BUILD)/forktest.elf: tests/forktest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, process lifecycle test)"
	@gcc -O2 -pthread -o $@ $<

# wynrc: the init (/sbin/init) and rc-status/rc-service/rc-update
$(BUILD)/wynrc.elf: apps/wynrc/wynrc.c apps/wynrc/wynrc.h
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, init)"
	@gcc -O2 -Wall -Wl,-rpath,/lib64 -o $@ $<
$(BUILD)/rc.elf: apps/wynrc/rc.c apps/wynrc/wynrc.h
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, rc tools)"
	@gcc -O2 -Wall -Wl,-rpath,/lib64 -o $@ $<

$(BUILD)/dlsymtest.elf: tests/dlsymtest.c
	@mkdir -p $(BUILD)
	@echo "  CC(HOST)   $< (glibc, dlsym diagnostic)"
	@gcc -O2 -o $@ $<

# Real TTF font for QFreeTypeFontDatabase (vendored inside qtbase's own 3rdparty tree)
$(BUILD)/lib/fonts/DejaVuSans.ttf: qtbase/src/3rdparty/wasm/DejaVuSans.ttf
	@mkdir -p $(BUILD)/lib/fonts
	@echo "  CP         $< -> $@"
	@cp $< $@

# ---------- Disk Image ----------

image: bootloader kernel $(BUILD)/test.bin $(BUILD)/test_cpp.bin $(BUILD)/test_dev.bin $(BUILD)/test_qt.bin $(BUILD)/t_clone.bin $(BUILD)/interp.elf $(BUILD)/main_dynamic.elf $(BUILD)/test_raw_fb.elf $(BUILD)/lib/fonts/DejaVuSans.ttf $(DISK_IMAGE)

$(BUILD)/card0:
	@echo "mock" > $@

$(BUILD)/renderD128:
	@echo "mock" > $@

# ext2 root partition: built by the host's own mke2fs + debugfs (root-free),
# populated from the manifest, verified file-by-file afterwards.
PORT_STAGING = $(wildcard build/ports/curl build/ports/nano build/ports/pkgconf build/ports/cmake build/ports/cert.pem)
# Qt 6 / Qt Quick (tools/stage_qt6.sh): Ubuntu's prebuilt Qt, QML modules and the
# qmldemo app. WITH_QT6=0 builds the image without it.
WITH_QT6 ?= 1
ifeq ($(WITH_QT6),1)
QT6_MANIFEST = $(BUILD)/qt6_manifest.txt
$(QT6_MANIFEST): tools/stage_qt6.sh apps/qml/qmldemo.cpp apps/qml/demo.qml $(wildcard apps/files/*.cpp apps/files/*.h apps/files/qml/*.qml) $(wildcard apps/zerp2/*.cpp apps/zerp2/*.h apps/zerp2/*.json apps/zerp2/qml/*.qml) rootfs/usr/bin/qt.conf $(wildcard qt_qpa/*.cpp qt_qpa/*.h) $(EXT2_MANIFEST)
	@bash tools/stage_qt6.sh
else
QT6_MANIFEST =
endif
# GStreamer (Ubuntu's, glibc): tools, a set of plugins, test media and
# the gsttest check. WITH_GST=0 builds the image without it.
WITH_GST ?= 1
ifeq ($(WITH_GST),1)
GST_MANIFEST = $(BUILD)/gst_manifest.txt
$(GST_MANIFEST): tools/stage_gst.sh $(EXT2_MANIFEST) $(QT6_MANIFEST) $(BUILD)/gsttest.elf rootfs/etc/wynrc/services/gsttest
	@bash tools/stage_gst.sh
else
GST_MANIFEST =
endif
# WPE WebKit (Debian sid's, with Ubuntu's libraries where they fit) and
# the wpetest check. WITH_WPE=0 builds the image without it.
WITH_WPE ?= 1
ifeq ($(WITH_WPE),1)
WPE_MANIFEST = $(BUILD)/wpe_manifest.txt
$(WPE_MANIFEST): tools/stage_wpe.sh tools/wpe_deps.py tests/wpeshot.c $(wildcard apps/web/*.c apps/web/*.cpp apps/web/*.h apps/web/qml/*.qml) $(BUILD)/wpetest.elf rootfs/etc/wynrc/services/wpetest $(wildcard rootfs/usr/share/wynland/web/*) $(EXT2_MANIFEST) $(QT6_MANIFEST) $(GST_MANIFEST)
	@bash tools/stage_wpe.sh
else
WPE_MANIFEST =
endif
# The base userland (Ubuntu's): GNU coreutils, fish, dash as /bin/sh,
# grep, sed, less, tar ... and the shtest check. WITH_BASE=0 leaves it out.
WITH_BASE ?= 1
ifeq ($(WITH_BASE),1)
BASE_MANIFEST = $(BUILD)/base_manifest.txt
$(BASE_MANIFEST): tools/stage_base.sh tools/python_launcher.c $(BUILD)/shtest.elf rootfs/etc/wynrc/services/shtest rootfs/etc/fish/conf.d/wynland.fish $(EXT2_MANIFEST) $(QT6_MANIFEST) $(GST_MANIFEST) $(WPE_MANIFEST)
	@bash tools/stage_base.sh
else
BASE_MANIFEST =
endif
# Boot choices, read by the kernel from /etc/wynland/boot.cfg:
#   ZERP=2      desktop: Zerp 2.0 (Qt Quick on the GPU; needs virgl) or 1 (classic)
#   AUTOTEST=1  run the test programs at boot (gltest, forktest, kmstest)
#   SNAPSHOT=1  Zerp 2.0 writes a screenshot to the serial log (debugging)
ZERP     ?= 2
AUTOTEST ?= 0
QEMU_EXTRA ?=
# RAM of the run-gl VM (a browser on YouTube wants a few GB; 4096M and up
# can starve WSL's own VM, which has half the host's RAM by default)
MEM ?= 3072M
# Sound: an Intel HDA card with an output codec. AUDIO=pa plays through
# PulseAudio (WSLg's server when there is one: the Windows speakers),
# AUDIO=wav records into AUDIO_WAV (for checking it without ears),
# AUDIO=none keeps the card but discards the sound.
AUDIO ?= pa
AUDIO_WAV ?= /tmp/wynland-audio.wav
ifeq ($(AUDIO),pa)
AUDIO_DEV = -audiodev pa,id=snd0$(if $(wildcard /mnt/wslg/PulseServer),$(comma)server=unix:/mnt/wslg/PulseServer,)
else ifeq ($(AUDIO),wav)
AUDIO_DEV = -audiodev wav,id=snd0,path=$(AUDIO_WAV),out.frequency=48000
else
AUDIO_DEV = -audiodev none,id=snd0
endif
comma := ,
AUDIO_ARGS = $(AUDIO_DEV) -device intel-hda -device hda-output,audiodev=snd0
SNAPSHOT ?= 0
BOOT_CFG = $(BUILD)/boot.cfg
$(BOOT_CFG): FORCE
	@mkdir -p $(BUILD)
	@printf 'zerp=$(ZERP)\nautotest=$(AUTOTEST)\n%s%s\n' "$(if $(filter 1,$(SNAPSHOT)),snapshot=1\n,)" "$(BOOT_EXTRA)" | sed 's/\\n/\n/g' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

EXT2_MANIFEST_FULL = $(BUILD)/ext2_manifest_full.txt
$(EXT2_MANIFEST_FULL): $(EXT2_MANIFEST) $(QT6_MANIFEST) $(GST_MANIFEST) $(WPE_MANIFEST) $(BASE_MANIFEST) $(BOOT_CFG)
	@mkdir -p $(BUILD)
	@cat $(EXT2_MANIFEST) $(QT6_MANIFEST) $(GST_MANIFEST) $(WPE_MANIFEST) $(BASE_MANIFEST) > $@
	@printf 'D /etc/wynland\nF /etc/wynland/boot.cfg $(BOOT_CFG)\n' >> $@

$(EXT2_PART_IMG): build_ext2_image.py $(EXT2_MANIFEST_FULL) $(BUILD)/wall.png $(BUILD)/wynrc.elf $(BUILD)/rc.elf $(wildcard rootfs/etc/wynrc/*/*) $(BUILD)/card0 $(BUILD)/renderD128 $(PORT_STAGING) $(ZERP_ELFS) $(BUILD)/gltest.elf $(BUILD)/dlsymtest.elf $(BUILD)/forktest.elf $(BUILD)/kmstest.elf $(BUILD)/test_afunix.elf $(BUILD)/sndtest.elf $(BUILD)/jittest.elf
	@python3 build_ext2_image.py $@ $$(( ($(TOTAL_IMG_MB) - 1 - $(ESP_SIZE_MB)) )) $(EXT2_MANIFEST_FULL)
	@e2fsck -f -n $@ > /dev/null 2>&1 && echo "  EXT2       e2fsck: clean" || echo "  EXT2       WARNING: e2fsck reported issues"

# ESP partition image: FAT32, firmware-loadable content only.
# Screen size the bootloader asks the firmware for (boot/boot.c reads
# \resolution.cfg from the ESP): make RESOLUTION=2560x1440 ...
RESOLUTION ?= 1920x1080
$(BUILD)/resolution.cfg: FORCE
	@mkdir -p $(BUILD)
	@echo "$(RESOLUTION)" | cmp -s - $@ || echo "$(RESOLUTION)" > $@

FORCE:

$(ESP_PART_IMG): $(BOOTLOADER_EFI) $(KERNEL_ELF) $(BUILD)/resolution.cfg
	@echo "  IMG        Building FAT32 ESP partition..."
	@mformat -i $@ -C -T $$(( $(ESP_SIZE_MB) * 2048 )) -F -v WYNLAND ::
	@mmd -i $@ ::/EFI
	@mmd -i $@ ::/EFI/BOOT
	@mcopy -o -i $@ $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -o -i $@ $(KERNEL_ELF) ::/kernel.elf
	@mcopy -o -i $@ $(BUILD)/resolution.cfg ::/resolution.cfg

# Final assembly: zeroed disk <- MBR sector <- ESP @1MiB <- ext2 root @1MiB+ESP.
$(DISK_IMAGE): $(ESP_PART_IMG) $(EXT2_PART_IMG)
	@echo "  IMG        Assembling MBR + ESP + ext2 disk image..."
	@dd if=/dev/zero of=$@ bs=1M count=$(TOTAL_IMG_MB) status=none
	@python3 make_mbr.py $(MBR_SECT) $(TOTAL_IMG_MB) $(ESP_SIZE_MB)
	@dd if=$(MBR_SECT) of=$@ bs=512 count=1 conv=notrunc status=none
	@dd if=$(ESP_PART_IMG) of=$@ bs=1M seek=1 conv=notrunc status=none
	@dd if=$(EXT2_PART_IMG) of=$@ bs=1M seek=$$(( 1 + $(ESP_SIZE_MB) )) conv=notrunc status=none
	@echo "  => wynland.img created ($(TOTAL_IMG_MB) MB, ESP $(ESP_SIZE_MB) MB + ext2 root)"

# ---------- Run in QEMU ----------

run: all
	@echo ""
	@echo "  Starting transparent HTTPS proxy on host..."
	@python3 tools/proxy.py & PROXY_PID=$$! ; \
	trap 'kill $$PROXY_PID 2>/dev/null || true' EXIT; \
	echo ""; \
	echo "  Launching WynlandOS in QEMU..."; \
	echo ""; \
	qemu-system-x86_64                                    \
		-machine q35                                      \
		-cpu qemu64                                       \
		-m 1024M                                          \
		-bios $(OVMF_FW)                                  \
		-drive file=$(DISK_IMAGE),format=raw              \
		-device virtio-net-pci,netdev=net0                \
		-netdev user,id=net0                              \
		-serial stdio                                     \
		-no-reboot                                        \
		-no-shutdown

# 3D: virtio-gpu with virgl, rendered by the host GPU through WSLg (D3D12).
# KVM is required: under TCG, QEMU 10.2 deadlocks in virtio-gpu-gl reset.
# -vga none: otherwise q35 adds a second (std VGA) display, the window shows
# that one, and everything drawn through virtio-gpu lands on a hidden tab.
# GPU_ADAPTER picks the host adapter (default NVIDIA; e.g. GPU_ADAPTER=AMD).
# UI picks QEMU's window: gtk (menus, but under WSLg it may ignore input),
# sdl (plain SDL window over Wayland; Ctrl+Alt releases the pointer).
GPU_ADAPTER ?= NVIDIA
UI ?= gtk
ifeq ($(UI),sdl)
QEMU_UI = SDL_VIDEODRIVER=wayland
QEMU_DISPLAY = sdl,gl=es
else
QEMU_UI =
QEMU_DISPLAY = gtk,gl=on,zoom-to-fit=off
endif
run-gl: all
	@echo ""
	@echo "  Launching WynlandOS in QEMU (KVM, virgl 3D on $(GPU_ADAPTER), $(UI) window)..."
	@echo ""
	$(QEMU_UI) GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=$(GPU_ADAPTER) \
	qemu-system-x86_64                                    \
		-machine q35,accel=kvm                            \
		-cpu host                                         \
		-m $(MEM)                                         \
		-vga none                                         \
		-bios $(OVMF_FW)                                  \
		-drive file=$(DISK_IMAGE),format=raw              \
		-device virtio-gpu-gl-pci                         \
		-display $(QEMU_DISPLAY)                          \
		$(QEMU_EXTRA) $(AUDIO_ARGS)                       \
		-device virtio-net-pci,netdev=net0                \
		-netdev user,id=net0                              \
		-serial stdio                                     \
		-no-reboot                                        \
		-no-shutdown

debug: all
	@echo ""
	@echo "  Launching WynlandOS in QEMU (debug mode)..."
	@echo "  Connect GDB: target remote localhost:1234"
	@echo ""
	qemu-system-x86_64                                    \
		-machine q35                                      \
		-cpu qemu64                                       \
		-m 1024M                                          \
		-bios $(OVMF_FW)                                  \
		-drive file=$(DISK_IMAGE),format=raw              \
		-device virtio-net-pci,netdev=net0                \
		-netdev user,id=net0                              \
		-serial stdio                                     \
		-no-reboot                                        \
		-no-shutdown                                      \
		-S -s                                             \
		-d int,cpu_reset

# ---------- Clean ----------

clean:
	@echo "  CLEAN      Removing build artifacts..."
	@rm -rf $(BUILD)
	@echo "  => Done"

# ---------- Tool check ----------

check-tools:
	@echo "Checking build tools..."
	@which $(CC_BOOT) > /dev/null 2>&1  || (echo "ERROR: $(CC_BOOT) not found. Run ./setup.sh" && exit 1)
	@which $(CC_KERNEL) > /dev/null 2>&1 || (echo "ERROR: $(CC_KERNEL) not found." && exit 1)
	@which $(AS) > /dev/null 2>&1       || (echo "ERROR: nasm not found. Run ./setup.sh" && exit 1)
	@which mformat > /dev/null 2>&1     || (echo "ERROR: mtools not found. Run ./setup.sh" && exit 1)
	@which mke2fs > /dev/null 2>&1      || (echo "ERROR: mke2fs (e2fsprogs) not found. Run ./setup.sh" && exit 1)
	@which debugfs > /dev/null 2>&1     || (echo "ERROR: debugfs (e2fsprogs) not found. Run ./setup.sh" && exit 1)
	@which python3 > /dev/null 2>&1     || (echo "ERROR: python3 not found." && exit 1)
	@test -f $(OVMF_FW)                 || (echo "ERROR: OVMF firmware not found at $(OVMF_FW). Run ./setup.sh" && exit 1)
	@echo "All tools found."

# Header dependencies (-MMD -MP): a struct change in a header used to leave
# every object that was not edited compiled against the old layout.
-include $(shell find $(BUILD) -name "*.d" 2>/dev/null)

# Default wallpaper (/wall.png), generated
$(BUILD)/wall.png: tools/gen_wallpaper.py
	@mkdir -p $(BUILD)
	@python3 tools/gen_wallpaper.py $@
