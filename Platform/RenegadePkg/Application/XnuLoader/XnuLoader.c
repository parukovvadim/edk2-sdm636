#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>

#define BOOT_LINE_LENGTH    608
#define PAGE_16K            0x4000ULL

struct payload_seg {
    UINT64 vmaddr;
    UINT64 vmsize;
    UINT64 file_off;
    UINT64 file_size;
};

struct payload {
    CHAR8  magic[8];
    UINT32 version;
    UINT32 nsegs;
    UINT64 virt_base;
    UINT64 entry_va;
    UINT64 adt_off;
    UINT64 adt_size;
    UINT64 kernel_end;
    UINT64 phys_base;
    UINT64 mem_size;
    CHAR8  cmdline[BOOT_LINE_LENGTH];
    struct payload_seg segs[1];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
    UINT64 v_baseAddr;
    UINT64 v_display;
    UINT64 v_rowBytes;
    UINT64 v_width;
    UINT64 v_height;
    UINT64 v_depth;
};

struct boot_args {
    UINT16 Revision;
    UINT16 Version;
    UINT64 virtBase;
    UINT64 physBase;
    UINT64 memSize;
    UINT64 topOfKernelData;
    struct boot_video Video;
    UINT32 machineType;
    UINT64 deviceTreeP;
    UINT32 deviceTreeLength;
    CHAR8  CommandLine[BOOT_LINE_LENGTH];
    UINT64 bootFlags;
    UINT64 memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

#define GICD_BASE           0x17a00000ULL
#define GICR_BASE           0x17b00000ULL
#define GICR_SIZE           0x100000ULL
#define GICR_PE_SIZE        0x20000ULL
#define GICR_TYPER          0x08
#define GICR_WAKER          0x14
#define GICR_SGI            0x10000
#define GICR_IGROUPR0       (GICR_SGI + 0x080)
#define GICR_ISENABLER0     (GICR_SGI + 0x100)
#define GICR_IPRIORITYR     (GICR_SGI + 0x400)
#define VTIMER_INTID        19
#define PTIMER_INTID        18
#define SGI_INTID           1

#define PSCI_CPU_ON         0xc4000003ULL

// GUID for payload.bin embedded in Firmware Volume
// 3F15E0F4-D544-4A3C-9E2A-94639B0B7882
static EFI_GUID gPayloadBinGuid = {
    0x3F15E0F4, 0xD544, 0x4A3C, { 0x9E, 0x2A, 0x94, 0x63, 0x9B, 0x0B, 0x78, 0x82 }
};

extern VOID gold_entry(VOID);
extern volatile UINT64 gXnuEntryPa;

static inline UINT32 MmioRead32(UINTN Addr) { return *(volatile UINT32 *)Addr; }
static inline UINT64 MmioRead64(UINTN Addr) { return *(volatile UINT64 *)Addr; }
static inline VOID MmioWrite32(UINTN Addr, UINT32 Val) { *(volatile UINT32 *)Addr = Val; }
static inline VOID MmioWrite8(UINTN Addr, UINT8 Val) { *(volatile UINT8 *)Addr = Val; }

static UINT64 SmcCall(UINT64 fn, UINT64 a1, UINT64 a2, UINT64 a3)
{
    register UINT64 x0 __asm__("x0") = fn;
    register UINT64 x1 __asm__("x1") = a1;
    register UINT64 x2 __asm__("x2") = a2;
    register UINT64 x3 __asm__("x3") = a3;
    __asm__ volatile(
        "smc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x16", "x17", "memory"
    );
    return x0;
}

static VOID PrepareGic(VOID)
{
    // Enable GICD Group 1 and ARE_NS (non-secure view)
    UINT32 Dctlr = MmioRead32(GICD_BASE);
    MmioWrite32(GICD_BASE, Dctlr | 0x13);

    // Prepare redistributors for Gold cores (affinities 0x100 to 0x103)
    for (UINT32 aff = 0x100; aff <= 0x103; aff++) {
        UINTN r = 0;
        for (UINTN off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
            UINT64 typer = MmioRead64(GICR_BASE + off + GICR_TYPER);
            if ((UINT32)(typer >> 32) == aff) {
                r = GICR_BASE + off;
                break;
            }
            if (typer & (1 << 4)) break; // Last frame
        }
        if (!r) continue;

        // Wake redistributor
        UINT32 waker = MmioRead32(r + GICR_WAKER);
        if (waker & 2) {
            MmioWrite32(r + GICR_WAKER, waker & ~2U);
            for (int i = 0; i < 100000 && (MmioRead32(r + GICR_WAKER) & 4); i++)
                ;
        }

        // Put SGIs/PPIs in Group 1
        MmioWrite32(r + GICR_IGROUPR0, 0xffffffff);
        for (int id = 0; id < 32; id++)
            MmioWrite8(r + GICR_IPRIORITYR + id, 0xa0);

        // Enable virtual timer, physical timer, and SGI 1
        MmioWrite32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
    }
}

static EFI_STATUS LoadPayload(OUT VOID **PayloadBuf, OUT UINTN *PayloadSize)
{
    EFI_STATUS Status;

    // 1. Check if payload.bin exists on any filesystem (MicroSD / USB OTG)
    UINTN HandleCount = 0;
    EFI_HANDLE *HandleBuffer = NULL;
    Status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiSimpleFileSystemProtocolGuid,
        NULL,
        &HandleCount,
        &HandleBuffer
    );
    if (!EFI_ERROR(Status) && HandleBuffer != NULL) {
        for (UINTN i = 0; i < HandleCount; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
            Status = gBS->HandleProtocol(
                HandleBuffer[i],
                &gEfiSimpleFileSystemProtocolGuid,
                (VOID **)&Fs
            );
            if (EFI_ERROR(Status)) continue;

            EFI_FILE_PROTOCOL *Root;
            Status = Fs->OpenVolume(Fs, &Root);
            if (EFI_ERROR(Status)) continue;

            EFI_FILE_PROTOCOL *File;
            Status = Root->Open(Root, &File, L"payload.bin", EFI_FILE_MODE_READ, 0);
            if (!EFI_ERROR(Status)) {
                EFI_FILE_INFO *FileInfo;
                UINTN InfoSize = sizeof(EFI_FILE_INFO) + 128;
                FileInfo = AllocatePool(InfoSize);
                if (FileInfo != NULL) {
                    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, FileInfo);
                    if (!EFI_ERROR(Status)) {
                        UINTN Size = (UINTN)FileInfo->FileSize;
                        VOID *Buf = AllocatePool(Size);
                        if (Buf != NULL) {
                            Status = File->Read(File, &Size, Buf);
                            if (!EFI_ERROR(Status)) {
                                File->Close(File);
                                Root->Close(Root);
                                FreePool(FileInfo);
                                FreePool(HandleBuffer);
                                Print(L"[XNU] Found payload.bin on storage (%u bytes)\n", Size);
                                *PayloadBuf = Buf;
                                *PayloadSize = Size;
                                return EFI_SUCCESS;
                            }
                            FreePool(Buf);
                        }
                    }
                    FreePool(FileInfo);
                }
                File->Close(File);
            }
            Root->Close(Root);
        }
        FreePool(HandleBuffer);
    }

    // 2. Fall back to embedded payload.bin in Firmware Volume
    Status = GetSectionFromAnyFv(&gPayloadBinGuid, EFI_SECTION_RAW, 0, PayloadBuf, PayloadSize);
    if (!EFI_ERROR(Status)) {
        Print(L"[XNU] Found embedded payload.bin in ROM (%u bytes)\n", *PayloadSize);
        return EFI_SUCCESS;
    }

    return EFI_NOT_FOUND;
}

