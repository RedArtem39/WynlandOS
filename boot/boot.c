/*
 * WynlandOS - UEFI Bootloader
 * Copyright (c) 2026 WynlandOS Project
 *
 * Phase 1 bootloader: initializes graphics, loads kernel ELF,
 * retrieves ACPI RSDP, exits boot services, and jumps to kernel.
 *
 * Compiled with: x86_64-w64-mingw32-gcc (MS ABI is default)
 */

#include "uefi.h"
#include "elf.h"
#include <wynland/boot_info.h>

/* ============================================================
 * Global State
 * ============================================================ */

static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_HANDLE         gImageHandle;

/* ============================================================
 * Helper: Print - output a UCS-2 string via ConOut
 * ============================================================ */

static void Print(CHAR16 *str) {
    gST->ConOut->OutputString(gST->ConOut, str);
}

/* ============================================================
 * Helper: PrintHex - print a 64-bit value in hexadecimal
 * ============================================================ */

static void PrintHex(UINT64 value) {
    CHAR16 buf[19]; /* "0x" + 16 hex digits + null */
    CHAR16 hexChars[] = L"0123456789ABCDEF";
    int i;

    buf[0] = L'0';
    buf[1] = L'x';
    for (i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hexChars[(value >> (i * 4)) & 0xF];
    }
    buf[18] = L'\0';

    Print(buf);
}

/* ============================================================
 * Helper: memset - freestanding, no library dependency
 * ============================================================ */

void* memset(void *dst, int val, UINTN size) {
    UINT8 *p = (UINT8*)dst;
    UINTN i;
    for (i = 0; i < size; i++) {
        p[i] = (UINT8)val;
    }
    return dst;
}

/* ============================================================
 * Helper: memcpy - freestanding, no library dependency
 * ============================================================ */

void* memcpy(void *dst, const void *src, UINTN size) {
    UINT8 *d = (UINT8*)dst;
    const UINT8 *s = (const UINT8*)src;
    UINTN i;
    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
    return dst;
}

/* ============================================================
 * Helper: CompareGuid - compare two EFI_GUIDs
 * Returns 0 if equal, non-zero otherwise
 * ============================================================ */

static int CompareGuid(EFI_GUID *a, EFI_GUID *b) {
    UINT8 *pa = (UINT8*)a;
    UINT8 *pb = (UINT8*)b;
    UINTN i;
    for (i = 0; i < sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return 1;
    }
    return 0;
}

/* ============================================================
 * InitGraphics - locate GOP protocol and set best video mode
 * ============================================================ */

