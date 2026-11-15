/* Execute the single native mode, double-buffer presentation and same-layout
 * child handoff against fake MMIO. This does not simulate physical DMA. */
#include <Uefi.h>
#define W202_LK_TRACE_H
STATIC VOID LkTraceStage(UINT64 Stage) {}
#include "../Ums9620Pkg/W202ds/drivers/LkGopDxe/LkGopDxe.c"
#define CHECK(x) do {if (!(x)) return __LINE__;} while (0)
static UINT32 scanouts[2][1920*1200], pinned[1920*1200];
static UINT8 configure[64];
static UINT32 ctrl, raw, layer=0x00200045, geometry[5]={0x078004b0,0x078004b0,1200,0,0};
static const UINT32 offsets[5]={0x44,0x48,0x4c,0x50,0x60};
static UINT32 dma=HANDOFF_BASE;
static unsigned blits,stops,cleans,installs;
static BOOLEAN stop_stalled, stalled;
static EFI_STATUS EFIAPI Pages(EFI_ALLOCATE_TYPE T,EFI_MEMORY_TYPE M,UINTN N,EFI_PHYSICAL_ADDRESS *A) {
 if (T!=AllocateMaxAddress || M!=EfiBootServicesData || N!=2*EFI_SIZE_TO_PAGES(NATIVE_BYTES)) return EFI_INVALID_PARAMETER;
 *A=(UINTN)scanouts;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...) {++installs;return EFI_SUCCESS;}
static EFI_BOOT_SERVICES services={.AllocatePages=Pages,.InstallMultipleProtocolInterfaces=Install};
EFI_BOOT_SERVICES *gBS=&services;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) {
 VOID *R=D;if ((UINTN)D==HANDOFF_BASE) D=pinned;
 UINT8 *d=D;const UINT8 *s=S;while(N--)*d++=*s++;return R;
}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V) {UINT8 *d=D;while(N--)*d++=V;return D;}
VOID *EFIAPI AllocatePool(UINTN N) {return N==sizeof(configure)?configure:NULL;}
VOID EFIAPI FreePool(VOID *P) {}
VOID *EFIAPI AllocateCopyPool(UINTN N,CONST VOID *B) {
 static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION I;if(N!=sizeof(I))return NULL;I=*(CONST EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *)B;return &I;
}
RETURN_STATUS EFIAPI FrameBufferBltConfigure(VOID *B,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *I,FRAME_BUFFER_CONFIGURE *C,UINTN *N) {
 if (!C) {*N=sizeof(configure);return RETURN_BUFFER_TOO_SMALL;}return RETURN_SUCCESS;
}
RETURN_STATUS EFIAPI FrameBufferBlt(FRAME_BUFFER_CONFIGURE *C,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *B,EFI_GRAPHICS_OUTPUT_BLT_OPERATION O,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN D) {++blits;return RETURN_SUCCESS;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L) {return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...) {}
VOID EFIAPI MemoryFence(VOID) {}
VOID *EFIAPI WriteBackDataCacheRange(VOID *A,UINTN N) {++cleans;return A;}
UINTN EFIAPI MicroSecondDelay(UINTN N) {return N;}
UINT32 EFIAPI MmioRead32(UINTN A) {
 if(A==DPU_REG(0))return 0x600;
 if(A==DPU_REG(0x18))return 0x078004b0;
 if(A==DPU_REG(0x2c))return 1;
 if(A==DPU_REG(0x30))return dma;
 if(A==DPU_REG(0x40))return layer;
 for(UINTN i=0;i<5;i++)if(A==DPU_REG(offsets[i]))return geometry[i];
 if(A==DPU_REG(8))return ctrl;
 if(A==DPU_REG(0x25c))return raw;
 return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) {
 if(A==DPU_REG(0x254))raw&=~V;
 if(A==DPU_REG(0x30))dma=V;
 if(A==DPU_REG(0x40))layer=V;
 for(UINTN i=0;i<5;i++)if(A==DPU_REG(offsets[i]))geometry[i]=V;
 if(A==DPU_REG(8)) {ctrl=V;if(V&BIT1){++stops;if(!stop_stalled)raw|=BIT0;}
 else if(!stalled&&(V&BIT0))raw|=BIT18;}
 return V;
}
EFI_GUID gEfiGraphicsOutputProtocolGuid={0},gEfiDevicePathProtocolGuid={0};
int main(void) {
 EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *I;UINTN N;
 CHECK(Query(&mGop,0,&N,&I)==EFI_SUCCESS && I->HorizontalResolution==1920 && I->VerticalResolution==1200);
 CHECK(Query(&mGop,1,&N,&I)==EFI_UNSUPPORTED && Query(NULL,0,&N,&I)==EFI_INVALID_PARAMETER);
 CHECK(Initialize(NULL,NULL)==EFI_SUCCESS && installs==1 && stops==1);
 CHECK(mMode.MaxMode==1 && mMode.Mode==0 && mMode.FrameBufferSize==NATIVE_BYTES);
 CHECK((layer&0x007000f0)==0x00100030 && geometry[2]==1920);
 CHECK(mFrontBase==(UINTN)scanouts[0] && mMode.FrameBufferBase==(UINTN)scanouts[1]);
 UINT32 *draw=(VOID *)(UINTN)mMode.FrameBufferBase;draw[10]=0x123456;
 CHECK(Blt(&mGop,NULL,EfiBltVideoToVideo,10,0,10,0,1,1,0)==EFI_SUCCESS && !blits);
 CHECK(mFrontBase==(UINTN)scanouts[1] && mMode.FrameBufferBase==(UINTN)scanouts[0] && scanouts[0][10]==0x123456);
 CHECK(Blt(&mGop,NULL,EfiBltVideoToVideo,1919,0,1919,0,2,1,0)==EFI_INVALID_PARAMETER);
 CHECK(Set(&mGop,1)==EFI_UNSUPPORTED && Set(&mGop,0)==EFI_SUCCESS);
 UINT32 before_layer=layer;stop_stalled=TRUE;
 CHECK(Handoff(&mDisplay)==EFI_TIMEOUT && !mPinned && layer==before_layer && mMode.FrameBufferBase!=(UINTN)HANDOFF_BASE);
 stop_stalled=FALSE;
 CHECK(Handoff(&mDisplay)==EFI_SUCCESS && mPinned && mMode.FrameBufferBase==HANDOFF_BASE && dma==HANDOFF_BASE);
 CHECK(pinned[10]==0x123456 && layer==before_layer && geometry[2]==1920);
 unsigned before_stops=stops;
 CHECK(Handoff(&mDisplay)==EFI_SUCCESS && stops==before_stops);
 CHECK(Blt(&mGop,NULL,EfiBltVideoToVideo,0,0,0,0,1,1,0)==EFI_SUCCESS && mMode.FrameBufferBase==HANDOFF_BASE);
 CHECK(cleans>0);
 return 0;
}
