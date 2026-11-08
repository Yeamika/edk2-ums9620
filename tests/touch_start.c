/* Real touch SPI/startup code with a byte-level retained-IC emulator.
 * Check that identity/ownership gates precede writes and failed readback
 * cannot expose input. No reset or firmware-upload command is permitted. */
#include "../Ums9620Pkg/Drivers/BoardServicesDxe/Touch.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
typedef struct { UINT32 address, value; } CELL;
static CELL cells[32];
static unsigned cell_count, writes, forbidden, frame_writes;
static unsigned sleeps, awakes;
static UINT32 spi[0x100/4], gate, clock, ahb_address;
static UINT8 wire[132], fifo[32];
static UINTN wire_length, fifo_head, fifo_tail;
static BOOLEAN selected, bad_frame;
static BOOLEAN bad_sleep;
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n) {
  UINT8 *a=d;const UINT8 *b=s;while(n--)*a++=*b++;return d;
}
UINTN EFIAPI MicroSecondDelay(UINTN n) { return n; }
static UINT32 word(const UINT8 *p) {
  return (UINT32)p[0]|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24);
}
static UINT32 *cell(UINT32 address) {
  unsigned i;
  for(i=0;i<cell_count;++i) if(cells[i].address==address) return &cells[i].value;
  cells[cell_count].address=address;
  cells[cell_count].value=0;
  return &cells[cell_count++].value;
}
static UINT32 device_read(UINT32 address) {
  UINT32 value=*cell(address);
  if(bad_sleep && address==0x10007fd0 && value==0xa55aa55a) return 0;
  return bad_frame && address==0x10007294 && value==1 ? 2 : value;
}
static VOID device_write(UINT32 address,UINT32 value) {
  ++writes;
  /* Exact list used by normal reports, sense_on(1), CRC and AP notification. */
  if(address!=0x100072ec && address!=0x10007f04 && address!=0x10007294 &&
     address!=0x9000005c && address!=0x80010000 && address!=0x90000048 &&
     address!=0x10007fd0) ++forbidden;
  if(address==0x10007294) ++frame_writes;
  if(address==0x10007fd0) {
    if(value==0xa55aa55a) ++sleeps;
    else if(value==0) ++awakes;
  }
  *cell(address)=value;
  if(address==0x90000048 && value==0xec) {
    *cell(address)=0x1ec;*cell(0x900000a8)=5;
  }
}
static VOID finish(VOID) {
  if(wire_length<2 || wire[0]!=0xf2) return;
  switch(wire[1]) {
    case 0:
      if(wire_length==6) ahb_address=word(wire+2);
      else if(wire_length==10) device_write(word(wire+2),word(wire+6));
      else ++forbidden;
      break;
    case 0x13: case 0x0d: case 0x0c: break;
    case 0x31:
      if(wire_length!=4 || wire[2] || wire[3]) ++forbidden;
      break;
    default: ++forbidden;
  }
}
UINT32 EFIAPI MmioRead32(UINTN address) {
  UINTN off=address-SPI_BASE;
  if(address==0x20100000) return gate;
  if(address==0x649202c8) return clock;
  if(off==0x34) return BIT7;
  if(off==0x28) return BIT9;
  if(off==0) return fifo_head<fifo_tail ? fifo[fifo_head++] : 0;
  return off<sizeof(spi) ? spi[off/4] : 0;
}
UINT32 EFIAPI MmioWrite32(UINTN address,UINT32 value) {
  UINTN off=address-SPI_BASE;
  if(address==0x20100000) {gate=value;return value;}
  if(address==0x649202c8) {clock=value;return value;}
  if(off>=sizeof(spi)) return value;
  if(off==8) {
    BOOLEAN active=(value&BIT8)==0;
    if(active && !selected) wire_length=0;
    if(!active && selected) finish();
    selected=active;
  } else if(off==0x58) {
    fifo_head=0;fifo_tail=0;
  } else if(off==0 && selected) {
    UINTN pos=wire_length;
    UINT8 response=0;
    wire[wire_length++]=(UINT8)value;
    if(pos>=3 && wire[0]==0xf3 && wire[1]==8)
      response=(UINT8)(device_read(ahb_address)>>(8*((pos-3)%4)));
    fifo[fifo_tail++]=response;
  }
  spi[off/4]=value;return value;
}
UINT32 EFIAPI MmioAndThenOr32(UINTN address,UINT32 and,UINT32 or) {
  return MmioWrite32(address,(MmioRead32(address)&and)|or);
}
UINT32 EFIAPI MmioOr32(UINTN address,UINT32 or) {
  return MmioAndThenOr32(address,MAX_UINT32,or);
}
static VOID baseline(VOID) {
  unsigned i;
  for(i=0;i<ARRAY_SIZE(spi);++i) spi[i]=0;
  cell_count=0;writes=0;forbidden=0;frame_writes=0;
  selected=FALSE;bad_frame=FALSE;gate=BIT2;clock=7;
  bad_sleep=FALSE;sleeps=0;awakes=0;
  *cell(0x900000d0)=0x83102900;*cell(0x900000a8)=4;
  *cell(0x10007004)=0x5703842c;*cell(0x100072c0)=0x72c0;
  *cell(0x100070f8)=0x0ab6020a;*cell(0x1000719c)=0x00030300;
  *cell(0x10007088)=0x0012913f;*cell(0x10007f04)=0x9999;
  *cell(0x100072ec)=7;*cell(0x10007294)=10;
}
int main(void) {
  baseline();*cell(0x900000d0)=0;
  CHECK(W202TouchStart()==EFI_UNSUPPORTED && !mOwned && writes==0);
  CHECK(gate==BIT2 && clock==7);
  baseline();*cell(0x10007fd0)=0xa55aa55a;
  CHECK(W202TouchStart()==EFI_NOT_READY && !mOwned && writes==0);
  baseline();*cell(0x10007088)&=~BIT8;
  CHECK(W202TouchStart()==EFI_UNSUPPORTED && !mOwned && writes==0);
  baseline();bad_frame=TRUE;
  CHECK(W202TouchStart()==EFI_DEVICE_ERROR && !mOwned && frame_writes==3);
  CHECK(forbidden==0 && gate==BIT2 && clock==7);
#ifdef W202_TOUCH_RESUME_TEST
  baseline();bad_sleep=TRUE;
  CHECK(W202TouchStart()==EFI_DEVICE_ERROR && !mOwned && sleeps==8 && awakes==1);
  CHECK(*cell(0x10007fd0)==0 && forbidden==0 && gate==BIT2 && clock==7);
#endif
  baseline();
  CHECK(W202TouchStart()==EFI_SUCCESS && mOwned && forbidden==0);
  CHECK(*cell(0x100072ec)==0 && *cell(0x10007f04)==0 && *cell(0x10007294)==1);
  CHECK(*cell(0x10007fd0)==0 && *cell(0x90000048)==0x1ec);
#ifdef W202_TOUCH_RESUME_TEST
  CHECK(sleeps==1 && awakes==1);
#else
  CHECK(sleeps==0 && awakes==1);
#endif
  CHECK((gate&BIT6)!=0 && clock==0);
  W202TouchStop();
  CHECK(!mOwned && gate==BIT2 && clock==7);
  return 0;
}
