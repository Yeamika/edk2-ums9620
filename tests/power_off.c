#include "../Ums9620Pkg/W202ds/Library/Ump9620ResetSystemLib/PowerOff.c"
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) {}
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static UINT32 address,power=0xa50,unlock;
static UINTN elapsed,writes,triggers;
static int fifo_busy,read_busy,rtc_stuck,unlock_stuck,bad_write;
UINTN EFIAPI MicroSecondDelay(UINTN n) { elapsed+=n; return n; }
UINT32 EFIAPI MmioRead32(UINTN a) {
  if(a==ADI_FIFO) return fifo_busy?BIT31:BIT10;
  if(a!=ADI_DATA) return 0;
  if(read_busy) return BIT31;
  if(address==RTC_STATUS) return rtc_stuck?BIT4:0;
  if(address==POWER_UNLOCK) return unlock && !unlock_stuck?BIT15:0;
  if(address==POWER_PD) return power;
  return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  if(a==ADI_READ) {address=v;return v;}
  ++writes;
  if(a==POWER_UNLOCK) unlock=v;
  else if(a==RTC_CLEAR) {if(v!=RTC_MASK)bad_write=1;}
  else if(a==POWER_PD) {++triggers; if(unlock!=0x6e7f || v!=(power|BIT0))bad_write=1; power=v;}
  else bad_write=1;
  return v;
}
int main(void) {
  CHECK(W202PowerOff()==EFI_DEVICE_ERROR && triggers==1 && power==0xa51 && !unlock && !bad_write);
  CHECK(elapsed==100000);
  triggers=0;elapsed=0;unlock_stuck=1;
  CHECK(W202PowerOff()==EFI_TIMEOUT && !triggers && !unlock && elapsed<10000);
  unlock_stuck=0;rtc_stuck=1;elapsed=0;
  CHECK(W202PowerOff()==EFI_TIMEOUT && !triggers && elapsed<10000);
  rtc_stuck=0;fifo_busy=1;elapsed=0;writes=0;
  CHECK(W202PowerOff()==EFI_TIMEOUT && !writes && elapsed==2000);
  fifo_busy=0;read_busy=1;
  CHECK(W202PowerOff()==EFI_NOT_READY && !triggers);
  return 0;
}
