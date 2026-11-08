// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPRD SDHCI-r11 inherited from LK. No LK callbacks after handoff.
// Register layout: sprd-linux/lk 47f4d044, storage/mmc/include/sdhci.h;
// controller base: qogirn6pro hardware.h and W202DS stock device tree.
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
// LoaderHandoff.h is also consumed by the freestanding LK bridge.
typedef UINT32 uint32_t;
typedef INT32 int32_t;
#define PJB_EDK2
#include "../../Include/LoaderHandoff.h"
#include "Gpt.h"

#define EMMC_BASE          0x22200000U
// In v4 mode SPRD uses a 32-bit count at 0x00, not the legacy count at 0x06.
#define BLOCK_COUNT        0x00
#define BLOCK_SIZE         0x04
#define ARGUMENT           0x08
#define TRANSFER_COMMAND   0x0c
#define RESPONSE           0x10
#define BUFFER_DATA        0x20
#define PRESENT_STATE      0x24
#define HOST_CONTROL       0x28
#define CLOCK_CONTROL      0x2c
#define DATA_TIMEOUT       0x2e
#define SOFTWARE_RESET     0x2f
#define INT_STATUS         0x30
#define INT_ENABLE         0x34
#define SIGNAL_ENABLE      0x38
#define HOST_VERSION       0xfe
#define DMA_LOW            0x58
#define DMA_HIGH           0x5c
#define CMD_DONE           BIT0
#define DATA_DONE          BIT1
#define BUFFER_READY       BIT5
#define ERROR_STATUS       0xffff8000U
#define TRANSFER_STATUS    (ERROR_STATUS | CMD_DONE | DATA_DONE | BUFFER_READY)
#define COMMAND_BUSY       (BIT0 | BIT1)
#define R1_ERRORS          0xfdffa080U
#define SECTOR_SIZE        512U
#define WAIT_US            1000000U
#define READ_BYTES         0x8000U
#define DMA_BOUNDARY       0x10000U
#define DMA_PAGES          EFI_SIZE_TO_PAGES (2 * DMA_BOUNDARY)

STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC BOOLEAN mAvailable;
STATIC BOOLEAN mBusy;
STATIC EFI_EVENT mExitEvent;
STATIC EFI_PHYSICAL_ADDRESS mDmaAddress;
STATIC EFI_PHYSICAL_ADDRESS mDmaAllocation;
STATIC UINT8 *mDma;
STATIC EFI_LBA mEspFirst, mEspLast;
STATIC BOOLEAN mWritable, mCacheEnabled, mDirty;

STATIC EFI_STATUS
WaitStatus (UINT32 Mask)
{
  UINT32 Value;
  UINTN Elapsed;
  for (Elapsed = 0; Elapsed < WAIT_US; Elapsed += 10) {
    Value = MmioRead32 (EMMC_BASE + INT_STATUS);
    if (Value & ERROR_STATUS) {
      DEBUG ((DEBUG_ERROR, "EMMC: interrupt error status=%08x wait=%08x\n", Value, Mask));
      return EFI_DEVICE_ERROR;
    }
    if ((Value & Mask) == Mask) return EFI_SUCCESS;
    MicroSecondDelay (10);
  }
  DEBUG ((DEBUG_ERROR, "EMMC: timeout mask=%08x irq=%08x state=%08x response=%08x command=%08x\n",
          Mask, MmioRead32 (EMMC_BASE + INT_STATUS), MmioRead32 (EMMC_BASE + PRESENT_STATE),
          MmioRead32 (EMMC_BASE + RESPONSE), MmioRead32 (EMMC_BASE + TRANSFER_COMMAND)));
  return EFI_TIMEOUT;
}

