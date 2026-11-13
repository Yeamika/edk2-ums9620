/* Exercise real watchdog C with a locked, posted-write PMIC transport. */
#include "../Ums9620Pkg/W202ds/drivers/BoardServicesDxe/Watchdog.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static UINT32 control=0xa, raw, fifo=BIT10;
static EFI_STATUS read_status;
static unsigned writes, waits, raw_reads, locked=1;
static UINTN addresses[8];
static UINT32 values[8];
EFI_STATUS W202AdiRead(UINT32 a,UINT32 *v) {
  if (EFI_ERROR(read_status)) return read_status;
  if (a==WDG_CTRL) *v=control;
  else if (a==WDG_RAW) { *v=raw; ++raw_reads; }
  else return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN a) { return a==0x64400030?fifo:0; }
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  if (writes<8) { addresses[writes]=a; values[writes]=v; }
  ++writes;
  if (a==WDG_LOCK) locked=v!=0xe551;
  else if (locked) writes=99;
  return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN us) { waits+=(unsigned)us; return us; }
int main(void) {
  CHECK(W202WatchdogRenew()==EFI_SUCCESS);
  CHECK(writes==4 && locked && raw_reads==1);
  CHECK(addresses[0]==WDG_LOCK && values[0]==0xe551);
  CHECK(addresses[1]==WDG_HIGH && values[1]==0x5a);
  CHECK(addresses[2]==WDG_LOW && values[2]==0);
  CHECK(addresses[3]==WDG_LOCK && values[3]==0x1aae);
  writes=0; control=0; CHECK(W202WatchdogRenew()==EFI_UNSUPPORTED && !writes);
  control=0xb; CHECK(W202WatchdogRenew()==EFI_UNSUPPORTED && !writes);
  control=0xa; read_status=EFI_TIMEOUT;
  CHECK(W202WatchdogRenew()==EFI_TIMEOUT && !writes);
  read_status=EFI_SUCCESS;raw=BIT4;raw_reads=0;
  CHECK(W202WatchdogRenew()==EFI_TIMEOUT && writes==2 && locked && raw_reads==2000);
  CHECK(addresses[0]==WDG_LOCK && addresses[1]==WDG_LOCK);
  writes=0;raw=0;fifo=0;waits=0;
  CHECK(W202WatchdogRenew()==EFI_TIMEOUT && !writes && waits==4000);
  fifo=BIT10;control=0xe;
  CHECK(W202WatchdogRenew()==EFI_SUCCESS && writes==4 && locked);
  CHECK(Write(WDG_CTRL,0)==EFI_INVALID_PARAMETER && writes==4);
  return 0;
}