static EFI_STATUS InitGraphics(BootInfo *info) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;
    UINT32 bestMode = 0;
    UINT32 bestWidth = 0;
    UINT32 bestHeight = 0;
    UINT32 modeNum;

    /* Locate the Graphics Output Protocol */
    status = gBS->LocateProtocol(&gopGuid, (VOID*)0, (VOID**)&gop);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not locate GOP\r\n");
        return status;
    }

    /* Enumerate all available modes and find a standard crisp resolution (prefer exactly 2560x1440 first, then 1920x1080, then 1280x720, then 1024x768) */
    UINT32 found2560 = 0;
    UINT32 mode2560 = 0;
    UINT32 found1920 = 0;
    UINT32 mode1920 = 0;
    UINT32 found1280 = 0;
    UINT32 mode1280 = 0;
    UINT32 found1024 = 0;
    UINT32 mode1024 = 0;

    for (modeNum = 0; modeNum < gop->Mode->MaxMode; modeNum++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *modeInfo;
        UINTN infoSize;

        status = gop->QueryMode(gop, modeNum, &infoSize, &modeInfo);
        if (EFI_IS_ERROR(status)) {
            continue;
        }

        /* PixelBltOnly modes are fine: a BLT-only GOP (OVMF's virtio-gpu)
           gets a RAM framebuffer below. Skipping them left no mode at all
           and fell back to mode 0 (640x480). */

        UINT32 w = modeInfo->HorizontalResolution;
        UINT32 h = modeInfo->VerticalResolution;

        if (w == 2560 && h == 1440) {
            found2560 = 1;
            mode2560 = modeNum;
        }
        if (w == 1920 && h == 1080) {
            found1920 = 1;
            mode1920 = modeNum;
        }
        if (w == 1280 && h == 720) {
            found1280 = 1;
            mode1280 = modeNum;
        }
        if (w == 1024 && h == 768) {
            found1024 = 1;
            mode1024 = modeNum;
        }

        /* Fallback: pick the largest mode that is <= 2560x1600 */
        if (w <= 2560 && h <= 1600) {
            if (w > bestWidth || (w == bestWidth && h > bestHeight)) {
                bestMode = modeNum;
                bestWidth = w;
                bestHeight = h;
            }
        }
    }

    if (found1024) {
        bestMode = mode1024;
        bestWidth = 1024;
        bestHeight = 768;
    } else if (found1280) {
        bestMode = mode1280;
        bestWidth = 1280;
        bestHeight = 720;
    } else if (found1920) {
        bestMode = mode1920;
        bestWidth = 1920;
        bestHeight = 1080;
    } else if (found2560) {
        bestMode = mode2560;
        bestWidth = 2560;
        bestHeight = 1440;
    }

    /* Fallback if no moderate resolution mode is found */
    if (bestWidth == 0) {
        bestMode = 0;
    }

    /* Set the best mode */
    status = gop->SetMode(gop, bestMode);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Failed to set GOP mode\r\n");
        return status;
    }

    /* Fill in the BootInfo framebuffer fields */
    info->fb_addr   = gop->Mode->FrameBufferBase;
    info->fb_width  = gop->Mode->Info->HorizontalResolution;
    info->fb_height = gop->Mode->Info->VerticalResolution;
    info->fb_pitch  = gop->Mode->Info->PixelsPerScanLine * 4; /* 4 bytes per pixel (32bpp) */
    info->fb_bpp    = 32;

    if (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        info->fb_format = 1; /* BGR */
    } else {
        info->fb_format = 0; /* RGB */
    }

    /* A BLT-only GOP (OVMF's virtio-gpu driver, e.g. QEMU with -vga none)
       has no linear framebuffer: FrameBufferBase is 0. The kernel would
       then identity-map "the framebuffer" over physical 0.. -- right over
       its own code at 1 MB, with NX -- and die on the CR3 switch. Give it
       a RAM framebuffer instead (EfiReservedMemoryType: the kernel's page
       allocator never hands it out). Nothing scans it out until the
       virtio-gpu driver takes over the display. */
    if (gop->Mode->Info->PixelFormat == PixelBltOnly || gop->Mode->FrameBufferBase == 0) {
        UINTN fbPages = (UINTN)(((UINT64)info->fb_pitch * info->fb_height + 4095) / 4096);
        EFI_PHYSICAL_ADDRESS fbAddr = 0;
        status = gBS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, fbPages, &fbAddr);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: BLT-only GOP and no memory for a framebuffer\r\n");
            return status;
        }
        UINT8 *p = (UINT8 *)(UINTN)fbAddr;
        for (UINTN k = 0; k < fbPages * 4096; k++) p[k] = 0;
        info->fb_addr = fbAddr;
        info->fb_format = 1; /* BGR, like every virtio-gpu 2D format we use */
        Print(L"  GOP is BLT-only: using a RAM framebuffer\r\n");
    }

    /* Print selected resolution */
    Print(L"  Graphics: ");
    PrintHex(info->fb_width);
    Print(L" x ");
    PrintHex(info->fb_height);
    Print(L"\r\n");

    return EFI_SUCCESS;
}

/* ============================================================
 * LoadKernel - load kernel.elf from the boot volume
 * ============================================================ */