// Stop a completed multi-block read, as in the original LK MMC path. DAT may
// remain inhibited until CMD12, so do not wait for COMMAND_BUSY's DAT bit.
STATIC EFI_STATUS
StopRead (VOID)
{
  EFI_STATUS Status;
  UINTN Elapsed;
  for (Elapsed = 0; Elapsed < WAIT_US; Elapsed += 10) {
    if (!(MmioRead32 (EMMC_BASE + PRESENT_STATE) & BIT0)) break;
    MicroSecondDelay (10);
  }
  if (Elapsed == WAIT_US) return EFI_TIMEOUT;
  MmioWrite32 (EMMC_BASE + INT_STATUS, CMD_DONE | DATA_DONE);
  MmioWrite32 (EMMC_BASE + ARGUMENT, 0);
  MmioWrite32 (EMMC_BASE + TRANSFER_COMMAND, (12U << 24) | (0x1bU << 16) | 3U);
  Status = WaitStatus (CMD_DONE);
  if (!EFI_ERROR (Status) && (MmioRead32 (EMMC_BASE + RESPONSE) & R1_ERRORS)) {
    Status = EFI_DEVICE_ERROR;
  }
  return EFI_ERROR (Status) ? Status : WaitStatus (DATA_DONE);
}

// Use LK's SDMA CMD18 + explicit CMD12 sequence for contiguous reads. A 32 KiB
// transfer in a 64 KiB-aligned buffer cannot reach the configured 64 KiB SDMA
// boundary, so no DMA restart interrupt is required. Writes stay single-sector.
STATIC EFI_STATUS
TransferBlocks (UINT8 Command, UINT32 Argument, UINT8 *Buffer, UINT32 Blocks)
{
  EFI_STATUS Status;
  UINT32 Count, Enables, Signals, DmaLow, DmaHigh;
  UINT16 Size;
  UINT8 Control, Timeout;
  UINTN Elapsed;
  BOOLEAN Write = Command == 24;
  BOOLEAN Data = Command != 6;
  BOOLEAN Multi = Command == 18;
  UINTN Bytes = (UINTN)Blocks * SECTOR_SIZE;
  if ((Command != 8 && Command != 17 && Command != 18 && Command != 24 && Command != 6) ||
      (Data && (Buffer == NULL || Blocks == 0 || Bytes > READ_BYTES)) ||
      (Multi ? Blocks < 2 : Blocks != 1) ||
      (!Data && (Buffer != NULL || Argument != 0x03200100))) return EFI_INVALID_PARAMETER;
  for (Elapsed = 0; Elapsed < WAIT_US; Elapsed += 10) {
    if (!(MmioRead32 (EMMC_BASE + PRESENT_STATE) & COMMAND_BUSY)) break;
    MicroSecondDelay (10);
  }
  if (Elapsed == WAIT_US) return EFI_TIMEOUT;
  Count = MmioRead32 (EMMC_BASE + BLOCK_COUNT);
  Size = MmioRead16 (EMMC_BASE + BLOCK_SIZE);
  Control = MmioRead8 (EMMC_BASE + HOST_CONTROL);
  Timeout = MmioRead8 (EMMC_BASE + DATA_TIMEOUT);
  Enables = MmioRead32 (EMMC_BASE + INT_ENABLE);
  Signals = MmioRead32 (EMMC_BASE + SIGNAL_ENABLE);
  DmaLow = MmioRead32 (EMMC_BASE + DMA_LOW);
  DmaHigh = MmioRead32 (EMMC_BASE + DMA_HIGH);
  MmioWrite32 (EMMC_BASE + SIGNAL_ENABLE, 0);
  MmioWrite32 (EMMC_BASE + INT_ENABLE, Enables | TRANSFER_STATUS);
  MmioWrite32 (EMMC_BASE + INT_STATUS, TRANSFER_STATUS);
  MmioWrite8 (EMMC_BASE + HOST_CONTROL, Control & ~0x18U);
  MmioWrite8 (EMMC_BASE + DATA_TIMEOUT, 0xe);
  MmioWrite16 (EMMC_BASE + BLOCK_SIZE, (4U << 12) | SECTOR_SIZE);
  MmioWrite32 (EMMC_BASE + BLOCK_COUNT, Blocks);
  if (Write) CopyMem (mDma, Buffer, SECTOR_SIZE);
  if (Data) WriteBackInvalidateDataCacheRange (mDma, Bytes);
  MemoryFence ();
  MmioWrite32 (EMMC_BASE + DMA_LOW, (UINT32)mDmaAddress);
  MmioWrite32 (EMMC_BASE + DMA_HIGH, (UINT32)(mDmaAddress >> 32));
  MmioWrite32 (EMMC_BASE + ARGUMENT, Argument);
  // Short response, CRC/index check, data present; read + block count + SDMA.
  MmioWrite32 (EMMC_BASE + TRANSFER_COMMAND,
               ((UINT32)Command << 24) | (Data ? (0x3aU << 16) | (Write ? 3U : Multi ? 0x33U : 0x13U) : (0x1bU << 16) | 3U));
  Status = WaitStatus (CMD_DONE);
  if (!EFI_ERROR (Status) && (MmioRead32 (EMMC_BASE + RESPONSE) & R1_ERRORS)) {
    Status = EFI_DEVICE_ERROR;
  }
  if (!EFI_ERROR (Status)) Status = WaitStatus (DATA_DONE);
  if (!EFI_ERROR (Status) && Multi) Status = StopRead ();
  if (!EFI_ERROR (Status) && Data && !Write) {
    MemoryFence ();
    InvalidateDataCacheRange (mDma, Bytes);
    CopyMem (Buffer, mDma, Bytes);
  }
  if (EFI_ERROR (Status)) {
    // Reset only the command/data lines after a failed transfer. Never reset
    // the card, its bus timing or power. Stop serving this boot on an error.
    MmioWrite8 (EMMC_BASE + SOFTWARE_RESET, 0x6);
    MicroSecondDelay (1000);
    mAvailable = FALSE;
    mMedia.MediaPresent = FALSE;
  }
  MmioWrite32 (EMMC_BASE + INT_STATUS, TRANSFER_STATUS);
  MmioWrite32 (EMMC_BASE + BLOCK_COUNT, Count);
  MmioWrite16 (EMMC_BASE + BLOCK_SIZE, Size);
  MmioWrite8 (EMMC_BASE + HOST_CONTROL, Control);
  MmioWrite8 (EMMC_BASE + DATA_TIMEOUT, Timeout);
  MmioWrite32 (EMMC_BASE + DMA_LOW, DmaLow);
  MmioWrite32 (EMMC_BASE + DMA_HIGH, DmaHigh);
  MmioWrite32 (EMMC_BASE + INT_ENABLE, Enables);
  MmioWrite32 (EMMC_BASE + SIGNAL_ENABLE, Signals);
  return Status;
}

