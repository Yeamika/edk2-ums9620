/* Real charger transport/policy with a two-byte FIFO and BQ register model. */
#include "../Ums9620Pkg/Drivers/BoardServicesDxe/Charger.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
static UINT32 controller[2][12],gate=0xa0000000;
static UINT8 chip[2][12],pointer[2],fifo[2][2],length[2];
static UINT32 battery_mask=BIT3,battery_data=BIT3;
static BOOLEAN ignore_write,fail_after_write,bad_write;
static UINTN writes;
static int bus(UINTN a) { return a>=0x222a0000 && a<0x222a0030?0:a>=0x22260000 && a<0x22260030?1:-1; }
EFI_STATUS W202AdiRead(UINT32 a,UINT32 *v) {
  if(a==0x64420284)*v=battery_mask;
  else if(a==0x64420280)*v=battery_data;
  else return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN a) {
  int b=bus(a);UINTN n=a&0xffff;
  if(a==I2C_GATE)return gate;
  if(b<0)return 0;
  if(n==0xc)return chip[b][pointer[b]];
  return controller[b][n/4];
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  int b=bus(a);UINTN n=a&0xffff;
  if(a==I2C_GATE){gate=v;return v;}
  if(b<0){bad_write=TRUE;return v;}
  if(n==0x14 && !(controller[b][0]&I2C_ENABLE))return v;
  controller[b][n/4]=v;
  if(n==0x2c){length[b]=0;return v;}
  if(n==0x10){if(length[b]<2)fifo[b][length[b]++]=(UINT8)v;else bad_write=TRUE;}
  if(n==0 && (v&I2C_START)) {
    if(v&BIT20)bad_write=TRUE;
    controller[b][0x14/4]=I2C_IRQ;
    if(!(v&I2C_READ)) {
      pointer[b]=fifo[b][0];
      if(controller[b][2]==2) {
        ++writes;
        if(pointer[b]!=1 || length[b]!=2)bad_write=TRUE;
        if(!ignore_write)chip[b][pointer[b]]=fifo[b][1];
        if(fail_after_write){controller[b][0x14/4]|=I2C_NACK;fail_after_write=FALSE;}
      } else if(controller[b][2]!=1 || length[b]!=1)bad_write=TRUE;
    }
  }
  return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN n) { return n; }
