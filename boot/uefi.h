/*
 * WynlandOS - UEFI Header Definitions
 * Copyright (c) 2026 WynlandOS Project
 *
 * Minimal UEFI headers written from scratch (no gnu-efi dependency).
 * Compiled with x86_64-w64-mingw32-gcc, so MS ABI is the default.
 */

#pragma once

/* ============================================================
 * 1. Basic UEFI Types
 * ============================================================ */

typedef unsigned long long  UINTN;
typedef signed long long    INTN;

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;

typedef signed char         INT8;
typedef signed short        INT16;
typedef signed int          INT32;
typedef signed long long    INT64;

typedef unsigned short      CHAR16;
typedef unsigned char       BOOLEAN;

typedef void                VOID;

typedef UINTN               EFI_STATUS;
typedef VOID*               EFI_HANDLE;
typedef VOID*               EFI_EVENT;
typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;
typedef UINTN               EFI_TPL;

#define TRUE    1
#define FALSE   0

#define IN
#define OUT
#define OPTIONAL

#define EFIAPI

/* ============================================================
 * 2. EFI_GUID
 * ============================================================ */

typedef struct {
    UINT32  Data1;
    UINT16  Data2;
    UINT16  Data3;
    UINT8   Data4[8];
} EFI_GUID;

/* ============================================================
 * 3. EFI_STATUS Codes
 * ============================================================ */

#define EFI_SUCCESS             0
#define EFI_ERROR_MASK          0x8000000000000000ULL
#define EFI_ERR(x)              (EFI_ERROR_MASK | (x))

#define EFI_LOAD_ERROR          EFI_ERR(1)
#define EFI_INVALID_PARAMETER   EFI_ERR(2)
#define EFI_UNSUPPORTED         EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL    EFI_ERR(5)
#define EFI_NOT_FOUND           EFI_ERR(14)

#define EFI_IS_ERROR(x)         ((x) & EFI_ERROR_MASK)

/* ============================================================
 * 4. EFI_TABLE_HEADER (24 bytes)
 * ============================================================ */

typedef struct {
    UINT64  Signature;
    UINT32  Revision;
    UINT32  HeaderSize;
    UINT32  CRC32;
    UINT32  Reserved;
} EFI_TABLE_HEADER;

/* ============================================================
 * 5. Forward Declarations
 * ============================================================ */

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL   EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL      EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL                 EFI_FILE_PROTOCOL;
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct EFI_LOADED_IMAGE_PROTOCOL         EFI_LOADED_IMAGE_PROTOCOL;
typedef struct EFI_BOOT_SERVICES                 EFI_BOOT_SERVICES;
typedef struct EFI_SYSTEM_TABLE                  EFI_SYSTEM_TABLE;
typedef struct EFI_CONFIGURATION_TABLE           EFI_CONFIGURATION_TABLE;

/* ============================================================
 * 6. EFI_MEMORY_TYPE
 * ============================================================ */

typedef enum {
    EfiReservedMemoryType       = 0,
    EfiLoaderCode               = 1,
    EfiLoaderData               = 2,
    EfiBootServicesCode         = 3,
    EfiBootServicesData         = 4,
    EfiRuntimeServicesCode      = 5,
    EfiRuntimeServicesData      = 6,
    EfiConventionalMemory       = 7,
    EfiUnusableMemory           = 8,
    EfiACPIReclaimMemory        = 9,
    EfiACPINVS                  = 10,
    EfiMemoryMappedIO           = 11,
    EfiMemoryMappedIOPortSpace  = 12,
    EfiPalCode                  = 13,
    EfiPersistentMemory         = 14,
    EfiMaxMemoryType            = 15
} EFI_MEMORY_TYPE;

/* ============================================================
 * 7. EFI_ALLOCATE_TYPE
 * ============================================================ */

typedef enum {
    AllocateAnyPages    = 0,
    AllocateMaxAddress  = 1,
    AllocateAddress     = 2
} EFI_ALLOCATE_TYPE;

/* ============================================================
 * 8. EFI_MEMORY_DESCRIPTOR
 * ============================================================ */

