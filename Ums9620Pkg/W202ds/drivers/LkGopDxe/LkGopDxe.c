// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>
#include <Library/CacheMaintenanceLib.h>
#include "pj4edk2_display.h"
#include "../../Library/LkInheritedPlatformLib/LkTrace.h"
// One public landscape mode. DPU owns rotation; no portrait compatibility mode.
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mNativeInfo = {
  0, 1920, 1200, PixelBlueGreenRedReserved8BitPerColor, {0,0,0,0}, 1920
};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mMode = {1,0,&mNativeInfo,sizeof(mNativeInfo),0,1920*1200*4};
STATIC FRAME_BUFFER_CONFIGURE *mConfigure;
STATIC UINTN mConfigurationSize;
STATIC EFI_PHYSICAL_ADDRESS mNativeBase,mAlternateBase,mFrontBase;
STATIC UINT32 mDpuCfg;
STATIC UINTN mRefreshCount;
STATIC BOOLEAN mPinned;
#define HANDOFF_BASE 0xb0000000ULL
#define NATIVE_BYTES (1920*1200*4)
/* DPU r6p0, inherited panel/DSI/rotation. Explicitly latch updated layer
 * state for streaming scanout and trigger single-run scanout when needed.
 * Offsets/trigger bits: pinned sprd LK dpu_r6p0.c and W202 DRM driver.
 * Poll raw status: EDK2 does not own the inherited display interrupt. */