int main(void) {
  PJB_POWER_SAMPLE s={.valid=PJB_POWER_VOLTAGE|PJB_POWER_CURRENT|PJB_POWER_TEMPERATURE|PJB_POWER_EXTERNAL,
    .voltage_mv=4050,.external_power=1,.temperature_decic=320};
  UINTN before;
  chip[0][0]=0x14;chip[0][1]=0x8a;chip[0][2]=0xa3;
  chip[0][4]=0x90;chip[0][5]=0x87;chip[0][6]=0x66;chip[0][8]=0x24;chip[0][11]=0x14;
  chip[1][1]=0x9a;chip[1][11]=0x14;
  controller[0][1]=0x92;controller[0][2]=3;controller[0][8]=51;
  CHECK(W202ChargeUpdate(&s,700)==EFI_SUCCESS && chip[0][1]==0x8a && chip[1][1]==0x8a);
  CHECK(!mChargeRequested && !mChargeOwned); /* Boot never auto-enables. */
  CHECK(W202ChargeRequest(&s,700,1)==EFI_SUCCESS && chip[0][1]==0x9a && mChargeOwned);
  CHECK(chip[1][1]==0x8a && gate==0xa0000000 && !bad_write);
  CHECK(controller[0][0]==0 && controller[0][1]==0x92 && controller[0][2]==3 && controller[0][8]==51);
  before=writes;CHECK(W202ChargeUpdate(&s,700)==EFI_SUCCESS && writes==before);
  CHECK(W202ChargeUpdate(&s,356)==EFI_ACCESS_DENIED && chip[0][1]==0x8a && !mChargeRequested);
  CHECK(W202ChargeUpdate(&s,700)==EFI_SUCCESS && !mChargeOwned); /* Fault does not restart. */
  CHECK(W202ChargeRequest(&s,357,1)==EFI_SUCCESS && mChargeOwned);
  CHECK(W202ChargeUpdate(&s,1304)==EFI_SUCCESS && mChargeOwned);
  CHECK(W202ChargeUpdate(&s,1305)==EFI_ACCESS_DENIED && !mChargeOwned);
  s.valid&=~PJB_POWER_TEMPERATURE;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_ACCESS_DENIED && !mChargeOwned);
  s.valid|=PJB_POWER_TEMPERATURE;s.voltage_mv=3449;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_ACCESS_DENIED && !mChargeOwned);
  s.voltage_mv=3450;CHECK(W202ChargeRequest(&s,700,1)==EFI_SUCCESS && mChargeOwned);
  s.voltage_mv=4450;CHECK(W202ChargeUpdate(&s,700)==EFI_ACCESS_DENIED && !mChargeOwned);
  s.voltage_mv=4050;chip[0][9]=1;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_ACCESS_DENIED && chip[0][1]==0x8a);
  chip[0][9]=0;chip[0][0]|=BIT7;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_ACCESS_DENIED && chip[0][0]==0x94);
  chip[0][0]=0x14;chip[0][2]=0;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_ACCESS_DENIED && chip[0][2]==0);
  chip[0][2]=0xa3;chip[0][11]=0xff;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_UNSUPPORTED && chip[0][1]==0x8a);
  chip[0][11]=0x14;CHECK(W202ChargeRequest(&s,700,1)==EFI_SUCCESS && mChargeOwned);
  battery_data=0;CHECK(W202ChargeUpdate(&s,700)==EFI_ACCESS_DENIED && !mChargeOwned);
  battery_data=BIT3;battery_mask=0;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_ACCESS_DENIED && !mChargeOwned);
  battery_mask=BIT3;ignore_write=TRUE;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_DEVICE_ERROR && !mChargeOwned);
  ignore_write=FALSE;fail_after_write=TRUE;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_NO_RESPONSE && chip[0][1]==0x8a && !mChargeOwned);
  CHECK(W202ChargeRequest(&s,700,1)==EFI_SUCCESS && mChargeOwned);
  s.external_power=0;CHECK(W202ChargeUpdate(&s,700)==EFI_NOT_READY && !mChargeOwned && mChargeRequested);
  s.external_power=1;CHECK(W202ChargeUpdate(&s,700)==EFI_SUCCESS && mChargeOwned);
  /* Replug can report external power before PG or source detection settles. */
  chip[0][8]=0x20;
  CHECK(W202ChargeUpdate(&s,700)==EFI_NOT_READY && mChargeRequested && !mChargeOwned);
  CHECK(chip[0][1]==0x8a && chip[1][1]==0x8a);
  chip[0][8]=0x04;
  CHECK(W202ChargeUpdate(&s,700)==EFI_NOT_READY && mChargeRequested && !mChargeOwned);
  chip[0][8]=0x24;
  CHECK(W202ChargeUpdate(&s,700)==EFI_SUCCESS && mChargeOwned);
  chip[0][8]=0xe4;
  CHECK(W202ChargeUpdate(&s,700)==EFI_ACCESS_DENIED && !mChargeRequested && !mChargeOwned);
  chip[0][8]=0x24;
  CHECK(W202ChargeRequest(&s,700,1)==EFI_SUCCESS && mChargeOwned);
  s.external_power=0;CHECK(W202ChargeUpdate(&s,700)==EFI_NOT_READY && mChargeRequested);
  CHECK(W202ChargeStop()==EFI_SUCCESS && !mChargeRequested);
  s.external_power=1;CHECK(W202ChargeUpdate(&s,700)==EFI_SUCCESS && !mChargeOwned);
  CHECK(W202ChargeStop()==EFI_SUCCESS && !bad_write && chip[1][1]==0x8a);
  chip[0][1]=0x9a;chip[1][1]=0x9a;ignore_write=TRUE;
  CHECK(W202ChargeStop()==EFI_DEVICE_ERROR && chip[0][1]==0x9a && chip[1][1]==0x9a);
  ignore_write=FALSE;CHECK(W202ChargeStop()==EFI_SUCCESS && chip[0][1]==0x8a && chip[1][1]==0x8a);
  CHECK(W202ChargeRequest(&s,700,2)==EFI_INVALID_PARAMETER);
  return 0;
}