STATIC EFI_STATUS
Transfer (UINT8 Command, UINT32 Argument, UINT8 *Buffer)
{
  return TransferBlocks (Command, Argument, Buffer, 1);
}

STATIC EFI_STATUS
ReadSector (UINT8 Command, UINT32 Argument, UINT8 *Buffer)
{
  if (Command != 8 && Command != 17) return EFI_INVALID_PARAMETER;
  return Transfer (Command, Argument, Buffer);
}

STATIC EFI_STATUS
CheckRequest (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
              UINTN BufferSize, VOID *Buffer)
{
  UINTN Blocks;
  if (This == NULL || This->Media != &mMedia) return EFI_INVALID_PARAMETER;
  if (MediaId != mMedia.MediaId) return EFI_MEDIA_CHANGED;
  if (!mAvailable || !mMedia.MediaPresent) return EFI_NO_MEDIA;
  if (BufferSize == 0) return EFI_SUCCESS;
  if (Buffer == NULL) return EFI_INVALID_PARAMETER;
  if (BufferSize % SECTOR_SIZE) return EFI_BAD_BUFFER_SIZE;
  Blocks = BufferSize / SECTOR_SIZE;
  if (Lba > mMedia.LastBlock || Blocks - 1 > mMedia.LastBlock - Lba) {
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
ReadBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
            UINTN BufferSize, VOID *Buffer)
{
  EFI_STATUS Status;
  UINTN Offset, Bytes;
  Status = CheckRequest (This, MediaId, Lba, BufferSize, Buffer);
  if (EFI_ERROR (Status) || BufferSize == 0) return Status;
  if (mBusy) return EFI_NOT_READY;
  mBusy = TRUE;
  for (Offset = 0; Offset < BufferSize; Offset += Bytes, Lba += Bytes / SECTOR_SIZE) {
    Bytes = MIN (BufferSize - Offset, READ_BYTES);
    Status = TransferBlocks (Bytes == SECTOR_SIZE ? 17 : 18, (UINT32)Lba,
                             (UINT8 *)Buffer + Offset, (UINT32)(Bytes / SECTOR_SIZE));
    if (EFI_ERROR (Status)) {
      mAvailable = FALSE;
      mMedia.MediaPresent = FALSE;
      break;
    }
  }
  mBusy = FALSE;
  return Status;
}

STATIC EFI_STATUS EFIAPI
WriteBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba,
             UINTN BufferSize, VOID *Buffer)
{
  EFI_STATUS Status = CheckRequest (This, MediaId, Lba, BufferSize, Buffer);
  UINTN Offset;
  if (EFI_ERROR (Status) || BufferSize == 0) return Status;
  if (!mWritable || mMedia.ReadOnly || Lba < mEspFirst || Lba > mEspLast ||
      BufferSize / SECTOR_SIZE - 1 > mEspLast - Lba) return EFI_WRITE_PROTECTED;
  if (mBusy) return EFI_NOT_READY;
  mBusy = TRUE;
  mDirty = TRUE;
  for (Offset = 0; Offset < BufferSize; Offset += SECTOR_SIZE, ++Lba) {
    Status = Transfer (24, (UINT32)Lba, (UINT8 *)Buffer + Offset);
    if (EFI_ERROR (Status)) {
      mAvailable = FALSE; mMedia.MediaPresent = FALSE; break;
    }
  }
  mBusy = FALSE;
  return Status;
}

