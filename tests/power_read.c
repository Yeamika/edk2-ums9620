#include "../Ums9620Pkg/W202ds/drivers/BoardServicesDxe/Power.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
static UINT32 address, writes, raw=2630, saved=0x552d, counter=100000, current=8865;
static UINT32 fail_address, busy_reads;
static BOOLEAN timeout, invalid_write;
UINT32 EFIAPI MmioRead32(UINTN a) {
  if(a!=0x6440002c)return 0;
  if(timeout)return BIT31;
  if(busy_reads){--busy_reads;return BIT31;}
  if(address==0x64420820)return raw;
  if(address==0x644208a8)return saved;
  if(address==0x6442082c)return current;
  if(address==0x64420868)return counter>>16;
  if(address==0x6442086c)return counter&0xffff;
  if(address==0x6442239c)return BIT2;
  return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 value) {
  if(a!=0x64400028)invalid_write=TRUE;
  address=value;if(address==fail_address)busy_reads=2000;++writes;return value;
}
EFI_STATUS W202ReadTemperature(CONST W202_LOADER_HANDOFF *h,CONST PJB_POWER_SAMPLE *s,INT32 *t) { return EFI_UNSUPPORTED; }
EFI_STATUS W202ReadCharging(UINT32 *c) { return EFI_UNSUPPORTED; }
UINTN EFIAPI MicroSecondDelay(UINTN n) { return n; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return 1000000; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 ticks) { return ticks; }
int main(void) {
  PJB_POWER_SAMPLE s={0};
  W202_LOADER_HANDOFF h={.flags=3,.voltage_gain=694,.voltage_offset=0};
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS);
  CHECK(s.voltage_mv==3789 && s.voltage_raw==2630 && s.external_power==1 && s.current_ma==1000);
  CHECK(s.valid==(PJB_POWER_RAW_FGU|PJB_POWER_EXTERNAL|PJB_POWER_VOLTAGE|PJB_POWER_CURRENT|PJB_POWER_PERCENT|PJB_POWER_PERCENT_ESTIMATED));
  CHECK(s.percent==46 && !(s.valid&PJB_POWER_CHARGING) && !invalid_write);
  counter+=371660;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && s.percent==47);
  /* Counter/SOC read failures must not discard charge accumulated in a gap. */
  fail_address=0x64420868;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && !(s.valid&PJB_POWER_PERCENT));
  fail_address=0;counter+=371660;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && s.percent==48);
  fail_address=0x644208a8;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && (s.valid&PJB_POWER_VOLTAGE) &&
        (s.valid&PJB_POWER_CURRENT) && (s.valid&PJB_POWER_EXTERNAL) && !(s.valid&PJB_POWER_PERCENT));
  fail_address=0x64420820;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && !(s.valid&PJB_POWER_VOLTAGE) &&
        (s.valid&PJB_POWER_CURRENT) && (s.valid&PJB_POWER_EXTERNAL) && s.percent==48);
  fail_address=0;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && s.percent==48);
  counter=0xfffffff0;W202PowerReset();s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && s.percent==46);
  counter=0x10;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && s.percent==46);
  saved=0xffff;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && !(s.valid&PJB_POWER_PERCENT));
  saved=0x5a05;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && !(s.valid&PJB_POWER_PERCENT));
  saved=0x5164;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && !(s.valid&PJB_POWER_PERCENT));
  h.voltage_gain=0;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_SUCCESS && !(s.valid&(PJB_POWER_VOLTAGE|PJB_POWER_PERCENT|PJB_POWER_CURRENT)));
  timeout=TRUE;s.valid=0;
  CHECK(W202ReadPower(&h,&s)==EFI_NOT_READY && !s.valid);
  timeout=FALSE;h.flags=0;
  CHECK(W202ReadPower(&h,&s)==EFI_UNSUPPORTED);
  CHECK(W202ReadPower(NULL,&s)==EFI_INVALID_PARAMETER);
  CHECK(W202AdiRead(0x64420820,NULL)==EFI_INVALID_PARAMETER);
  return 0;
}