typedef struct {
    UINT32                  Type;
    /* 4 bytes implicit padding here for alignment */
    EFI_PHYSICAL_ADDRESS    PhysicalStart;
    EFI_VIRTUAL_ADDRESS     VirtualStart;
    UINT64                  NumberOfPages;
    UINT64                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ============================================================
 * 9. EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
 * ============================================================ */

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID*       Reset;
    EFI_STATUS  (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    VOID*       TestString;
    VOID*       QueryMode;
    VOID*       SetMode;
    VOID*       SetAttribute;
    EFI_STATUS  (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    VOID*       SetCursorPosition;
    VOID*       EnableCursor;
    VOID*       Mode;
};

/* ============================================================
 * 10. EFI_GRAPHICS_PIXEL_FORMAT
 * ============================================================ */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor   = 0,
    PixelBlueGreenRedReserved8BitPerColor   = 1,
    PixelBitMask                            = 2,
    PixelBltOnly                            = 3
} EFI_GRAPHICS_PIXEL_FORMAT;

/* ============================================================
 * 11. EFI_PIXEL_BITMASK
 * ============================================================ */

typedef struct {
    UINT32  RedMask;
    UINT32  GreenMask;
    UINT32  BlueMask;
    UINT32  ReservedMask;
} EFI_PIXEL_BITMASK;

/* ============================================================
 * 12. EFI_GRAPHICS_OUTPUT_MODE_INFORMATION
 * ============================================================ */

typedef struct {
    UINT32                      Version;
    UINT32                      HorizontalResolution;
    UINT32                      VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT   PixelFormat;
    EFI_PIXEL_BITMASK           PixelInformation;
    UINT32                      PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* ============================================================
 * 13. EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE
 * ============================================================ */

typedef struct {
    UINT32                                  MaxMode;
    UINT32                                  Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION    *Info;
    UINTN                                   SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                    FrameBufferBase;
    UINTN                                   FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* ============================================================
 * 14. EFI_GRAPHICS_OUTPUT_PROTOCOL
 * ============================================================ */

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS  (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                             UINT32 ModeNumber,
                             UINTN *SizeOfInfo,
                             EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS  (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                           UINT32 ModeNumber);
    VOID        *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* ============================================================
 * 15. EFI_FILE_PROTOCOL
 * ============================================================ */

struct EFI_FILE_PROTOCOL {
    UINT64      Revision;
    EFI_STATUS  (*Open)(struct EFI_FILE_PROTOCOL *This,
                        struct EFI_FILE_PROTOCOL **NewHandle,
                        CHAR16 *FileName,
                        UINT64 OpenMode,
                        UINT64 Attributes);
    EFI_STATUS  (*Close)(struct EFI_FILE_PROTOCOL *This);
    VOID*       Delete;
    EFI_STATUS  (*Read)(struct EFI_FILE_PROTOCOL *This,
                        UINTN *BufferSize,
                        VOID *Buffer);
    VOID*       Write;
    VOID*       GetPosition;
    VOID*       SetPosition;
    EFI_STATUS  (*GetInfo)(struct EFI_FILE_PROTOCOL *This,
                           EFI_GUID *InformationType,
                           UINTN *BufferSize,
                           VOID *Buffer);
    VOID*       SetInfo;
    VOID*       Flush;
};

#define EFI_FILE_MODE_READ      0x0000000000000001ULL
#define EFI_FILE_READ_ONLY      0x0000000000000001ULL

/* ============================================================
 * 16. EFI_FILE_INFO
 * ============================================================ */

typedef struct {
    UINT64  Size;
    UINT64  FileSize;
    UINT64  PhysicalSize;
    /* Remaining fields omitted for simplicity */
} EFI_FILE_INFO;

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* ============================================================
 * 17. EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
 * ============================================================ */

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64      Revision;
    EFI_STATUS  (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                              EFI_FILE_PROTOCOL **Root);
};

/* ============================================================
 * 18. EFI_LOADED_IMAGE_PROTOCOL
 * ============================================================ */

struct EFI_LOADED_IMAGE_PROTOCOL {
    UINT32          Revision;
    UINT32          _pad;
    EFI_HANDLE      ParentHandle;
    VOID            *SystemTable;
    EFI_HANDLE      DeviceHandle;
    VOID            *FilePath;
    VOID            *Reserved;
    UINT32          LoadOptionsSize;
    UINT32          _pad2;
    VOID            *LoadOptions;
    VOID            *ImageBase;
    UINT64          ImageSize;
    UINTN           ImageCodeType;
    UINTN           ImageDataType;
    VOID*           Unload;
};

/* ============================================================
 * 19. EFI_CONFIGURATION_TABLE
 * ============================================================ */

struct EFI_CONFIGURATION_TABLE {
    EFI_GUID    VendorGuid;
    VOID        *VendorTable;
};

/* ============================================================
 * 20. EFI_BOOT_SERVICES
 * ============================================================ */

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;

    /* TPL Services */
    VOID*   RaiseTPL;
    VOID*   RestoreTPL;

    /* Memory Services */
    EFI_STATUS  (*AllocatePages)(UINTN Type,
                                 UINTN MemoryType,
                                 UINTN Pages,
                                 EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS  (*FreePages)(EFI_PHYSICAL_ADDRESS Memory,
                             UINTN Pages);
    EFI_STATUS  (*GetMemoryMap)(UINTN *MemoryMapSize,
                                EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                UINTN *MapKey,
                                UINTN *DescriptorSize,
                                UINT32 *DescriptorVersion);
    EFI_STATUS  (*AllocatePool)(UINTN PoolType,
                                UINTN Size,
                                VOID **Buffer);
    EFI_STATUS  (*FreePool)(VOID *Buffer);

    /* Event & Timer Services */
    VOID*   CreateEvent;
    VOID*   SetTimer;
    VOID*   WaitForEvent;
    VOID*   SignalEvent;
    VOID*   CloseEvent;
    VOID*   CheckEvent;

    /* Protocol Handler Services */
    VOID*   InstallProtocolInterface;
    VOID*   ReinstallProtocolInterface;
    VOID*   UninstallProtocolInterface;
    EFI_STATUS  (*HandleProtocol)(EFI_HANDLE Handle,
                                  EFI_GUID *Protocol,
                                  VOID **Interface);
    VOID*   Reserved;
    VOID*   RegisterProtocolNotify;
    VOID*   LocateHandle;
    VOID*   LocateDevicePath;
    VOID*   InstallConfigurationTable;

    /* Image Services */
    VOID*   LoadImage;
    VOID*   StartImage;
    VOID*   Exit;
    VOID*   UnloadImage;
    EFI_STATUS  (*ExitBootServices)(EFI_HANDLE ImageHandle,
                                    UINTN MapKey);

    /* Miscellaneous Services */
    VOID*       GetNextMonotonicCount;
    EFI_STATUS  (*Stall)(UINTN Microseconds);
    EFI_STATUS  (*SetWatchdogTimer)(UINTN Timeout,
                                    UINT64 WatchdogCode,
                                    UINTN DataSize,
                                    CHAR16 *WatchdogData);

    /* DriverSupport Services */
    VOID*   ConnectController;
    VOID*   DisconnectController;

    /* Open and Close Protocol Services */
    VOID*   OpenProtocol;
    VOID*   CloseProtocol;
    VOID*   OpenProtocolInformation;

    /* Library Services */
    VOID*   ProtocolsPerHandle;
    VOID*   LocateHandleBuffer;
    EFI_STATUS  (*LocateProtocol)(EFI_GUID *Protocol,
                                  VOID *Registration,
                                  VOID **Interface);
    VOID*   InstallMultipleProtocolInterfaces;
    VOID*   UninstallMultipleProtocolInterfaces;

    /* 32-bit CRC Services */
    VOID*   CalculateCrc32;

    /* Miscellaneous Services (continued) */
    VOID    (*CopyMem)(VOID *Destination, VOID *Source, UINTN Length);
    VOID    (*SetMem)(VOID *Buffer, UINTN Size, UINT8 Value);
    VOID*   CreateEventEx;
};

/* ============================================================
 * 21. EFI_SYSTEM_TABLE
 * ============================================================ */

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER                Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                          FirmwareRevision;
    UINT32                          _pad;
    EFI_HANDLE                      ConsoleInHandle;
    VOID                            *ConIn;
    EFI_HANDLE                      ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;
    EFI_HANDLE                      StandardErrorHandle;
    VOID                            *StdErr;
    VOID                            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                           NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
};

/* ============================================================
 * 22. Protocol GUIDs
 * ============================================================ */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#define EFI_ACPI_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