STATIC EFI_STATUS EFIAPI
Reset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification)
{
  if (This == NULL || This->Media != &mMedia) return EFI_INVALID_PARAMETER;
  // The protocol's reset does not reset the inherited card/controller.
  return mAvailable ? EFI_SUCCESS : EFI_NO_MEDIA;
}

STATIC EFI_STATUS EFIAPI
Flush (EFI_BLOCK_IO_PROTOCOL *This)
{
  EFI_STATUS Status = Reset (This, FALSE);
  UINT8 ExtCsd[SECTOR_SIZE];
  if (EFI_ERROR (Status)) return Status;
  if (mBusy) return EFI_NOT_READY;
  if (!mDirty) return EFI_SUCCESS;
  mBusy = TRUE;
  // CMD6 WRITE_BYTE FLUSH_CACHE=1, R1b. DATA_DONE includes the busy phase.
  if (mCacheEnabled) {
    Status = Transfer (6, 0x03200100, NULL);
    // SWITCH reports operation errors in the following command's R1 status.
    // Re-read EXT_CSD (including R1 checks), without changing the card's RCA.
    if (!EFI_ERROR (Status)) Status = ReadSector (8, 0, ExtCsd);
    if (!EFI_ERROR (Status) && !(ExtCsd[33] & 1)) Status = EFI_DEVICE_ERROR;
  }
  if (!EFI_ERROR (Status)) mDirty = FALSE;
  else { mAvailable = FALSE; mMedia.MediaPresent = FALSE; }
  mBusy = FALSE;
  return Status;
}

STATIC EFI_BLOCK_IO_PROTOCOL mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION3, &mMedia, Reset, ReadBlocks, WriteBlocks, Flush
};

STATIC struct {
  VENDOR_DEVICE_PATH Controller;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof (VENDOR_DEVICE_PATH), 0}},
   {0x8299ad86, 0xf23a, 0x4266, {0x8d, 0x0b, 0xc8, 0x21, 0x25, 0x32, 0x0b, 0x8c}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}
};

STATIC VOID EFIAPI
ExitBootServices (EFI_EVENT Event, VOID *Context)
{
  mAvailable = FALSE;
  mMedia.MediaPresent = FALSE;
}

