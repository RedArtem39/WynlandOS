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
CFLAGS_KERNEL  = $(COMMON_FLAGS)          \
                 -mcmodel=large           \
                 -fno-pie                 \
                 -fno-pic                 \
                 -fno-jump-tables         \
                 -nostdlib                \
                 -nostdinc                \
                 -std=c11                 \
                 -O2

CXXFLAGS_KERNEL = $(COMMON_FLAGS)         \
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

ASFLAGS        = -f elf64 -g

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
                 $(QUICKJS_DIR)/compat.c \
                 $(QUICKJS_DIR)/quickjs_binding.c
QUICKJS_OBJ    = $(patsubst $(QUICKJS_DIR)/%.c, $(BUILD_GUI)/quickjs_%.o, $(QUICKJS_SRC))

# All kernel-side objects
KERNEL_ALL_OBJ = $(KERNEL_ASM_OBJ) $(KERNEL_C_OBJ) $(KERNEL_CPP_OBJ) $(DRIVER_C_OBJ) $(DRIVER_ASM_OBJ) $(LIB_OBJ) $(PKG_OBJ) $(GUI_OBJ) $(GUI_CPP_OBJ) $(QUICKJS_OBJ)

# ============================================================================
# Output Files
# ============================================================================

BOOTLOADER_EFI = $(BUILD)/BOOTX64.EFI
KERNEL_ELF     = $(BUILD)/kernel.elf
DISK_IMAGE     = $(BUILD)/wynland.img
OVMF_FW        = tools/ovmf/OVMF.fd

# ============================================================================
# Targets
# ============================================================================

.PHONY: all bootloader kernel image run debug clean dirs check-tools

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

$(BUILD_KERNEL)/%.o: $(SRC_KERNEL)/%.asm
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

