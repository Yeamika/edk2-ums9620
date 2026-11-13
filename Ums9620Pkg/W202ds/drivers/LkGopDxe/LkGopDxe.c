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
#include "../../Library/LkInheritedPlatformLib/LkTrace.h"
// Keep inherited panel/DSI setup. Mode 0 remains the EFI/OS handoff ABI.
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mInfo = {
  0, 1200, 1920, PixelBitMask, {0xf800, 0x07e0, 0x001f, 0}, 1200
};
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mNativeInfo = {
  0, 1920, 1200, PixelBlueGreenRedReserved8BitPerColor, {0, 0, 0, 0}, 1920
};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mMode = {2, 0, &mInfo, sizeof(mInfo), 0xb0000000, 1200 * 1920 * 2};
STATIC FRAME_BUFFER_CONFIGURE *mConfigure;
STATIC FRAME_BUFFER_CONFIGURE *mConfigurations[2];
STATIC UINT32 mInheritedLayer, mInheritedAlpha;
STATIC UINT32 mInheritedGeometry[5];
STATIC CONST UINTN mGeometryRegisters[5]={0x44,0x48,0x4c,0x50,0x60};
STATIC UINT32 mDpuCfg;
STATIC UINTN mRefreshCount;
/* DPU r6p0, inherited panel/DSI/rotation. The LK page uses command-mode
 * single-run, so writing its framebuffer alone does not present a frame.
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
STATIC EFI_STATUS EFIAPI Query(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode, UINTN *Size, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info) {
  if (!This || !Size || !Info) return EFI_INVALID_PARAMETER;
  if (Mode >= mMode.MaxMode) return EFI_UNSUPPORTED;
  *Info = AllocateCopyPool(sizeof(mInfo), Mode ? &mNativeInfo : &mInfo);
  if (!*Info) return EFI_OUT_OF_RESOURCES;
  *Size = sizeof(mInfo); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Set(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode) {
  EFI_STATUS Status;
  UINTN I, X, Y;
  UINT16 *Temporary;
  volatile UINT16 *Short=(volatile UINT16 *)(UINTN)mMode.FrameBufferBase;
  volatile UINT32 *Wide=(volatile UINT32 *)(UINTN)mMode.FrameBufferBase;
  if (!This) return EFI_INVALID_PARAMETER;
  if (Mode>=mMode.MaxMode) return EFI_UNSUPPORTED;
  if (Mode==mMode.Mode) return Refresh();
  // Rotated packing needs scratch only at mode/EFI boundaries, never per frame.
  Temporary=AllocatePool(1200*1920*2);
  if (!Temporary) return EFI_OUT_OF_RESOURCES;
  /* Only expose mode 1 on inherited single-run scanout. Finish the previous
   * frame before changing the in-place storage or layer interpretation. */
  Status=Refresh();
  if (EFI_ERROR(Status)) {FreePool(Temporary);return Status;}
  if (Mode) {
    for (I=0;I<1200*1920;++I) Temporary[I]=Short[I];
    for (Y=0;Y<1200;++Y) for (X=0;X<1920;++X) {
      UINT32 V=Temporary[X*1200+1199-Y],R=(V>>11)&31,G=(V>>5)&63,B=V&31;
      Wide[Y*1920+X]=(((R<<3)|(R>>2))<<16)|(((G<<2)|(G>>4))<<8)|((B<<3)|(B>>2));
    }
  } else {
    // Compatibility conversion occurs only when leaving the menu, not on
    // every UI frame. Reconstruct the physical portrait layout for old EFI.
    for (Y=0;Y<1200;++Y) for (X=0;X<1920;++X) {
      UINT32 V=Wide[Y*1920+X];
      Temporary[X*1200+1199-Y]=(UINT16)(((V>>8)&0xf800)|((V>>5)&0x07e0)|((V>>3)&0x001f));
    }
    for (I=0;I<1200*1920;++I) Short[I]=Temporary[I];
  }
  FreePool(Temporary);
  WriteBackDataCacheRange((VOID *)(UINTN)mMode.FrameBufferBase,1200*1920*(Mode?4:2));
  MemoryFence();
  /* r6p0 code 1 = DRM counter-clockwise 90. This composes the previous CPU
   * clockwise 90 with the board's inherited 180 mounting. Android r6p0
   * dpu_layer() swaps destination dimensions for quarter turns: both layer
   * sizes hold 1920x1200, with scaling disabled. Panel/DSI stay 1200x1920. */
  for (I=0;I<5;++I)
    MmioWrite32(DPU_REG(mGeometryRegisters[I]),Mode?
      (I<2?0x04b00780:(I==2?1920:0)):mInheritedGeometry[I]);
  MmioWrite32(DPU_REG(0x54),Mode?0xff:mInheritedAlpha);
  MmioWrite32(DPU_REG(0x40),Mode?((mInheritedLayer&~(0x007007f0U|BIT24))|0x00100030|BIT2):mInheritedLayer);
  mMode.Mode=Mode;mMode.Info=Mode?&mNativeInfo:&mInfo;
  mMode.FrameBufferSize=1200*1920*(Mode?4:2);mConfigure=mConfigurations[Mode];
  // Record the actual mode before presenting: callers can roll back even if
  // the hardware times out after accepting the new register values.
  return Refresh();
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
      for (I=64*mMode.Info->PixelsPerScanLine*(mMode.Mode?4:2);I<mMode.FrameBufferSize;++I) Hash=(Hash^Pixels[I])*16777619U;
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
      WriteBackDataCacheRange((VOID *)(UINTN)(mMode.FrameBufferBase+(Dy*mMode.Info->PixelsPerScanLine+Dx)*(mMode.Mode?4:2)),
                             ((H-1)*mMode.Info->PixelsPerScanLine+W)*(mMode.Mode?4:2));
