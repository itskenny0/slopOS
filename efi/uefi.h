/* Minimal UEFI headers, so the loader builds without gnu-efi.
 *
 * This is not a UEFI development kit: it declares exactly what loader.c
 * uses, nothing more. Every struct layout and table offset below follows
 * the UEFI specification, and the static asserts at the bottom pin the
 * offsets the loader depends on. If you touch a struct, the asserts will
 * tell you before any firmware has to.
 *
 * Calling convention: the whole loader is compiled with -mabi=ms, so a
 * firmware function pointer is called directly. uefi_call_wrapper exists
 * only so loader.c reads the same as it did under gnu-efi.
 */
#ifndef PICOOS_UEFI_H
#define PICOOS_UEFI_H

/* ---- scalar types ---- */
typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef long long          INT64;
typedef short              INT16;
typedef unsigned long      UINTN;
typedef long               INTN;
typedef unsigned short     CHAR16;
typedef char               CHAR8;
typedef unsigned char      BOOLEAN;
typedef UINTN              EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef void              *EFI_EVENT;
typedef UINT64             EFI_PHYSICAL_ADDRESS;
typedef UINT64             EFI_VIRTUAL_ADDRESS;
typedef UINTN              EFI_TPL;

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

#define EFI_ERROR(s)   ((INTN)(s) < 0)

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EFI_SUCCESS             0
#define EFI_LOAD_ERROR          ((EFI_STATUS)0x8000000000000001ULL)
#define EFI_UNSUPPORTED         ((EFI_STATUS)0x8000000000000003ULL)
#define EFI_OUT_OF_RESOURCES    ((EFI_STATUS)0x8000000000000009ULL)

#define EFI_LIGHTGRAY  7
#define EFI_LIGHTRED   12
#define EFI_BACKGROUND_BLACK 0x00

/* ---- memory ---- */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32 Type;
    UINT32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- protocols the loader touches ---- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, \
      { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, \
      { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, \
      { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN  SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                            UINT32 ModeNumber, UINTN *SizeOfInfo,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (*SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 ModeNumber);
    EFI_STATUS (*Blt)(void);   /* unused here; present for layout */
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    struct _EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE;

typedef struct _EFI_FILE_HANDLE *EFI_FILE_HANDLE;
typedef struct _EFI_FILE_HANDLE {
    UINT64 Revision;
    EFI_STATUS (*Open)(struct _EFI_FILE_HANDLE *This,
                       struct _EFI_FILE_HANDLE **NewHandle,
                       CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (*Close)(struct _EFI_FILE_HANDLE *This);
    EFI_STATUS (*Delete)(struct _EFI_FILE_HANDLE *This);
    EFI_STATUS (*Read)(struct _EFI_FILE_HANDLE *This, UINTN *BufferSize,
                       void *Buffer);
    EFI_STATUS (*Write)(struct _EFI_FILE_HANDLE *This, UINTN *BufferSize,
                        void *Buffer);
    EFI_STATUS (*GetPosition)(struct _EFI_FILE_HANDLE *This, UINT64 *Position);
    EFI_STATUS (*SetPosition)(struct _EFI_FILE_HANDLE *This, UINT64 Position);
    EFI_STATUS (*GetInfo)(struct _EFI_FILE_HANDLE *This, EFI_GUID *InformationType,
                          UINTN *BufferSize, void *Buffer);
    EFI_STATUS (*SetInfo)(struct _EFI_FILE_HANDLE *This, EFI_GUID *InformationType,
                          UINTN BufferSize, void *Buffer);
    EFI_STATUS (*Flush)(struct _EFI_FILE_HANDLE *This);
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (*OpenVolume)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                             EFI_FILE_HANDLE *Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[];
} EFI_FILE_INFO;

#define SIZE_OF_EFI_FILE_INFO 80

/* ---- text console ---- */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*Reset)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                        BOOLEAN ExtendedVerification);
    EFI_STATUS (*OutputString)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                               CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    EFI_STATUS (*SetAttribute)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                               UINTN Attribute);
    EFI_STATUS (*ClearScreen)(struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    void *Reset;
    void *ReadKeyStroke;
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* ---- boot services (the full table, in spec order) ---- */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct _EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER Hdr;
    EFI_TPL (*RaiseTPL)(EFI_TPL NewTpl);
    void (*RestoreTPL)(EFI_TPL OldTpl);
    EFI_STATUS (*AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                               UINTN *MapKey, UINTN *DescriptorSize,
                               UINT32 *DescriptorVersion);
    EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (*FreePool)(void *Buffer);
    void *CreateEvent;
    void *SetTimer;
    EFI_STATUS (*WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN Microseconds);
    void *SetWatchdogTimer;
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
};

typedef struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    UINT32 __pad;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
} EFI_SYSTEM_TABLE;

/* ---- what efilib used to provide ---- */
extern EFI_SYSTEM_TABLE *ST __attribute__((visibility("hidden")));
extern EFI_BOOT_SERVICES *BS __attribute__((visibility("hidden")));

EFI_STATUS InitializeLib(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab);
UINTN Print(CHAR16 *fmt, ...);

/* direct call: the loader is built -mabi=ms, like the firmware */
#define uefi_call_wrapper(f, n, ...)  (f)(__VA_ARGS__)

/* ---- layout pins: these numbers come straight from the UEFI spec ---- */
#define UEFI_ASSERT_OFF(t, m, want) \
    _Static_assert(__builtin_offsetof(t, m) == (want), #t "." #m " moved!")

UEFI_ASSERT_OFF(EFI_SYSTEM_TABLE, ConIn, 48);
UEFI_ASSERT_OFF(EFI_SYSTEM_TABLE, ConOut, 64);
UEFI_ASSERT_OFF(EFI_SYSTEM_TABLE, BootServices, 96);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, AllocatePages, 40);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, GetMemoryMap, 56);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, AllocatePool, 64);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, FreePool, 72);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, WaitForEvent, 96);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, HandleProtocol, 152);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, ExitBootServices, 232);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, Stall, 248);
UEFI_ASSERT_OFF(EFI_BOOT_SERVICES, LocateProtocol, 320);
UEFI_ASSERT_OFF(EFI_LOADED_IMAGE, DeviceHandle, 24);
UEFI_ASSERT_OFF(EFI_FILE_INFO, FileSize, 8);
UEFI_ASSERT_OFF(EFI_MEMORY_DESCRIPTOR, NumberOfPages, 24);
_Static_assert(sizeof(EFI_MEMORY_DESCRIPTOR) == 40, "descriptor size");
_Static_assert(sizeof(EFI_GUID) == 16, "guid size");
_Static_assert(sizeof(CHAR16) == 2, "CHAR16 must be 16-bit: -fshort-wchar?");

#endif