static EFI_STATUS LoadKernel(Elf64_Addr *entry_point) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem;
    EFI_FILE_PROTOCOL *rootDir;
    EFI_FILE_PROTOCOL *kernelFile;
    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fileInfoGuid = EFI_FILE_INFO_GUID;
    UINT8 *fileBuffer;
    UINTN fileSize;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINT16 i;

    /* Get EFI_LOADED_IMAGE_PROTOCOL from our image handle */
    status = gBS->HandleProtocol(gImageHandle, &loadedImageGuid, (VOID**)&loadedImage);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not get LoadedImage protocol\r\n");
        return status;
    }

    /* Get EFI_SIMPLE_FILE_SYSTEM_PROTOCOL from the device we booted from */
    status = gBS->HandleProtocol(loadedImage->DeviceHandle, &fileSystemGuid, (VOID**)&fileSystem);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not get FileSystem protocol\r\n");
        return status;
    }

    /* Open the root directory of the boot volume */
    status = fileSystem->OpenVolume(fileSystem, &rootDir);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not open root volume\r\n");
        return status;
    }

    /* Open kernel.elf */
    status = rootDir->Open(rootDir, &kernelFile, L"\\kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not open \\kernel.elf\r\n");
        rootDir->Close(rootDir);
        return status;
    }

    /* Get file size using GetInfo */
    {
        UINT8 infoBuffer[256];
        UINTN infoSize = sizeof(infoBuffer);
        EFI_FILE_INFO *finfo;

        status = kernelFile->GetInfo(kernelFile, &fileInfoGuid, &infoSize, infoBuffer);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: Could not get kernel file info\r\n");
            kernelFile->Close(kernelFile);
            rootDir->Close(rootDir);
            return status;
        }

        finfo = (EFI_FILE_INFO*)infoBuffer;
        fileSize = (UINTN)finfo->FileSize;
    }

    Print(L"  Kernel file size: ");
    PrintHex(fileSize);
    Print(L" bytes\r\n");

    /* Allocate buffer and read the entire file */
    status = gBS->AllocatePool(EfiLoaderData, fileSize, (VOID**)&fileBuffer);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not allocate memory for kernel\r\n");
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return status;
    }

    {
        UINTN readSize = fileSize;
        status = kernelFile->Read(kernelFile, &readSize, fileBuffer);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: Could not read kernel file\r\n");
            gBS->FreePool(fileBuffer);
            kernelFile->Close(kernelFile);
            rootDir->Close(rootDir);
            return status;
        }
    }

    /* Parse ELF header */
    ehdr = (Elf64_Ehdr*)fileBuffer;

    /* Verify ELF magic */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        Print(L"  ERROR: Invalid ELF magic\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    /* Verify ELF class (64-bit) */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        Print(L"  ERROR: Not a 64-bit ELF\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    /* Verify machine type (x86_64) */
    if (ehdr->e_machine != EM_X86_64) {
        Print(L"  ERROR: Not an x86_64 ELF\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    /* Verify type (executable) */
    if (ehdr->e_type != ET_EXEC) {
        Print(L"  ERROR: ELF is not an executable\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    Print(L"  ELF header valid, loading segments...\r\n");

    /* Load each PT_LOAD segment */
    phdr = (Elf64_Phdr*)(fileBuffer + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            UINTN numPages = (UINTN)((phdr[i].p_memsz + 0xFFF) / 0x1000);
            EFI_PHYSICAL_ADDRESS segAddr = phdr[i].p_paddr;

            /* Allocate pages at the exact physical address */
            status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, numPages, &segAddr);
            if (EFI_IS_ERROR(status)) {
                Print(L"  ERROR: Could not allocate pages for segment at ");
                PrintHex(phdr[i].p_paddr);
                Print(L"\r\n");
                gBS->FreePool(fileBuffer);
                kernelFile->Close(kernelFile);
                rootDir->Close(rootDir);
                return status;
            }

            /* Copy segment data from file buffer to physical memory */
            memcpy((VOID*)segAddr, fileBuffer + phdr[i].p_offset, (UINTN)phdr[i].p_filesz);

            /* Zero out BSS (p_memsz > p_filesz) */
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset((VOID*)(segAddr + phdr[i].p_filesz), 0,
                       (UINTN)(phdr[i].p_memsz - phdr[i].p_filesz));
            }

            Print(L"  Loaded segment: ");
            PrintHex(segAddr);
            Print(L" (");
            PrintHex(phdr[i].p_memsz);
            Print(L" bytes)\r\n");
        }
    }

    /* Set the kernel entry point */
    *entry_point = ehdr->e_entry;
    Print(L"  Kernel entry point: ");
    PrintHex(*entry_point);
    Print(L"\r\n");

    /* Cleanup */
    kernelFile->Close(kernelFile);
    rootDir->Close(rootDir);
    gBS->FreePool(fileBuffer);

    Print(L"  Kernel loaded successfully!\r\n");
    return EFI_SUCCESS;
}

/* ============================================================
 * FindRSDP - locate the ACPI RSDP in the configuration tables
 * ============================================================ */

static VOID* FindRSDP(void) {
    EFI_GUID acpi20Guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10Guid = EFI_ACPI_TABLE_GUID;
    UINTN i;

    /* First pass: look for ACPI 2.0+ table (preferred) */
    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &acpi20Guid) == 0) {
            Print(L"  Found ACPI 2.0 RSDP at ");
            PrintHex((UINT64)gST->ConfigurationTable[i].VendorTable);
            Print(L"\r\n");
            return gST->ConfigurationTable[i].VendorTable;
        }
    }

    /* Second pass: fall back to ACPI 1.0 table */
    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &acpi10Guid) == 0) {
            Print(L"  Found ACPI 1.0 RSDP at ");
            PrintHex((UINT64)gST->ConfigurationTable[i].VendorTable);
            Print(L"\r\n");
            return gST->ConfigurationTable[i].VendorTable;
        }
    }

    Print(L"  WARNING: ACPI RSDP not found!\r\n");
    return (VOID*)0;
}