EFI_STATUS EFIAPI XnuLoaderEntryPoint(
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
)
{
    gBS->SetWatchdogTimer(0, 0, 0, NULL);
    if (gST->ConOut != NULL) {
        gST->ConOut->ClearScreen(gST->ConOut);
    }
    Print(L"\n========================================\n");
    Print(L"  XNU Mach-O UEFI Loader for whyred\n");
    Print(L"  Xiaomi Redmi Note 5 / Pro (SDM636)\n");
    Print(L"========================================\n\n");

    // 1. Locate and load payload
    VOID *PayloadData = NULL;
    UINTN PayloadSize = 0;
    EFI_STATUS Status = LoadPayload(&PayloadData, &PayloadSize);
    if (EFI_ERROR(Status) || PayloadData == NULL) {
        Print(L"[!] ERROR: payload.bin not found on disk or in ROM!\n");
        gBS->Stall(5000000);
        return EFI_NOT_FOUND;
    }

    struct payload *p = (struct payload *)PayloadData;
    if (CompareMem(p->magic, "SWEETXNU", 8) != 0 || p->version != 1) {
        Print(L"[!] ERROR: Invalid payload header (magic/version mismatch)!\n");
        gBS->Stall(5000000);
        return EFI_LOAD_ERROR;
    }

    Print(L"[+] Payload: version=%d, nsegs=%d\n", p->version, p->nsegs);
    Print(L"    virt_base: 0x%lx\n", p->virt_base);
    Print(L"    phys_base: 0x%lx\n", p->phys_base);
    Print(L"    mem_size : 0x%lx\n", p->mem_size);
    Print(L"    entry_va : 0x%lx\n", p->entry_va);

    UINT64 pa_of_va_base = p->phys_base - p->virt_base;
    UINT64 entry_pa = p->entry_va + pa_of_va_base;
    gXnuEntryPa = entry_pa;

    // 2. Relocate kernel segments
    for (UINT32 i = 0; i < p->nsegs; i++) {
        struct payload_seg *s = &p->segs[i];
        UINTN dst = (UINTN)(s->vmaddr + pa_of_va_base);
        CopyMem((VOID *)dst, (VOID *)((UINTN)p + s->file_off), (UINTN)s->file_size);
        if (s->vmsize > s->file_size) {
            ZeroMem((VOID *)(dst + s->file_size), (UINTN)(s->vmsize - s->file_size));
        }
    }
    Print(L"[+] Relocated %d Mach-O segments to DRAM\n", p->nsegs);

    // 3. Setup ADT and boot_args behind kernel
    UINTN adt_pa = (UINTN)(p->kernel_end + pa_of_va_base);
    CopyMem((VOID *)adt_pa, (VOID *)((UINTN)p + p->adt_off), (UINTN)p->adt_size);

    UINTN ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
    UINTN top = ba_pa + PAGE_16K;

    ZeroMem((VOID *)ba_pa, PAGE_16K);
    struct boot_args *ba = (struct boot_args *)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = top;

    // Retrieve live display parameters from GOP
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
    Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
    if (!EFI_ERROR(Status) && Gop && Gop->Mode && Gop->Mode->Info) {
        ba->Video.v_baseAddr = Gop->Mode->FrameBufferBase;
        ba->Video.v_width    = Gop->Mode->Info->HorizontalResolution;
        ba->Video.v_height   = Gop->Mode->Info->VerticalResolution;
        ba->Video.v_rowBytes = Gop->Mode->Info->PixelsPerScanLine * 4;
    } else {
        ba->Video.v_baseAddr = 0x9d400000;
        ba->Video.v_width    = 1080;
        ba->Video.v_height   = 2160;
        ba->Video.v_rowBytes = 1080 * 4;
    }
    ba->Video.v_display = 1;
    ba->Video.v_depth   = 32;

    ba->deviceTreeP = adt_pa - pa_of_va_base;
    ba->deviceTreeLength = (UINT32)p->adt_size;
    CopyMem(ba->CommandLine, p->cmdline, BOOT_LINE_LENGTH);
    ba->memSizeActual = p->mem_size;

    Print(L"[+] Framebuffer: %ux%u @ 0x%lx (stride=%u)\n",
        (UINT32)ba->Video.v_width, (UINT32)ba->Video.v_height,
        ba->Video.v_baseAddr, (UINT32)ba->Video.v_rowBytes);
    Print(L"[+] boot_args: pa=0x%lx, top=0x%lx\n", ba_pa, top);
    Print(L"[+] XNU entry physical: 0x%lx\n", entry_pa);

    // 4. Prepare hardware (GICv3 & Watchdog)
    Print(L"[+] Configuring GICv3...\n");
    PrepareGic();
    Print(L"[+] GICv3 configured.\n");

    // Disable Qualcomm APSS Hardware Watchdog
    Print(L"[+] Disabling Qualcomm APSS Watchdog...\n");
    MmioWrite32(0x17980000 + 0x8, 0); // APSS_WDT_ENABLE = 0
    MmioWrite32(0x17980000 + 0x0, 1); // APSS_WDT_RST = 1 (pet)
    Print(L"[+] Qualcomm APSS Watchdog disabled.\n");

    Print(L"[+] Booting Apple XNU kernel on Gold CPU 0x100...\n\n");
    gBS->Stall(2000000); // 2s delay to let user read status

    // 5. Exit Boot Services
    UINTN MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MapKey = 0;
    UINTN DescriptorSize = 0;
    UINT32 DescriptorVersion = 0;

    Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (Status == EFI_BUFFER_TOO_SMALL) {
        MemoryMapSize += 4096;
        MemoryMap = AllocatePool(MemoryMapSize);
        if (MemoryMap != NULL) {
            Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
        }
    }
    if (!EFI_ERROR(Status) && MemoryMap != NULL) {
        Status = gBS->ExitBootServices(ImageHandle, MapKey);
        if (EFI_ERROR(Status)) {
            // Retry once
            MemoryMapSize += 4096;
            gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
            gBS->ExitBootServices(ImageHandle, MapKey);
        }
    }

    // 6. Clean caches so data is coherent in DRAM for XNU (which starts with caches off)
    WriteBackInvalidateDataCacheRange((VOID *)(UINTN)p->phys_base, (UINTN)(top - p->phys_base));
    WriteBackInvalidateDataCacheRange((VOID *)&gXnuEntryPa, sizeof(gXnuEntryPa));
    WriteBackInvalidateDataCacheRange((VOID *)(UINTN)gold_entry, 4096);

    // 7. Hand off to Gold CPU 0x100 via PSCI CPU_ON
    SmcCall(PSCI_CPU_ON, 0x100, (UINTN)gold_entry, ba_pa);

    // 8. Silver CPU0 parks forever
    __asm__ volatile("msr daifset, #0xf");
    while (1) {
        __asm__ volatile("wfi");
    }

    return EFI_SUCCESS;
}
