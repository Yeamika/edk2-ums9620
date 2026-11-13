// SPDX-License-Identifier: BSD-2-Clause-Patent
// Leave persistent evidence of actual ESP file reads before entering the UI.
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Guid/Gpt.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#ifdef W202_STORAGE_TEST
STATIC VOID TestEspWrite(EFI_FILE_PROTOCOL *Root, EFI_BLOCK_IO_PROTOCOL *Block) {
  CONST CHAR16 *Path = L"\\EFI\\W202DS\\UEFI-RW.BIN";
  EFI_FILE_PROTOCOL *File = NULL;
  EFI_STATUS Status, Closed;
  UINT8 *Data = AllocatePool(8193);
  UINTN Index, Bytes;
  BOOLEAN Created = FALSE;
  if (!Data) return;
  Status = Root->Open(Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (Status == EFI_NOT_FOUND) {
    Status = Root->Open(Root, &File, (CHAR16 *)Path,
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(Status)) goto Done;
    Created = TRUE;
    for (Index = 0; Index < 8193; ++Index) Data[Index] = (UINT8)((Index * 37) ^ (Index >> 8) ^ 0x5a);
    Bytes = 8193; Status = File->Write(File, &Bytes, Data);
    if (!EFI_ERROR(Status) && Bytes != 8193) Status = EFI_DEVICE_ERROR;
    if (!EFI_ERROR(Status)) Status = File->Flush(File);
    Closed = File->Close(File); File = NULL;
    if (!EFI_ERROR(Status)) Status = Closed;
    if (!EFI_ERROR(Status)) Status = Root->Flush(Root);
    if (!EFI_ERROR(Status)) Status = Block->FlushBlocks(Block);
    if (EFI_ERROR(Status)) goto Done;
    Status = Root->Open(Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  }
  if (EFI_ERROR(Status)) goto Done;
  ZeroMem(Data, 8193); Bytes = 8193; Status = File->Read(File, &Bytes, Data);
  if (!EFI_ERROR(Status) && Bytes != 8193) Status = EFI_VOLUME_CORRUPTED;
  if (!EFI_ERROR(Status)) {
    for (Index = 0; Index < 8193; ++Index)
      if (Data[Index] != (UINT8)((Index * 37) ^ (Index >> 8) ^ 0x5a)) { Status = EFI_CRC_ERROR; break; }
  }
  if (!EFI_ERROR(Status)) {
    Bytes = 1; Status = File->Read(File, &Bytes, Data);
    if (!EFI_ERROR(Status) && Bytes) Status = EFI_VOLUME_CORRUPTED;
  }
Done:
  if (File) { Closed = File->Close(File); if (!EFI_ERROR(Status)) Status = Closed; }
  DEBUG ((DEBUG_WARN, "UEFI_ESP_RW path=%s bytes=8193 created=%u status=%r\n", Path, (UINT32)Created, Status));
  FreePool(Data);
}
#endif

STATIC VOID
InspectFile (EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Path, BOOLEAN Marker)
{
  STATIC CONST CHAR8 Expected[] = "W202DS_UEFI_EMMC_ESP_READ_V1\n";
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  EFI_STATUS Status;
  UINTN Size, InfoSize = 0;
  UINT8 Data[64];
  UINT32 Crc;
  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "UEFI_ESP_FILE path=%s open=%r\n", Path, Status));
    return;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize < SIZE_OF_EFI_FILE_INFO || InfoSize > 65536) {
    File->Close (File);
    return;
  }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) { File->Close (File); return; }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (!EFI_ERROR (Status) && !(Info->Attribute & EFI_FILE_DIRECTORY)) {
    Size = sizeof (Data);
    Status = File->Read (File, &Size, Data);
    if (!EFI_ERROR (Status)) {
      Status = gBS->CalculateCrc32 (Data, Size, &Crc);
      if (!EFI_ERROR (Status)) {
        DEBUG ((DEBUG_WARN, "UEFI_ESP_READ path=%s size=%Lu sample=%u crc32=%08x\n",
                Path, Info->FileSize, (UINT32)Size, Crc));
        if (Marker && Info->FileSize == sizeof (Expected) - 1 &&
            Size == sizeof (Expected) - 1 && CompareMem (Data, Expected, Size) == 0) {
          DEBUG ((DEBUG_WARN, "UEFI_ESP_MARKER_OK actual eMMC/GPT/FAT file read\n"));
        }
        if (!Marker && Size >= 2) {
          DEBUG ((DEBUG_WARN, "UEFI_ESP_IMAGE path=%s mz=%u\n",
                  Path, (UINT32)(Data[0] == 'M' && Data[1] == 'Z')));
        }
      }
    }
  }
  if (EFI_ERROR (Status)) DEBUG ((DEBUG_WARN, "UEFI_ESP_FILE path=%s read=%r\n", Path, Status));
  FreePool (Info);
  File->Close (File);
}

VOID
InspectEsp (VOID)
{
  EFI_HANDLE *Handles;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_PARTITION_INFO_PROTOCOL *Part;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_FILE_PROTOCOL *Root;
  EFI_STATUS Status;
  UINTN Count, Index, EspCount = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                  NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "UEFI_STORAGE filesystems=%r\n", Status));
    return;
  }
  for (Index = 0; Index < Count; ++Index) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Part);
    if (EFI_ERROR (Status) || Part->Type != PARTITION_TYPE_GPT ||
        !CompareGuid (&Part->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid)) continue;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) continue;
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block);
    if (EFI_ERROR (Status)) continue;
    Status = Fs->OpenVolume (Fs, &Root);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "UEFI_ESP volume=%r\n", Status));
      continue;
    }
    ++EspCount;
    DEBUG ((DEBUG_WARN, "UEFI_ESP_OPEN index=%u first=%Lu last=%Lu readonly=%u\n",
            (UINT32)Index, Part->Info.Gpt.StartingLBA, Part->Info.Gpt.EndingLBA,
            (UINT32)Block->Media->ReadOnly));
    InspectFile (Root, L"\\EFI\\W202DS\\storage-test.txt", TRUE);
    InspectFile (Root, L"\\EFI\\W202DS\\README.TXT", FALSE);
#ifdef W202_STORAGE_TEST
    TestEspWrite (Root, Block);
#endif
    InspectFile (Root, L"\\EFI\\BOOT\\BOOTAA64.EFI", FALSE);
    InspectFile (Root, L"\\EFI\\Android\\bootaa64.efi", FALSE);
    InspectFile (Root, L"\\EFI\\DebianServer\\bootaa64.efi", FALSE);
    InspectFile (Root, L"\\EFI\\DebianKDE\\bootaa64.efi", FALSE);
    Root->Close (Root);
  }
  DEBUG ((DEBUG_WARN, "UEFI_STORAGE filesystems=%u esp=%u\n", (UINT32)Count, (UINT32)EspCount));
  FreePool (Handles);
}
