/* Exercise the real SPI burst writer, including its 32-byte FIFO boundaries. */
#define W202_TOUCH_RELOAD
#include <PiDxe.h>
#include "../Ums9620Pkg/W202ds/drivers/BoardServicesDxe/Touch.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static UINT32 spi[0x100/4];
static UINT8 wire[132], received[108544], data[108544];
static UINTN wire_length, received_length, transactions, fail_at, delays;
static UINT32 destination;
static BOOLEAN selected, continuous, increment, invalid;
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n) {
  UINT8 *a=d;const UINT8 *b=s;while(n--)*a++=*b++;return d;
}
UINTN EFIAPI MicroSecondDelay(UINTN n) { delays+=n;return n; }
static UINT32 word(const UINT8 *p) {
  return (UINT32)p[0]|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24);
}
static VOID finish(VOID) {
  UINTN n;
  if (!wire_length) return;
  if (wire_length<3 || wire[0]!=0xf2) {invalid=TRUE;return;}
  if(wire[1]==0x13) continuous=wire_length==3 && wire[2]==0x31;
  else if(wire[1]==0x0d) increment=wire_length==3 && wire[2]==0x11;
  else if(wire[1]==0) {
    if(!continuous || !increment || wire_length<7 || wire_length>130 ||
       word(wire+2)!=destination+received_length) {invalid=TRUE;return;}
    n=wire_length-6;
    if(received_length+n>sizeof(received)) {invalid=TRUE;return;}
    CopyMem(received+received_length,wire+6,n);received_length+=n;
  } else invalid=TRUE;
}
UINT32 EFIAPI MmioRead32(UINTN address) {
  UINTN off=address-SPI_BASE;
  if(off==0x34) return transactions==fail_at ? BIT6|BIT7 : BIT7;
  if(off==0x28) return BIT9;
  if(off==0) return 0;
  return off<sizeof(spi) ? spi[off/4] : 0;
}
UINT32 EFIAPI MmioWrite32(UINTN address,UINT32 value) {
  UINTN off=address-SPI_BASE;
  if(off>=sizeof(spi)) {invalid=TRUE;return value;}
  if(off==8) {
    BOOLEAN active=(value&BIT8)==0;
    if(active && !selected) {wire_length=0;++transactions;}
    if(!active && selected) finish();
    selected=active;
  } else if(off==0 && selected) {
    if(wire_length>=sizeof(wire)) invalid=TRUE;
    else wire[wire_length++]=(UINT8)value;
  }
  spi[off/4]=value;return value;
}
static VOID reset(UINTN failure) {
  UINTN i;
  for(i=0;i<ARRAY_SIZE(spi);++i) spi[i]=0;
  wire_length=received_length=transactions=delays=0;
  selected=continuous=increment=invalid=FALSE;fail_at=failure;
  destination=0x20000000;
}
int main(void) {
  UINTN i,j;
  STATIC CONST UINTN sizes[]={1,4,31,32,123,124,125,40720,108544};
  for(i=0;i<sizeof(data);++i) data[i]=(UINT8)(i^(i>>8));
  for(j=0;j<ARRAY_SIZE(sizes);++j) {
    reset(MAX_UINTN);
    CHECK(WriteFirmwareMemory(destination,data,sizes[j])==EFI_SUCCESS);
    CHECK(received_length==sizes[j] && !selected && !invalid);
    CHECK(transactions==2+(sizes[j]+123)/124);
    for(i=0;i<sizes[j];++i) CHECK(received[i]==data[i]);
  }
  for(j=1;j<=5;++j) {
    reset(j);
    CHECK(WriteFirmwareMemory(destination,data,sizeof(data))==EFI_TIMEOUT);
    CHECK(!selected && !invalid && transactions==j && delays==4000);
    CHECK(received_length==(j>3 ? (j-3)*124 : 0));
    /* A following transfer can re-establish the bus mode after an error. */
    fail_at=MAX_UINTN;destination+=received_length;received_length=0;
    CHECK(WriteFirmwareMemory(destination,data,125)==EFI_SUCCESS);
    CHECK(received_length==125 && !selected && !invalid);
  }
  return 0;
}
