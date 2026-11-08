/* Exercise STOP, timeouts, two-chip truth and inherited register restoration. */
#include "../Ums9620Pkg/Drivers/BoardServicesDxe/Charger.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
static UINT32 regs[2][12],gate=0xa0000000;
static UINT8 pointer[2],status[2]={0x24,0x24},id[2]={0x10,0x10};
static BOOLEAN missing[2],stuck[2],bad_write;
static UINTN delay;
static int bus(UINTN a) { return a>=0x222a0000 && a<0x222a0030?0:a>=0x22260000 && a<0x22260030?1:-1; }
UINT32 EFIAPI MmioRead32(UINTN a) {
  int b=bus(a);UINTN n=a&0xffff;
  if(a==I2C_GATE)return gate;
  if(b<0)return 0;
  if(n==0xc)return pointer[b]==0xb?id[b]:status[b];
  if(n==0x14 && stuck[b] && (regs[b][0]&I2C_START))return I2C_BUSY;
  return regs[b][n/4];
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  int b=bus(a);UINTN n=a&0xffff;
  if(a==I2C_GATE){gate=v;return v;}
  if(b<0){bad_write=TRUE;return v;}
  /* Real IP does not acknowledge a completion with its enable bit clear. */
  if(n==0x14 && !(regs[b][0]&I2C_ENABLE))return v;
  regs[b][n/4]=v;
  if(n==0x10){pointer[b]=(UINT8)v;if(v!=0 && v!=1 && v!=5 && v!=0x08 && v!=0x0b)bad_write=TRUE;}
  if(n==0 && (v&I2C_START)) {
    if(v&BIT20)bad_write=TRUE; /* both phases must issue STOP */
    regs[b][0x14/4]=missing[b]?I2C_IRQ|I2C_NACK:I2C_IRQ;
  }
  return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN n) { delay+=n;return n; }
int main(void) {
  UINT32 charging=99;
  regs[0][1]=0x92;regs[0][2]=3;regs[0][8]=51;
  regs[1][1]=0x94;regs[1][2]=5;regs[1][8]=77;
  CHECK(W202ReadCharging(&charging)==EFI_SUCCESS && charging==0);
  CHECK(W202ReadCharging(&charging)==EFI_SUCCESS && charging==0);
  CHECK(gate==0xa0000000 && regs[0][0]==0 && regs[1][0]==0 &&
        regs[0][1]==0x92 && regs[0][2]==3 && regs[0][8]==51 && regs[1][8]==77 && !bad_write);
  status[1]=0x34;CHECK(W202ReadCharging(&charging)==EFI_SUCCESS && charging==1);
  missing[0]=TRUE;CHECK(W202ReadCharging(&charging)==EFI_SUCCESS && charging==1);
  status[1]=0x24;charging=99;CHECK(EFI_ERROR(W202ReadCharging(&charging)) && charging==99);
  missing[0]=FALSE;status[0]=0x3c;CHECK(W202ReadCharging(&charging)==EFI_SUCCESS && charging==0);
  status[0]=0xf4;CHECK(W202ReadCharging(&charging)==EFI_SUCCESS && charging==0); /* OTG */
  id[0]=0xff;CHECK(W202ReadCharging(&charging)==EFI_UNSUPPORTED);id[0]=0x10;
  stuck[0]=TRUE;delay=0;charging=99;
  CHECK(W202ReadCharging(&charging)==EFI_TIMEOUT && charging==99 && delay<=4000);
  CHECK(gate==0xa0000000 && regs[0][0]==0 && regs[0][1]==0x92 && !bad_write);
  stuck[0]=FALSE;CHECK(W202ReadCharging(&charging)==EFI_SUCCESS && charging==0);
  regs[0][0x14/4]=I2C_BUSY;charging=99;
  CHECK(W202ReadCharging(&charging)==EFI_NOT_READY && charging==99 && gate==0xa0000000);
  return 0;
}