/* ============================================================
 * ExitBootServicesAndGetMemoryMap
 * Retrieves the UEFI memory map, exits boot services, and
 * converts the map to WynlandOS MemoryRegion format.
 * ============================================================ */

static EFI_STATUS ExitBootServicesAndGetMemoryMap(BootInfo *info) {
    EFI_STATUS status;
    UINTN mmapSize = 8192; /* Start with 8KB buffer */
    UINTN mapKey;
    UINTN descSize;
    UINT32 descVersion;
    UINT8 *mmapBuffer;
    MemoryRegion *regions;
    UINTN regionCount;
    UINT64 totalUsable;
    UINTN numEntries;
    UINTN i;

    /* Allocate the memory map buffer */
    status = gBS->AllocatePool(EfiLoaderData, mmapSize, (VOID**)&mmapBuffer);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not allocate memory map buffer\r\n");
        return status;
    }

    /* Get the memory map - may need to retry with larger buffer */
    status = gBS->GetMemoryMap(&mmapSize, (EFI_MEMORY_DESCRIPTOR*)mmapBuffer,
                               &mapKey, &descSize, &descVersion);
    if (status == EFI_BUFFER_TOO_SMALL) {
        gBS->FreePool(mmapBuffer);
        /* Add extra space for map changes caused by our allocation */
        mmapSize += 2 * descSize;
        status = gBS->AllocatePool(EfiLoaderData, mmapSize, (VOID**)&mmapBuffer);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: Could not allocate larger memory map buffer\r\n");
            return status;
        }
        status = gBS->GetMemoryMap(&mmapSize, (EFI_MEMORY_DESCRIPTOR*)mmapBuffer,
                                   &mapKey, &descSize, &descVersion);
    }

    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: GetMemoryMap failed\r\n");
        gBS->FreePool(mmapBuffer);
        return status;
    }

    /* Try to exit boot services - retry if map key changed */
    status = gBS->ExitBootServices(gImageHandle, mapKey);
    if (EFI_IS_ERROR(status)) {
        /* Map key changed, retry once */
        mmapSize = 8192 + 4096; /* Use a larger buffer */
        /* Note: we cannot call AllocatePool after a failed ExitBootServices reliably,
           so we reuse the existing buffer if it's large enough, or retry with the
           existing allocation */
        status = gBS->GetMemoryMap(&mmapSize, (EFI_MEMORY_DESCRIPTOR*)mmapBuffer,
                                   &mapKey, &descSize, &descVersion);
        if (EFI_IS_ERROR(status)) {
            return status;
        }
        status = gBS->ExitBootServices(gImageHandle, mapKey);
        if (EFI_IS_ERROR(status)) {
            return status;
        }
    }

    /*
     * === BOOT SERVICES ARE NOW TERMINATED ===
     * We can no longer call any Boot Services functions.
     * Only the pre-allocated static buffers are available.
     */

    /* Convert EFI memory map to WynlandOS MemoryRegion format */
    regions = (MemoryRegion*)info->mmap_addr;
    regionCount = 0;
    totalUsable = 0;
    numEntries = mmapSize / descSize;

    for (i = 0; i < numEntries && regionCount < 512; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR*)(mmapBuffer + i * descSize);
        UINT32 type;
        UINT64 regionSize = desc->NumberOfPages * 4096;

        /* Map EFI memory types to WynlandOS memory types */
        switch (desc->Type) {
            case EfiConventionalMemory:
                type = MEMORY_USABLE;
                totalUsable += regionSize;
                break;
            case EfiACPIReclaimMemory:
                type = MEMORY_ACPI_RECLAIMABLE;
                break;
            case EfiACPINVS:
                type = MEMORY_ACPI_NVS;
                break;
            case EfiLoaderCode:
            case EfiLoaderData:
                type = MEMORY_BOOTLOADER;
                break;
            case EfiBootServicesCode:
            case EfiBootServicesData:
                /* Boot services memory is reclaimable after ExitBootServices */
                type = MEMORY_USABLE;
                totalUsable += regionSize;
                break;
            default:
                type = MEMORY_RESERVED;
                break;
        }

        regions[regionCount].base        = desc->PhysicalStart;
        regions[regionCount].size        = regionSize;
        regions[regionCount].type        = type;
        regions[regionCount].reserved    = 0;
        regionCount++;
    }

    info->mmap_entries = (UINT32)regionCount;
    info->total_memory = totalUsable;

    return EFI_SUCCESS;
}

