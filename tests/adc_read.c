/* Exercise the real conversion, calibration and cleanup with a PMIC model. */
#include "../Ums9620Pkg/Drivers/BoardServicesDxe/Adc.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
static UINT32 ctl=0x1305,channel=0x1c,raw=1822,irq_en,reads,write_count;
static UINTN delay;
static BOOLEAN convert_timeout,write_timeout,bad_write;
EFI_STATUS W202AdiRead(UINT32 a,UINT32 *v) {
  if(a==ADC_CTL)*v=ctl;
  else if(a==ADC_CHANNEL)*v=channel;
  else if(a==ADC_INT_EN)*v=irq_en;
  else if(a==ADC_INT_RAW)*v=convert_timeout?0:1;
  else if(a==ADC_DATA){*v=raw;++reads;}
  else return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN a) { return a==0x64400030 && !write_timeout?BIT10:0; }
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  if(a==ADC_CTL)ctl=v;
  else if(a==ADC_CHANNEL)channel=v;
  else if(a!=ADC_INT_CLR)bad_write=TRUE;
  ++write_count;return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN n) { delay+=n;return n; }
int main(void) {
  W202_LOADER_HANDOFF h={.version=2,.flags=7,.ntc_high_mv=1400,.ntc_high_raw=3484,.ntc_low_mv=200,.ntc_low_raw=470};
  PJB_POWER_SAMPLE s={.valid=PJB_POWER_CURRENT};
  INT32 t=1234;
  UINT32 mv=0;
  CHECK(W202NtcVoltage(&mv)==EFI_NOT_READY);
  CHECK(W202ReadTemperature(&h,&s,&t)==EFI_SUCCESS && t>=306 && t<=308);
  CHECK(W202NtcVoltage(&mv)==EFI_SUCCESS && mv>=738 && mv<=739);
  CHECK(ctl==0x1305 && channel==0x1c && reads==15 && !bad_write && delay<4000);
  /* ADC values that would mimic a disconnected NTC never become temperatures. */
  raw=4095;CHECK(EFI_ERROR(W202ReadTemperature(&h,&s,&t)) && ctl==0x1305 && channel==0x1c);
  CHECK(W202NtcVoltage(&mv)==EFI_NOT_READY);
  raw=1822;convert_timeout=TRUE;delay=0;
  CHECK(W202ReadTemperature(&h,&s,&t)==EFI_TIMEOUT && delay<10000);
  CHECK(ctl==0x1305 && channel==0x1c && !mAdcFaulted);
  convert_timeout=FALSE;CHECK(W202ReadTemperature(&h,&s,&t)==EFI_SUCCESS);
  s.current_ma=1000;CHECK(W202ReadTemperature(&h,&s,&t)==EFI_SUCCESS && t>308);
  s.valid=0;reads=0;write_count=0;
  CHECK(W202ReadTemperature(&h,&s,&t)==EFI_NOT_READY && reads==0 && write_count==0);
  s.valid=PJB_POWER_CURRENT;irq_en=1;
  CHECK(W202ReadTemperature(&h,&s,&t)==EFI_NOT_READY && write_count==0);
  irq_en=0;ctl|=BIT1;
  CHECK(W202ReadTemperature(&h,&s,&t)==EFI_NOT_READY && write_count==0);
  ctl&=~BIT1;h.ntc_high_raw=h.ntc_low_raw;
  CHECK(W202ReadTemperature(&h,&s,&t)==EFI_COMPROMISED_DATA && write_count==0);
  h.ntc_high_raw=3484;write_timeout=TRUE;
  CHECK(EFI_ERROR(W202ReadTemperature(&h,&s,&t)) && mAdcFaulted);
  write_timeout=FALSE;CHECK(W202ReadTemperature(&h,&s,&t)==EFI_DEVICE_ERROR && write_count==0);
  return 0;
}
