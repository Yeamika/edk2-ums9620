/* Validate the actual write-scope parser against corrupt and overlapping GPTs. */
#include "../Ums9620Pkg/W202ds/drivers/EmmcDxe/Gpt.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static UINT8 pool[131072];
EFI_GUID gEfiPartTypeSystemPartGuid = EFI_PART_TYPE_EFI_SYSTEM_PART_GUID;
VOID *EFIAPI AllocatePool(UINTN n) { return n<=sizeof(pool)?pool:NULL; }
VOID EFIAPI FreePool(VOID *p) {}
VOID *EFIAPI CopyMem(VOID *d, CONST VOID *s, UINTN n) { for (UINTN i=0;i<n;++i) ((UINT8 *)d)[i]=((CONST UINT8 *)s)[i]; return d; }
INTN EFIAPI CompareMem(CONST VOID *a, CONST VOID *b, UINTN n) { for (UINTN i=0;i<n;++i) if (((CONST UINT8 *)a)[i]!=((CONST UINT8 *)b)[i]) return 1; return 0; }
BOOLEAN EFIAPI CompareGuid(CONST GUID *a, CONST GUID *b) { return CompareMem(a,b,16)==0; }
static UINT8 disk[256][512];
static UINT32 fail_lba;
static EFI_STATUS read_sector(UINT8 cmd, UINT32 lba, UINT8 *out) {
  if (cmd!=17 || lba>=256 || lba==fail_lba) return EFI_DEVICE_ERROR;
  CopyMem(out,disk[lba],512); return EFI_SUCCESS;
}
static void seal(void) {
  EFI_PARTITION_TABLE_HEADER *p=(VOID *)disk[1], *b=(VOID *)disk[255];
  CopyMem(disk[254],disk[2],512);
  p->PartitionEntryArrayCRC32=Crc32(disk[2],512); p->Header.CRC32=0;
  p->Header.CRC32=Crc32(disk[1],p->Header.HeaderSize);
  CopyMem(b,p,sizeof(*b)); b->MyLBA=255;b->AlternateLBA=1;b->PartitionEntryLBA=254;
  b->Header.CRC32=0;b->Header.CRC32=Crc32(disk[255],b->Header.HeaderSize);
}
static void setup(void) {
  for (UINTN i=0;i<sizeof(disk);++i) ((UINT8 *)disk)[i]=0;
  fail_lba=MAX_UINT32;
  EFI_PARTITION_TABLE_HEADER *p=(VOID *)disk[1];
  p->Header.Signature=EFI_PTAB_HEADER_ID;p->Header.Revision=0x10000;p->Header.HeaderSize=sizeof(*p);
  p->MyLBA=1;p->AlternateLBA=255;p->FirstUsableLBA=3;p->LastUsableLBA=253;
  p->PartitionEntryLBA=2;p->NumberOfPartitionEntries=4;p->SizeOfPartitionEntry=128;
  EFI_PARTITION_ENTRY *e=(VOID *)disk[2];
  e[0].PartitionTypeGUID.Data1=1;e[0].StartingLBA=10;e[0].EndingLBA=90;
  e[1].PartitionTypeGUID=gEfiPartTypeSystemPartGuid;e[1].StartingLBA=100;e[1].EndingLBA=120;
  seal();
}
int main(void) {
  EFI_LBA first,last; EFI_STATUS status;
  setup(); CHECK(W202FindWritableEsp(read_sector,256,&first,&last)==EFI_SUCCESS && first==100 && last==120);
  for (UINTN scenario=0;scenario<8;++scenario) {
    setup(); EFI_PARTITION_ENTRY *e=(VOID *)disk[2];
    switch (scenario) {
      case 0: disk[1][20]^=1;break;
      case 1: disk[254][90]^=1;break;
      case 2: e[0].EndingLBA=100;seal();break;
      case 3: e[2]=e[1];seal();break;
      case 4: e[1].Attributes=BIT60;seal();break;
      case 5: e[1].StartingLBA=0;seal();break;
      case 6: ((EFI_PARTITION_TABLE_HEADER *)(VOID *)disk[1])->NumberOfPartitionEntries=MAX_UINT32;seal();break;
      case 7: fail_lba=254;break;
    }
    first=last=99;status=W202FindWritableEsp(read_sector,256,&first,&last);
    CHECK(EFI_ERROR(status) && first==0 && last==0);
  }
  return 0;
}