$(BUILD_GUI)/quickjs_%.o: $(QUICKJS_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC(QJS)    $<"
	@$(CC_KERNEL) $(CFLAGS_KERNEL) -Ipkg/quickjs/compat/include -Ipkg/quickjs -include pkg/quickjs/compat.h -DCONFIG_VERSION=\"2024-01-13\" -DCONFIG_BIGNUM=0 -Wno-unused-parameter -Wno-unused-variable -Wno-implicit-fallthrough -Wno-return-type -Wno-sign-compare -Wno-unused-function -Wno-maybe-uninitialized -Wno-unused-but-set-variable -Wno-format -Wno-int-conversion -c $< -o $@

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

# Real TTF font for QFreeTypeFontDatabase (vendored inside qtbase's own 3rdparty tree)
$(BUILD)/lib/fonts/DejaVuSans.ttf: qtbase/src/3rdparty/wasm/DejaVuSans.ttf
	@mkdir -p $(BUILD)/lib/fonts
	@echo "  CP         $< -> $@"
	@cp $< $@

# ---------- Disk Image ----------

image: bootloader kernel $(BUILD)/test.bin $(BUILD)/test_cpp.bin $(BUILD)/test_dev.bin $(BUILD)/test_qt.bin $(BUILD)/t_clone.bin $(BUILD)/interp.elf $(BUILD)/main_dynamic.elf $(BUILD)/test_raw_fb.elf $(BUILD)/lib/fonts/DejaVuSans.ttf $(DISK_IMAGE)

$(DISK_IMAGE): $(BOOTLOADER_EFI) $(KERNEL_ELF) $(BUILD)/test.bin $(BUILD)/test_cpp.bin $(BUILD)/test_dev.bin $(BUILD)/test_qt.bin $(BUILD)/t_clone.bin $(BUILD)/interp.elf $(BUILD)/main_dynamic.elf $(BUILD)/test_raw_fb.elf $(BUILD)/lib/fonts/DejaVuSans.ttf
	@echo "  IMG        Creating FAT32 disk image..."
	@dd if=/dev/zero of=$@ bs=1M count=1024 status=none
	@mformat -i $@ -F -v WYNLAND ::
	@mmd -i $@ ::/EFI
	@mmd -i $@ ::/EFI/BOOT
	@mmd -i $@ ::/lib
	@mmd -i $@ ::/lib/platforms
	-@mmd -i $@ ::/lib/fonts
	-@mmd -i $@ ::/plugins
	-@mmd -i $@ ::/plugins/platforms
	-@mmd -i $@ ::/tmp
	-@mmd -i $@ ::/etc
	-@mmd -i $@ ::/etc/hypr
	@mcopy -o -i $@ $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -o -i $@ $(KERNEL_ELF) ::/kernel.elf
	@mcopy -o -i $@ $(BUILD)/test.bin ::/test.bin
	@mcopy -o -i $@ $(BUILD)/test_cpp.bin ::/test_cpp.bin
	@mcopy -o -i $@ $(BUILD)/test_dev.bin ::/test_dev.bin
	@mcopy -o -i $@ $(BUILD)/test_qt.bin ::/test_qt.bin
	@mcopy -o -i $@ $(BUILD)/t_clone.bin ::/t_clone.bin
	@mcopy -o -i $@ $(BUILD)/test.elf ::/test.elf
	@mcopy -o -i $@ $(BUILD)/test_cpp.elf ::/test_cpp.elf
	@mcopy -o -i $@ $(BUILD)/test_dev.elf ::/test_dev.elf
	@mcopy -o -i $@ $(BUILD)/test_qt.elf ::/test_qt.elf
	@mcopy -o -i $@ $(BUILD)/t_clone.elf ::/t_clone.elf
	-@mcopy -o -i $@ $(BUILD)/test_qt_real.elf ::/qtreal.elf
	@mcopy -o -i $@ $(BUILD)/test_raw_fb.elf ::/test_raw_fb.elf
	@mcopy -o -i $@ tools/x86_64-linux-musl-cross/x86_64-linux-musl/lib/libc.so ::/lib/ld-musl-x86_64.so.1
	@mcopy -o -i $@ tools/x86_64-linux-musl-cross/x86_64-linux-musl/lib/libstdc++.so.6.0.29 ::/lib/libstdc++.so.6
	@mcopy -o -i $@ tools/x86_64-linux-musl-cross/x86_64-linux-musl/lib/libstdc++.so.6.0.29 ::/lib/libstdc++.so.6.0.29
	@mcopy -o -i $@ tools/x86_64-linux-musl-cross/x86_64-linux-musl/lib/libgcc_s.so.1 ::/lib/libgcc_s.so.1
	-@mcopy -o -i $@ build/lib/libQt6Core.so.6.5.2 ::/lib/libQt6Core.so.6
	-@mcopy -o -i $@ build/lib/libQt6Core.so.6.5.2 ::/lib/libQt6Core.so.6.5.2
	-@mcopy -o -i $@ build/lib/libQt6Gui.so.6.5.2 ::/lib/libQt6Gui.so.6
	-@mcopy -o -i $@ build/lib/libQt6Gui.so.6.5.2 ::/lib/libQt6Gui.so.6.5.2
	-@mcopy -o -i $@ build/lib/libQt6DBus.so.6.5.2 ::/lib/libQt6DBus.so.6
	-@mcopy -o -i $@ build/lib/libQt6DBus.so.6.5.2 ::/lib/libQt6DBus.so.6.5.2
	-@mcopy -o -i $@ build/lib/libQt6Widgets.so.6.5.2 ::/lib/libQt6Widgets.so.6
	-@mcopy -o -i $@ build/lib/libQt6Widgets.so.6.5.2 ::/lib/libQt6Widgets.so.6.5.2
	-@mcopy -o -i $@ build/plugins/platforms/libqwynlandfb.so ::/lib/platforms/libqwynlandfb.so
	-@mcopy -o -i $@ build/plugins/platforms/libqwynlandfb.so ::/plugins/platforms/libqwynlandfb.so
	-@mcopy -o -i $@ build/lib/fonts/DejaVuSans.ttf ::/lib/fonts/DejaVuSans.ttf
	@mcopy -o -i $@ $(BUILD)/interp.elf ::/lib/ld-dummy.so
	@mcopy -o -i $@ $(BUILD)/main_dynamic.elf ::/t_dyn.elf
	@mcopy -o -i $@ app.wasm ::/app.was
	@mcopy -o -i $@ hello.wyn ::/hello.wyn
	@mcopy -o -i $@ hyprland.conf ::/hyprland.conf
	-@mcopy -o -i $@ external/nixos-configuration/config/sessions/hyprland/hyprland.conf ::/nixos_hyprland.conf
	-@mcopy -o -i $@ external/nixos-configuration/config/sessions/hyprland/config/settings.conf ::/nixos_settings.conf
	@mcopy -o -i $@ wynui.wyn ::/wynui.wyn
	@mcopy -o -i $@ node.wyn ::/node.wyn
	@mcopy -o -i $@ script.js ::/script.js
	@mmd -i $@ ::/lib64
	@mcopy -o -i $@ hyprland_stripped.wyn ::/hyprland.wyn
	@mcopy -o -i $@ /lib64/ld-linux-x86-64.so.2 ::/lib64/ld-linux-x86-64.so.2
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libc.so.6 ::/lib64/libc.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libm.so.6 ::/lib64/libm.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libgcc_s.so.1 ::/lib64/libgcc_s.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libstdc++.so.6 ::/lib64/libstdc++.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libEGL.so.1 ::/lib64/libEGL.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libGLESv2.so.2 ::/lib64/libGLESv2.so.2
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libdrm.so.2 ::/lib64/libdrm.so.2
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libgbm.so.1 ::/lib64/libgbm.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libGLdispatch.so.0 ::/lib64/libGLdispatch.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libexpat.so.1 ::/lib64/libexpat.so.1
	-@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libwayland-server.so.0 ::/lib64/libwayland-server.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxcb-present.so.0 ::/lib64/libxcb-present.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libdrm_amdgpu.so.1 ::/lib64/libdrm_amdgpu.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libX11-xcb.so.1 ::/lib64/libX11-xcb.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libgbm.so.1 ::/lib64/libgbm.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxcb.so.1 ::/lib64/libxcb.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libgallium-26.0.3-1ubuntu1.so ::/lib64/libgallium-26.0.3-1ubuntu1.so
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxshmfence.so.1 ::/lib64/libxshmfence.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libelf.so.1 ::/lib64/libelf.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libc.so.6 ::/lib64/libc.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libbsd.so.0 ::/lib64/libbsd.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libzstd.so.1 ::/lib64/libzstd.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxcb-sync.so.1 ::/lib64/libxcb-sync.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libgcc_s.so.1 ::/lib64/libgcc_s.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libedit.so.2 ::/lib64/libedit.so.2
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libXau.so.6 ::/lib64/libXau.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libdrm.so.2 ::/lib64/libdrm.so.2
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libLLVM.so.21.1 ::/lib64/libLLVM.so.21.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxcb-dri3.so.0 ::/lib64/libxcb-dri3.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxcb-randr.so.0 ::/lib64/libxcb-randr.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libffi.so.8 ::/lib64/libffi.so.8
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxcb-xfixes.so.0 ::/lib64/libxcb-xfixes.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libstdc++.so.6 ::/lib64/libstdc++.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libm.so.6 ::/lib64/libm.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libtinfo.so.6 ::/lib64/libtinfo.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libGLESv2.so.2 ::/lib64/libGLESv2.so.2
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libXdmcp.so.6 ::/lib64/libXdmcp.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libsensors.so.5 ::/lib64/libsensors.so.5
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libwayland-client.so.0 ::/lib64/libwayland-client.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libEGL_mesa.so.0 ::/lib64/libEGL_mesa.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libexpat.so.1 ::/lib64/libexpat.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libpciaccess.so.0 ::/lib64/libpciaccess.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libX11.so.6 ::/lib64/libX11.so.6
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libdrm_intel.so.1 ::/lib64/libdrm_intel.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxml2.so.16 ::/lib64/libxml2.so.16
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libz.so.1 ::/lib64/libz.so.1
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libGLdispatch.so.0 ::/lib64/libGLdispatch.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libmd.so.0 ::/lib64/libmd.so.0
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/libxcb-shm.so.0 ::/lib64/libxcb-shm.so.0
	-@mmd -i $@ ::/dev
	-@mmd -i $@ ::/dev/dri
	-@echo "mock" > build/card0
	-@echo "mock" > build/renderD128
	@mcopy -o -i $@ build/card0 ::/dev/dri/card0
	@mcopy -o -i $@ build/renderD128 ::/dev/dri/renderD128
	-@mmd -i $@ ::/lib64/dri
	@mcopy -o -i $@ /usr/lib/x86_64-linux-gnu/dri/swrast_dri.so ::/lib64/dri/swrast_dri.so
	-@mmd -i $@ ::/etc/glvnd
	-@mmd -i $@ ::/etc/glvnd/egl_vendor.d
	@mcopy -o -i $@ /usr/share/glvnd/egl_vendor.d/50_mesa.json ::/etc/glvnd/egl_vendor.d/50_mesa.json
	-@mmd -i $@ ::/usr
	-@mmd -i $@ ::/usr/share
	-@mmd -i $@ ::/usr/share/glvnd
	-@mmd -i $@ ::/usr/share/glvnd/egl_vendor.d
	@mcopy -o -i $@ /usr/share/glvnd/egl_vendor.d/50_mesa.json ::/usr/share/glvnd/egl_vendor.d/50_mesa.json
	@echo "  => wynland.img created (1024 MB)"

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
	@test -f $(OVMF_FW)                 || (echo "ERROR: OVMF firmware not found at $(OVMF_FW). Run ./setup.sh" && exit 1)
	@echo "All tools found."