#define DPU_REG(Offset) (0x31000000 + (Offset))
STATIC EFI_STATUS Refresh(VOID) {
  UINT32 Ctrl, Raw, Done;
  UINTN Count;
  BOOLEAN Single = (mDpuCfg & (BIT0 | BIT1)) != 0;
  Done = Single ? BIT0 : BIT18;
  MmioWrite32(DPU_REG(0x254), Done | BIT16 | BIT18 | BIT2);
  Ctrl = MmioRead32(DPU_REG(0x08)) & ~BIT1;
  MemoryFence();
  MmioWrite32(DPU_REG(0x08), Ctrl | BIT2 | BIT4);
  if (Single || !(Ctrl & BIT0))
    MmioWrite32(DPU_REG(0x08), MmioRead32(DPU_REG(0x08)) | BIT0);
  for (Count=0; Count<1500; ++Count) {
    Raw=MmioRead32(DPU_REG(0x25c));
    if (Raw & Done) {
      if (mRefreshCount++<3)
        DEBUG((DEBUG_WARN,"W202_GOP presented=%u raw=%08x wait_us=%u\n",mRefreshCount,Raw,Count*100));
      return EFI_SUCCESS;
    }
    MicroSecondDelay(100);
  }
  DEBUG((DEBUG_WARN,"W202_GOP timeout raw=%08x ctrl=%08x cfg=%08x\n",Raw,MmioRead32(DPU_REG(0x08)),mDpuCfg));
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,UINT32 Mode,UINTN *Size,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info) {
  if (!This || !Size || !Info) return EFI_INVALID_PARAMETER;
  if (Mode) return EFI_UNSUPPORTED;
  *Info=AllocateCopyPool(sizeof(mNativeInfo),&mNativeInfo);
  if (!*Info) return EFI_OUT_OF_RESOURCES;
  *Size=sizeof(mNativeInfo);return EFI_SUCCESS;
}
STATIC EFI_STATUS Quiesce(VOID) {
  UINTN Count;
  if (mDpuCfg&(BIT0|BIT1)) return Refresh();
  // DPI streaming keeps reading the framebuffer between updates. Stop it
  // before changing format or buffers, as the native r6p0 driver does.
  MmioWrite32(DPU_REG(0x254),BIT0);
  MemoryFence();
  MmioWrite32(DPU_REG(8),MmioRead32(DPU_REG(8))|BIT1);
  for (Count=0;Count<1500;++Count) {
    if ((MmioRead32(DPU_REG(0x25c))&BIT0) &&
        !((MmioRead32(DPU_REG(0x754))|MmioRead32(DPU_REG(0x758)))&BIT15)) {
      // Refresh must issue RUN, even if the inherited RUN bit read as set.
      MmioWrite32(DPU_REG(8),MmioRead32(DPU_REG(8))&~(BIT0|BIT1));
      return EFI_SUCCESS;
    }
    MicroSecondDelay(100);
  }
  DEBUG((DEBUG_WARN,"W202_GOP stop timeout raw=%08x ctrl=%08x\n",MmioRead32(DPU_REG(0x25c)),MmioRead32(DPU_REG(8))));
  // No pixels or geometry changed. Resume the inherited scanout if possible.
  Refresh();
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS EFIAPI Set(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,UINT32 Mode) {
  if (!This) return EFI_INVALID_PARAMETER;
  return Mode?EFI_UNSUPPORTED:Refresh();
}
/* Freeze the final landscape frame into the board's reserved logo RAM.
 * Only a same-layout copy is needed: no mode, format or pixel rotation change.
 * The menu's private double buffers can then be reclaimed by the next stage.
 */
STATIC EFI_STATUS EFIAPI Handoff(PJ4EDK2_DISPLAY_PROTOCOL *This) {
  EFI_STATUS Status;
  UINTN Size=mConfigurationSize;
  if (!This) return EFI_INVALID_PARAMETER;
  if (mPinned) return EFI_SUCCESS;
  Status=Quiesce();if (EFI_ERROR(Status)) return Status;
  CopyMem((VOID *)(UINTN)HANDOFF_BASE,(VOID *)(UINTN)mFrontBase,NATIVE_BYTES);
  WriteBackDataCacheRange((VOID *)(UINTN)HANDOFF_BASE,NATIVE_BYTES);
  MemoryFence();
  MmioWrite32(DPU_REG(0x30),HANDOFF_BASE);
  mFrontBase=mMode.FrameBufferBase=HANDOFF_BASE;
  Status=FrameBufferBltConfigure((VOID *)(UINTN)HANDOFF_BASE,&mNativeInfo,mConfigure,&Size);
  if (EFI_ERROR(Status)) return Status;
  mPinned=TRUE;
  return Refresh();
}
STATIC EFI_STATUS Present(UINTN X,UINTN Y,UINTN W,UINTN H) {
  EFI_STATUS Status;
  UINTN Row,Size;
  EFI_PHYSICAL_ADDRESS Draw=mMode.FrameBufferBase,Previous=mFrontBase;
  if (mPinned) return Refresh();
  // Latch the completed back buffer at a frame boundary. The old scanout
  // becomes writable only after UPDATE_DONE, never while DPU still reads it.
  MmioWrite32(DPU_REG(0x30),(UINT32)Draw);
  mFrontBase=Draw;
  Status=Refresh();
  if (EFI_ERROR(Status)) return Status;
  mMode.FrameBufferBase=Previous;
  Size=mConfigurationSize;
  Status=FrameBufferBltConfigure((VOID *)(UINTN)Previous,&mNativeInfo,mConfigure,&Size);
  if (EFI_ERROR(Status)) return Status;
  // Retain partial redraw semantics in both buffers. Only changed rows are
  // copied, after the old front buffer is no longer being scanned out.
  for (Row=Y;Row<Y+H;++Row)
    CopyMem((VOID *)(UINTN)(Previous+(Row*1920+X)*4),
      (VOID *)(UINTN)(Draw+(Row*1920+X)*4),W*4);
  WriteBackDataCacheRange((VOID *)(UINTN)(Previous+(Y*1920+X)*4),((H-1)*1920+W)*4);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer, EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op, UINTN Sx, UINTN Sy, UINTN Dx, UINTN Dy, UINTN W, UINTN H, UINTN Delta) {
  EFI_STATUS Status;
  if (!This) return EFI_INVALID_PARAMETER;
  // Direct framebuffer clients use a self BLT to request presentation. Avoid
  // copying the same pixels, but retain bounds checking and cache maintenance.
  if (Op==EfiBltVideoToVideo && Sx==Dx && Sy==Dy) {
    UINTN Width=mMode.Info->HorizontalResolution,Height=mMode.Info->VerticalResolution;
    if (!W || !H || Dx>=Width || Dy>=Height || W>Width-Dx || H>Height-Dy)
      return EFI_INVALID_PARAMETER;
    Status=EFI_SUCCESS;
  } else Status = FrameBufferBlt(mConfigure, Buffer, Op, Sx, Sy, Dx, Dy, W, H, Delta);
  MemoryFence();
  if (!EFI_ERROR(Status) && Op != EfiBltVideoToBltBuffer) {
#ifdef W202_DISPLAY_TEST
    STATIC UINT64 LastSecond=MAX_UINT64;
    UINT64 Second=GetTimeInNanoSecond(GetPerformanceCounter())/1000000000;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color={0,0,0,0};
    Color.Red=(Second&1)?255:0; Color.Green=(Second&1)?0:255; Color.Blue=255;
    FrameBufferBlt(mConfigure,&Color,EfiBltVideoFill,0,0,0,0,mMode.Info->HorizontalResolution,64,0);
    if (Second!=LastSecond) {
      UINT32 Hash=2166136261U;
      UINTN I;
      volatile UINT8 *Pixels=(volatile UINT8 *)(UINTN)mMode.FrameBufferBase;
      for (I=64*mMode.Info->PixelsPerScanLine*4;I<mMode.FrameBufferSize;++I) Hash=(Hash^Pixels[I])*16777619U;
      DEBUG((DEBUG_WARN,"W202_STRIPE sec=%lu pixel=%04x ui_hash=%08x ctrl=%x raw=%x\n",Second,Pixels[0],Hash,MmioRead32(DPU_REG(8)),MmioRead32(DPU_REG(0x25c))));
      LastSecond=Second;
    }
#endif
#ifdef W202_DISPLAY_TEST
    WriteBackDataCacheRange((VOID *)(UINTN)mMode.FrameBufferBase,mMode.FrameBufferSize);
#else
    // Only the destination rectangle was modified. Include inter-row padding
    // in a single bounded cache operation; don't clean 4.6 MB for one icon.
    if (W && H)
      WriteBackDataCacheRange((VOID *)(UINTN)(mMode.FrameBufferBase+(Dy*mMode.Info->PixelsPerScanLine+Dx)*4),
                             ((H-1)*mMode.Info->PixelsPerScanLine+W)*4);
#endif
    Status=Present(Dx,Dy,W,H);
  }
  return Status;
}
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mGop = {Query, Set, Blt, &mMode};
STATIC UINT32 EFIAPI Rotation(PJ4EDK2_DISPLAY_PROTOCOL *This,UINT32 Mode) {
  if (!This || This->gop!=&mGop || Mode) return MAX_UINT32;
  return 0; // AbsolutePointer is already normalized to this landscape GOP.
}
STATIC PJ4EDK2_DISPLAY_PROTOCOL mDisplay={PJ4EDK2_DISPLAY_REVISION,sizeof(mDisplay),0,&mGop,Rotation,Handoff};
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
   {0xea8cbfd1,0x714f,0x4794,{0x98,0x52,0xcb,0xb7,0x5c,0x37,0x90,0x10}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}
};
EFI_STATUS EFIAPI Initialize(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  EFI_HANDLE Handle=NULL;
  EFI_GUID DisplayGuid=PJ4EDK2_DISPLAY_GUID;
  EFI_PHYSICAL_ADDRESS Address=MAX_UINT32;
  EFI_STATUS Status;
  UINTN Size=0;
  UINT32 Layer;
  LkTraceStage(0x40);
  mDpuCfg=MmioRead32(DPU_REG(0x0c));
  Layer=MmioRead32(DPU_REG(0x40));
  if (MmioRead32(DPU_REG(0))!=0x600 || MmioRead32(DPU_REG(0x18))!=0x078004b0 ||
      MmioRead32(DPU_REG(0x2c))!=1 || MmioRead32(DPU_REG(0x30))!=HANDOFF_BASE ||
      (Layer&0x007000f0)!=0x00200040 || MmioRead32(DPU_REG(0x4c))!=1200)
    return EFI_UNSUPPORTED;
  Status=gBS->AllocatePages(AllocateMaxAddress,EfiBootServicesData,
    2*EFI_SIZE_TO_PAGES(NATIVE_BYTES),&Address);
  if (EFI_ERROR(Status)) return Status;
  mNativeBase=Address;mAlternateBase=Address+EFI_PAGES_TO_SIZE(EFI_SIZE_TO_PAGES(NATIVE_BYTES));
  SetMem((VOID *)(UINTN)mNativeBase,2*EFI_PAGES_TO_SIZE(EFI_SIZE_TO_PAGES(NATIVE_BYTES)),0);
  mFrontBase=mNativeBase;mMode.FrameBufferBase=mAlternateBase;
  Status=FrameBufferBltConfigure((VOID *)(UINTN)mAlternateBase,&mNativeInfo,NULL,&Size);
  if (Status!=RETURN_BUFFER_TOO_SMALL) return EFI_UNSUPPORTED;
  mConfigurationSize=Size;mConfigure=AllocatePool(Size);
  if (!mConfigure) return EFI_OUT_OF_RESOURCES;
  Status=FrameBufferBltConfigure((VOID *)(UINTN)mAlternateBase,&mNativeInfo,mConfigure,&Size);
  if (EFI_ERROR(Status)) return Status;
  Status=Quiesce();if (EFI_ERROR(Status)) return Status;
  WriteBackDataCacheRange((VOID *)(UINTN)mNativeBase,NATIVE_BYTES);
  MmioWrite32(DPU_REG(0x44),0x04b00780);MmioWrite32(DPU_REG(0x48),0x04b00780);
  MmioWrite32(DPU_REG(0x4c),1920);MmioWrite32(DPU_REG(0x50),0);MmioWrite32(DPU_REG(0x60),0);
  MmioWrite32(DPU_REG(0x54),0xff);
  MmioWrite32(DPU_REG(0x40),(Layer&~(0x007007f0U|BIT24))|0x00100030|BIT2);
  MmioWrite32(DPU_REG(0x30),(UINT32)mFrontBase);
  Status=Refresh();if (EFI_ERROR(Status)) return Status;
  Status=gBS->InstallMultipleProtocolInterfaces(&Handle,&gEfiGraphicsOutputProtocolGuid,&mGop,
    &gEfiDevicePathProtocolGuid,&mPath,&DisplayGuid,&mDisplay,NULL);
  if (!EFI_ERROR(Status)) LkTraceStage(0x41);
  return Status;
}
