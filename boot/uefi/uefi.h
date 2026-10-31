/* Минимальный набор UEFI-определений для MigaBoot (без gnu-efi). */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t UINTN;
typedef int64_t INTN;
typedef uint64_t EFI_STATUS;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef void *EFI_HANDLE;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS 0
#define EFI_ERR(n) (0x8000000000000000ULL | (n))
#define EFI_LOAD_ERROR EFI_ERR(1)
#define EFI_UNSUPPORTED EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_ERROR(s) ((INTN)(s) < 0)

typedef struct { uint32_t a; uint16_t b; uint16_t c; uint8_t d[8]; } EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ---- память ---- */
typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;
enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
    EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory
};
typedef struct {
    uint32_t Type, Pad;
    uint64_t PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- текстовый вывод ---- */
struct EFI_SIMPLE_TEXT_OUTPUT;
typedef struct EFI_SIMPLE_TEXT_OUTPUT {
    void *Reset;
    EFI_STATUS(EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT *, CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS(EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT *);
    void *SetCursorPosition, *EnableCursor, *Mode;
} EFI_SIMPLE_TEXT_OUTPUT;

/* ---- файлы ---- */
struct EFI_FILE;
typedef struct EFI_FILE {
    uint64_t Revision;
    EFI_STATUS(EFIAPI *Open)(struct EFI_FILE *, struct EFI_FILE **, CHAR16 *, uint64_t mode, uint64_t attr);
    EFI_STATUS(EFIAPI *Close)(struct EFI_FILE *);
    void *Delete;
    EFI_STATUS(EFIAPI *Read)(struct EFI_FILE *, UINTN *, void *);
    void *Write;
    EFI_STATUS(EFIAPI *GetPosition)(struct EFI_FILE *, uint64_t *);
    EFI_STATUS(EFIAPI *SetPosition)(struct EFI_FILE *, uint64_t);
    void *GetInfo, *SetInfo, *Flush;
} EFI_FILE;
#define EFI_FILE_MODE_READ 1ULL

typedef struct EFI_SIMPLE_FILE_SYSTEM {
    uint64_t Revision;
    EFI_STATUS(EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM *, EFI_FILE **);
} EFI_SIMPLE_FILE_SYSTEM;

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;
} EFI_LOADED_IMAGE; /* нам нужны только эти поля */

/* ---- графика (GOP) ---- */
typedef struct {
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;
enum { PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor,
       PixelBitMask, PixelBltOnly };
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution;
    uint32_t PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GOP_MODE_INFO;
typedef struct {
    uint32_t MaxMode, Mode;
    EFI_GOP_MODE_INFO *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GOP_MODE;
typedef struct EFI_GOP {
    EFI_STATUS(EFIAPI *QueryMode)(struct EFI_GOP *, uint32_t, UINTN *, EFI_GOP_MODE_INFO **);
    EFI_STATUS(EFIAPI *SetMode)(struct EFI_GOP *, uint32_t);
    void *Blt;
    EFI_GOP_MODE *Mode;
} EFI_GOP;

/* ---- Boot Services (порядок полей критичен) ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS(EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, uint32_t, UINTN, EFI_PHYSICAL_ADDRESS *);
    void *FreePages;
    EFI_STATUS(EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, uint32_t *);
    EFI_STATUS(EFIAPI *AllocatePool)(uint32_t, UINTN, void **);
    EFI_STATUS(EFIAPI *FreePool)(void *);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS(EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS(EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    void *GetNextMonotonicCount;
    EFI_STATUS(EFIAPI *Stall)(UINTN);
    EFI_STATUS(EFIAPI *SetWatchdogTimer)(UINTN, uint64_t, UINTN, CHAR16 *);
    void *ConnectController, *DisconnectController, *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS(EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32, *CopyMem, *SetMem, *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#define EFI_GOP_GUID {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}
#define EFI_SFS_GUID {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_LOADED_IMAGE_GUID {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_ACPI20_GUID {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}
#define EFI_SMBIOS3_GUID {0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}}
#define EFI_SMBIOS_GUID {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}
#define EFI_EDID_ACTIVE_GUID {0xbd8c1056, 0x9f36, 0x44ec, {0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86}}
#define EFI_EDID_DISCOVERED_GUID {0x1c0c34f6, 0xd380, 0x41fa, {0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa}}
typedef struct { uint32_t SizeOfEdid; uint8_t *Edid; } EFI_EDID_PROTOCOL;
#define EFI_ACPI10_GUID {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}