#endif
    Status=Refresh();
  }
  return Status;
}
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mGop = {Query, Set, Blt, &mMode};
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
   {0xea8cbfd1,0x714f,0x4794,{0x98,0x52,0xcb,0xb7,0x5c,0x37,0x90,0x10}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}
};
EFI_STATUS EFIAPI Initialize(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System) {
  EFI_HANDLE Handle = NULL;
  UINTN Size = 0, Mode;
  EFI_STATUS Status;
  LkTraceStage (0x40);
  mDpuCfg=MmioRead32(DPU_REG(0x0c));
  DEBUG((DEBUG_WARN,"W202_DSI reset=%x mode=%x format=%x phy=%x lanes=%x vactive=%x vblank=%x mmu=%x bypass=%x\n",
    MmioRead32(0x31300004),MmioRead32(0x31300018),MmioRead32(0x31300020),MmioRead32(0x3130009c),
    MmioRead32(0x313000a4),MmioRead32(0x31300034),MmioRead32(0x31300030),MmioRead32(DPU_REG(0x1804)),MmioRead32(DPU_REG(0x18a0))));
  DEBUG((DEBUG_WARN,"W202_GOP version=%08x mode=%08x cfg=%08x ctrl=%08x size=%08x enable=%08x addr=%08x layer=%08x pitch=%u raw=%08x\n",
    MmioRead32(DPU_REG(0)),MmioRead32(DPU_REG(4)),mDpuCfg,MmioRead32(DPU_REG(8)),
    MmioRead32(DPU_REG(0x18)),MmioRead32(DPU_REG(0x2c)),MmioRead32(DPU_REG(0x30)),
    MmioRead32(DPU_REG(0x40)),MmioRead32(DPU_REG(0x4c)),MmioRead32(DPU_REG(0x25c))));
  if (MmioRead32(DPU_REG(0))!=0x600 ||
      MmioRead32(DPU_REG(0x18))!=0x078004b0 ||
      MmioRead32(DPU_REG(0x2c))!=1 ||
      MmioRead32(DPU_REG(0x30))!=mMode.FrameBufferBase ||
      (MmioRead32(DPU_REG(0x40)) & 0x007000f0)!=0x00200040 ||
      MmioRead32(DPU_REG(0x4c))!=1200) return EFI_UNSUPPORTED;
  mInheritedLayer=MmioRead32(DPU_REG(0x40));mInheritedAlpha=MmioRead32(DPU_REG(0x54));
  for (Mode=0;Mode<5;++Mode) mInheritedGeometry[Mode]=MmioRead32(DPU_REG(mGeometryRegisters[Mode]));
  // Streaming scanout cannot safely be repacked in place; keep its old GOP.
  if (!(mDpuCfg&(BIT0|BIT1)) || mInheritedGeometry[0]!=0x078004b0 ||
      mInheritedGeometry[1]!=0x078004b0 || mInheritedGeometry[2]!=1200 ||
      mInheritedGeometry[3] || mInheritedGeometry[4]) mMode.MaxMode=1;
  for (Mode=0;Mode<mMode.MaxMode;++Mode) {
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=Mode?&mNativeInfo:&mInfo;
    Size=0;
    Status=FrameBufferBltConfigure((VOID *)(UINTN)mMode.FrameBufferBase,Info,NULL,&Size);
    if (Status!=RETURN_BUFFER_TOO_SMALL) { Status=EFI_UNSUPPORTED;break; }
    mConfigurations[Mode]=AllocatePool(Size);
    if (!mConfigurations[Mode]) { Status=EFI_OUT_OF_RESOURCES;break; }
    Status=FrameBufferBltConfigure((VOID *)(UINTN)mMode.FrameBufferBase,Info,mConfigurations[Mode],&Size);
    if (EFI_ERROR(Status)) break;
  }
  mConfigure=mConfigurations[0];
  if (!EFI_ERROR(Status)) Status = gBS->InstallMultipleProtocolInterfaces(&Handle, &gEfiGraphicsOutputProtocolGuid, &mGop, &gEfiDevicePathProtocolGuid, &mPath, NULL);
  if (!EFI_ERROR(Status)) LkTraceStage (0x41);
  if (EFI_ERROR(Status)) {
    for (Mode=0;Mode<2;++Mode) if (mConfigurations[Mode]) FreePool(mConfigurations[Mode]);
  }
  return Status;
}
