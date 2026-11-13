/* Execute real GOP mode transitions and direct presentation against fake MMIO.
 * This checks software contracts, not panel timing or physical DMA coherency. */
#include <Uefi.h>
#define W202_LK_TRACE_H
STATIC VOID LkTraceStage(UINT64 Stage) {}
#include "../Ums9620Pkg/W202ds/drivers/LkGopDxe/LkGopDxe.c"
#define CHECK(x) do {if (!(x)) return __LINE__;} while (0)
static UINT32 pixels[1200*1920], ctrl, raw, layer, alpha;
static UINT16 scratch[1200*1920];
static UINT32 geometry[5];
static unsigned blits, cleans;
static BOOLEAN stalled, allocation_failed;
VOID *EFIAPI AllocatePool(UINTN Size) {return !allocation_failed && Size==sizeof(scratch)?scratch:NULL;}
VOID EFIAPI FreePool(VOID *Buffer) {}
VOID *EFIAPI AllocateCopyPool(UINTN Size,CONST VOID *Buffer) {
  STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Copy;
  if (Size!=sizeof(Copy)) return NULL;
  Copy=*(CONST EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *)Buffer;return &Copy;
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) {}
VOID EFIAPI MemoryFence(VOID) {}
VOID *EFIAPI WriteBackDataCacheRange(VOID *Address,UINTN Length) {++cleans;return Address;}
UINTN EFIAPI MicroSecondDelay(UINTN N) {return N;}
UINT32 EFIAPI MmioRead32(UINTN A) {
  if (A==DPU_REG(8)) return ctrl;
  if (A==DPU_REG(0x25c)) return raw;
  return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) {
  if (A==DPU_REG(0x254)) raw&=~V;
  if (A==DPU_REG(0x40)) layer=V;
  if (A==DPU_REG(0x54)) alpha=V;
  for (UINTN I=0;I<5;++I) if (A==DPU_REG(mGeometryRegisters[I])) geometry[I]=V;
  if (A==DPU_REG(8)) {ctrl=V;if (!stalled && (V&BIT0)) raw|=BIT0;}
  return V;
}
RETURN_STATUS EFIAPI FrameBufferBlt(FRAME_BUFFER_CONFIGURE *Configure,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,
  UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta) {++blits;return RETURN_SUCCESS;}
int main(void) {
  UINTN I;
  volatile UINT16 *Short=(volatile UINT16 *)pixels;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;UINTN Size;
  CHECK(Query(&mGop,1,&Size,&Info)==EFI_SUCCESS && Size==sizeof(*Info) &&
        Info->PixelFormat==PixelBlueGreenRedReserved8BitPerColor);
  mMode.FrameBufferBase=(UINTN)pixels;mDpuCfg=BIT1;
  mInheritedLayer=0x00200045;mInheritedAlpha=0x80;
  mInheritedGeometry[0]=mInheritedGeometry[1]=0x078004b0;mInheritedGeometry[2]=1200;
  for (I=0;I<1200*1920;++I) Short[I]=(UINT16)I;
  allocation_failed=TRUE;
  CHECK(Set(&mGop,1)==EFI_OUT_OF_RESOURCES && mMode.Mode==0);
  allocation_failed=FALSE;
  CHECK(Set(&mGop,1)==EFI_SUCCESS && mMode.Mode==1);
  CHECK(mMode.Info->PixelFormat==PixelBlueGreenRedReserved8BitPerColor);
  CHECK(mMode.Info->HorizontalResolution==1920 && mMode.Info->VerticalResolution==1200 &&
        mMode.Info->PixelsPerScanLine==1920);
  CHECK(mMode.FrameBufferSize==sizeof(pixels) && layer==0x00100035 && alpha==255);
  CHECK(geometry[0]==0x04b00780 && geometry[1]==0x04b00780 && geometry[2]==1920 && !geometry[3] && !geometry[4]);
  for (I=0;I<1200*1920;++I) {
    UINT32 V=(UINT16)I,R=(V>>11)&31,G=(V>>5)&63,B=V&31;
    UINT32 Expected=(((R<<3)|(R>>2))<<16)|(((G<<2)|(G>>4))<<8)|((B<<3)|(B>>2));
    CHECK(pixels[(1199-I%1200)*1920+I/1200]==Expected);
  }
  CHECK(Blt(&mGop,NULL,EfiBltVideoToVideo,0,0,0,0,1920,1200,0)==EFI_SUCCESS && !blits);
  CHECK(Blt(&mGop,NULL,EfiBltVideoToVideo,1919,0,1919,0,2,1,0)==EFI_INVALID_PARAMETER);
  CHECK(Blt(&mGop,NULL,EfiBltVideoToVideo,0,0,1,0,1,1,0)==EFI_SUCCESS && blits==1);
  CHECK(Set(&mGop,0)==EFI_SUCCESS && mMode.Mode==0 && layer==mInheritedLayer && alpha==0x80);
  CHECK(mMode.Info==&mInfo && mMode.FrameBufferSize==1200*1920*2);
  for (I=0;I<5;++I) CHECK(geometry[I]==mInheritedGeometry[I]);
  for (I=0;I<1200*1920;++I) CHECK(Short[I]==(UINT16)I);
  CHECK(Set(&mGop,1)==EFI_SUCCESS);
  pixels[0]=0xff0000;pixels[1919]=0x00ff00;pixels[1199*1920]=0x0000ff;pixels[1200*1920-1]=0xffffff;
  CHECK(Set(&mGop,0)==EFI_SUCCESS);
  CHECK(Short[1199]==0xf800 && Short[1200*1920-1]==0x07e0 && Short[0]==0x001f && Short[1919*1200]==0xffff);
  stalled=TRUE;
  CHECK(Set(&mGop,1)==EFI_TIMEOUT && mMode.Mode==0);
  CHECK(Set(&mGop,2)==EFI_UNSUPPORTED && Set(NULL,0)==EFI_INVALID_PARAMETER);
  CHECK(cleans>=4);
  return 0;
}