/* ============================================================
 * EfiMain - UEFI application entry point
 * ============================================================ */

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    Elf64_Addr kernelEntry;

    /* Store global references */
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImageHandle = ImageHandle;

    /* Disable the 5-minute watchdog timer */
    gBS->SetWatchdogTimer(0, 0, 0, (CHAR16*)0);

    /* Clear screen */
    gST->ConOut->ClearScreen(gST->ConOut);

    Print(L"WynlandOS UEFI Bootloader v0.1\r\n");
    Print(L"================================\r\n\r\n");

    /*
     * Static BootInfo and MemoryRegion buffer.
     * Using static storage ensures they persist after ExitBootServices,
     * as they reside in the loader's data segment.
     */
    static BootInfo bootInfo;
    static MemoryRegion memRegions[512];

    memset(&bootInfo, 0, sizeof(BootInfo));
    bootInfo.mmap_addr = (UINT64)(UINTN)memRegions;

    /* [1/4] Initialize graphics */
    Print(L"[1/4] Initializing graphics...\r\n");
    status = InitGraphics(&bootInfo);
    if (EFI_IS_ERROR(status)) {
        Print(L"ERROR: Failed to initialize graphics!\r\n");
        while(1);
    }

    /* [2/4] Load kernel */
    Print(L"[2/4] Loading kernel...\r\n");
    status = LoadKernel(&kernelEntry);
    if (EFI_IS_ERROR(status)) {
        Print(L"ERROR: Failed to load kernel!\r\n");
        while(1);
    }

    /* [3/4] Find ACPI RSDP */
    Print(L"[3/4] Finding ACPI tables...\r\n");
    bootInfo.rsdp_addr = (UINT64)(UINTN)FindRSDP();

    /* [4/4] Get memory map and exit boot services */
    Print(L"[4/4] Exiting boot services...\r\n");
    status = ExitBootServicesAndGetMemoryMap(&bootInfo);
    if (EFI_IS_ERROR(status)) {
        /* Cannot print anymore - boot services may be partially exited */
        while(1);
    }

    /* Set the boot protocol magic number */
    bootInfo.magic = WYNLAND_BOOT_MAGIC;

    /*
     * Jump to the kernel!
     *
     * The bootloader is compiled with mingw (MS ABI), so the first
     * argument (BootInfo*) is passed in RCX. The kernel entry stub
     * (entry.asm) expects the BootInfo pointer in RCX.
     */
    typedef void (*KernelEntryFunc)(BootInfo*);
    KernelEntryFunc kernel = (KernelEntryFunc)(UINTN)kernelEntry;
    kernel(&bootInfo);

    /* Should never reach here */
    while(1);
    return EFI_SUCCESS;
}