STATIC EFI_STATUS
InitializeController (VOID)
{
  const W202_LOADER_HANDOFF *Handoff = (VOID *)(UINTN)W202_HANDOFF_ADDRESS;
  EFI_STATUS Status;
  EFI_HANDLE Handle = NULL;
  UINT8 Sector[SECTOR_SIZE];
  UINT32 Sectors;
  UINT16 Version, Clock;
  if (!W202HandoffValid (Handoff)) return EFI_UNSUPPORTED;
  Version = MmioRead16 (EMMC_BASE + HOST_VERSION);
  Clock = MmioRead16 (EMMC_BASE + CLOCK_CONTROL);
  DEBUG ((DEBUG_WARN, "EMMC: inherited base=%x version=%04x clock=%04x state=%08x\n",
          EMMC_BASE, Version, Clock, MmioRead32 (EMMC_BASE + PRESENT_STATE)));
  // SDHCI v4 and a live internal/card clock are required. Keep the inherited
  // HS400/voltage/DLL settings; no clock, regulator or pinctrl programming.
  if (Version == 0xffff || (Version & 0xff) < 3 || (Clock & 7) != 7) {
    return EFI_UNSUPPORTED;
  }
  Status = ReadSector (8, 0, Sector);
  if (EFI_ERROR (Status)) return Status;
  Sectors = (UINT32)Sector[212] | ((UINT32)Sector[213] << 8) |
            ((UINT32)Sector[214] << 16) | ((UINT32)Sector[215] << 24);
  DEBUG ((DEBUG_WARN, "EMMC: ext_csd revision=%u partition=%02x sectors=%u cq=%u\n",
          Sector[192], Sector[179], Sectors, Sector[15]));
  // SEC_COUNT > 2 GiB implies sector-addressed eMMC. Expose only the already
  // selected user area. Boot/RPMB partition selection and CQE are unsupported.
  if (Sector[192] < 2 || (Sector[179] & 7) != 0 || Sector[15] != 0 || Sector[61] != 0 ||
      Sectors <= 0x400000) return EFI_UNSUPPORTED;
  mCacheEnabled = (Sector[33] & 1) != 0;
  Status = ReadSector (17, 1, Sector);
  if (EFI_ERROR (Status)) return Status;
  if (CompareMem (Sector, "EFI PART", 8) != 0) {
    DEBUG ((DEBUG_ERROR, "EMMC: user LBA1 lacks GPT signature; refusing BlockIo\n"));
    return EFI_VOLUME_CORRUPTED;
  }
  Status = W202FindWritableEsp (ReadSector, Sectors, &mEspFirst, &mEspLast);
  mWritable = !EFI_ERROR (Status);
  // A bad GPT never enables writes. A controller failure disables the driver.
  if (Status == EFI_DEVICE_ERROR || Status == EFI_TIMEOUT) return Status;
  DEBUG ((DEBUG_WARN, "EMMC: ESP write-range status=%r first=%Lu last=%Lu cache=%u\n",
          Status, mEspFirst, mEspLast, (UINT32)mCacheEnabled));
  ZeroMem (&mMedia, sizeof (mMedia));
  mMedia.MediaId = 0x202069;
  mMedia.MediaPresent = TRUE;
  mMedia.ReadOnly = !mWritable;
  mMedia.BlockSize = SECTOR_SIZE;
  mMedia.IoAlign = 1;
  mMedia.LastBlock = (EFI_LBA)Sectors - 1;
  mMedia.LogicalBlocksPerPhysicalBlock = 1;
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBootServices, NULL,
                             &gEfiEventExitBootServicesGuid, &mExitEvent);
  if (EFI_ERROR (Status)) return Status;
  mAvailable = TRUE;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle, &gEfiDevicePathProtocolGuid, &mPath,
                  &gEfiBlockIoProtocolGuid, &mBlockIo, NULL);
  if (EFI_ERROR (Status)) {
    mAvailable = FALSE;
    mMedia.MediaPresent = FALSE;
    gBS->CloseEvent (mExitEvent);
    return Status;
  }
  DEBUG ((DEBUG_WARN, "EMMC: BlockIo ready (writes limited to validated ESP) blocks=%u bytes=%Lu\n",
          Sectors, (UINT64)Sectors * SECTOR_SIZE));
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
EmmcInitialize (EFI_HANDLE Image, EFI_SYSTEM_TABLE *System)
{
  EFI_STATUS Status;
  mDmaAllocation = 0xffffffff;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, DMA_PAGES, &mDmaAllocation);
  if (EFI_ERROR (Status)) return Status;
  mDmaAddress = (mDmaAllocation + DMA_BOUNDARY - 1) & ~((UINT64)DMA_BOUNDARY - 1);
  mDma = (VOID *)(UINTN)mDmaAddress;
  Status = InitializeController ();
  DEBUG ((DEBUG_WARN, "EMMC: initialize=%r\n", Status));
#ifdef W202_STORAGE_TEST
  // Keep the failure at the end of retained RAM for the bounded RAMBoot probe.
  if (EFI_ERROR (Status)) CpuDeadLoop ();
#endif
  if (EFI_ERROR (Status)) gBS->FreePages (mDmaAllocation, DMA_PAGES);
  return Status;
}
