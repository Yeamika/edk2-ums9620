// SPDX-License-Identifier: BSD-2-Clause-Patent
// Enable writes only inside the single ESP agreed upon by both valid GPT copies.
#include "Gpt.h"
#include <Guid/Gpt.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

STATIC UINT32 Crc32(CONST UINT8 *Data, UINTN Bytes) {
  UINT32 Crc = MAX_UINT32;
  UINTN Index, Bit;
  for (Index = 0; Index < Bytes; ++Index) {
    Crc ^= Data[Index];
    for (Bit = 0; Bit < 8; ++Bit) Crc = (Crc >> 1) ^ (0xedb88320U & (0U - (Crc & 1)));
  }
  return ~Crc;
}

STATIC BOOLEAN HeaderValid(UINT8 *Sector, UINT32 Lba, UINT32 Other, EFI_PARTITION_TABLE_HEADER *Out) {
  EFI_PARTITION_TABLE_HEADER *Header = (VOID *)Sector;
  UINT32 Saved, Actual;
  if (Header->Header.Signature != EFI_PTAB_HEADER_ID || Header->Header.Revision != 0x10000 ||
      Header->Header.HeaderSize < sizeof(*Header) || Header->Header.HeaderSize > 512 ||
      Header->Header.Reserved != 0 || Header->MyLBA != Lba || Header->AlternateLBA != Other) return FALSE;
  Saved = Header->Header.CRC32; Header->Header.CRC32 = 0;
  Actual = Crc32(Sector, Header->Header.HeaderSize); Header->Header.CRC32 = Saved;
  if (Actual != Saved) return FALSE;
  CopyMem(Out, Header, sizeof(*Out));
  return TRUE;
}

EFI_STATUS W202FindWritableEsp(W202_READ_SECTOR Read, UINT32 Sectors, EFI_LBA *First, EFI_LBA *Last) {
  UINT8 Sector[512], *Table = NULL;
  EFI_PARTITION_TABLE_HEADER Primary, Backup;
  EFI_PARTITION_ENTRY *Entry, *Esp = NULL;
  EFI_GUID Zero = {0};
  EFI_STATUS Status;
  UINTN Bytes, Blocks, Index, Count = 0, Size;
  *First = *Last = 0;
  if (Sectors < 128) return EFI_VOLUME_CORRUPTED;
  Status = Read(17, 1, Sector);
  if (EFI_ERROR(Status)) return Status;
  if (!HeaderValid(Sector, 1, Sectors - 1, &Primary)) return EFI_VOLUME_CORRUPTED;
  Size = Primary.SizeOfPartitionEntry;
  if (Size < 128 || Size > 1024 || (Size & (Size - 1)) ||
      !Primary.NumberOfPartitionEntries || Primary.NumberOfPartitionEntries > 1024) return EFI_UNSUPPORTED;
  Bytes = Size * Primary.NumberOfPartitionEntries;
  if (Bytes > 131072) return EFI_UNSUPPORTED;
  Blocks = (Bytes + 511) / 512;
  if (Primary.PartitionEntryLBA < 2 || Primary.PartitionEntryLBA > Sectors - Blocks ||
      Primary.FirstUsableLBA < Primary.PartitionEntryLBA + Blocks ||
      Primary.FirstUsableLBA > Primary.LastUsableLBA ||
      Primary.LastUsableLBA >= Sectors - Blocks - 1) return EFI_VOLUME_CORRUPTED;
  Status = Read(17, Sectors - 1, Sector);
  if (EFI_ERROR(Status)) return Status;
  if (!HeaderValid(Sector, Sectors - 1, 1, &Backup) ||
      Backup.FirstUsableLBA != Primary.FirstUsableLBA || Backup.LastUsableLBA != Primary.LastUsableLBA ||
      !CompareGuid(&Backup.DiskGUID, &Primary.DiskGUID) ||
      Backup.NumberOfPartitionEntries != Primary.NumberOfPartitionEntries || Backup.SizeOfPartitionEntry != Size ||
      Backup.PartitionEntryArrayCRC32 != Primary.PartitionEntryArrayCRC32 ||
      Backup.PartitionEntryLBA <= Backup.LastUsableLBA || Backup.PartitionEntryLBA > Sectors - 1 - Blocks)
    return EFI_VOLUME_CORRUPTED;
  Table = AllocatePool(Blocks * 512);
  if (Table == NULL) return EFI_OUT_OF_RESOURCES;
  for (Index = 0; Index < Blocks; ++Index) {
    Status = Read(17, (UINT32)Primary.PartitionEntryLBA + Index, Table + Index * 512);
    if (EFI_ERROR(Status)) goto Done;
    Status = Read(17, (UINT32)Backup.PartitionEntryLBA + Index, Sector);
    if (EFI_ERROR(Status)) goto Done;
    if (CompareMem(Table + Index * 512, Sector, MIN(512, Bytes - Index * 512)) != 0) {
      Status = EFI_VOLUME_CORRUPTED; goto Done;
    }
  }
  Status = EFI_VOLUME_CORRUPTED;
  if (Crc32(Table, Bytes) != Primary.PartitionEntryArrayCRC32) goto Done;
  for (Index = 0; Index < Primary.NumberOfPartitionEntries; ++Index) {
    Entry = (VOID *)(Table + Index * Size);
    if (CompareGuid(&Entry->PartitionTypeGUID, &Zero)) continue;
    if (Entry->StartingLBA < Primary.FirstUsableLBA || Entry->EndingLBA > Primary.LastUsableLBA ||
        Entry->StartingLBA > Entry->EndingLBA) goto Done;
    if (CompareGuid(&Entry->PartitionTypeGUID, &gEfiPartTypeSystemPartGuid)) { Esp = Entry; ++Count; }
  }
  if (Count != 1 || (Esp->Attributes & (BIT1 | BIT60))) goto Done;
  for (Index = 0; Index < Primary.NumberOfPartitionEntries; ++Index) {
    Entry = (VOID *)(Table + Index * Size);
    if (Entry == Esp || CompareGuid(&Entry->PartitionTypeGUID, &Zero)) continue;
    if (Entry->StartingLBA <= Esp->EndingLBA && Entry->EndingLBA >= Esp->StartingLBA) goto Done;
  }
  *First = Esp->StartingLBA; *Last = Esp->EndingLBA;
  Status = EFI_SUCCESS;
Done:
  FreePool(Table);
  return Status;
}
