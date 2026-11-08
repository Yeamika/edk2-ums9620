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
// Inherited scanout only. No panel power/DSI setup or undocumented LK calls.
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mInfo = {
  0, 1200, 1920, PixelBitMask, {0xf800, 0x07e0, 0x001f, 0}, 1200
};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mMode = {1, 0, &mInfo, sizeof(mInfo), 0xb0000000, 1200 * 1920 * 2};
STATIC FRAME_BUFFER_CONFIGURE *mConfigure;
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
  if (Mode != 0) return EFI_UNSUPPORTED;
  *Info = AllocateCopyPool(sizeof(mInfo), &mInfo);
  if (!*Info) return EFI_OUT_OF_RESOURCES;
  *Size = sizeof(mInfo); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Set(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, UINT32 Mode) {
  if (!This) return EFI_INVALID_PARAMETER;
  return Mode == 0 ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer, EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op, UINTN Sx, UINTN Sy, UINTN Dx, UINTN Dy, UINTN W, UINTN H, UINTN Delta) {
  EFI_STATUS Status;
  if (!This) return EFI_INVALID_PARAMETER;
  Status = FrameBufferBlt(mConfigure, Buffer, Op, Sx, Sy, Dx, Dy, W, H, Delta);
  MemoryFence();
  if (!EFI_ERROR(Status) && Op != EfiBltVideoToBltBuffer) {
#ifdef W202_DISPLAY_TEST
    STATIC UINT64 LastSecond=MAX_UINT64;
    UINT64 Second=GetTimeInNanoSecond(GetPerformanceCounter())/1000000000;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color={0,0,0,0};
    Color.Red=(Second&1)?255:0; Color.Green=(Second&1)?0:255; Color.Blue=255;
    FrameBufferBlt(mConfigure,&Color,EfiBltVideoFill,0,0,0,0,1200,64,0);
    if (Second!=LastSecond) {
      UINT32 Hash=2166136261U;
      UINTN I;
      volatile UINT16 *Pixels=(volatile UINT16 *)(UINTN)mMode.FrameBufferBase;
      for (I=64*1200;I<1200*1920;++I) Hash=(Hash^Pixels[I])*16777619U;
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
      WriteBackDataCacheRange((VOID *)(UINTN)(mMode.FrameBufferBase+(Dy*1200+Dx)*2),
                             ((H-1)*1200+W)*2);
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
  UINTN Size = 0;
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
  Status = FrameBufferBltConfigure((VOID *)(UINTN)mMode.FrameBufferBase, &mInfo, NULL, &Size);
  if (Status != RETURN_BUFFER_TOO_SMALL) return EFI_UNSUPPORTED;
  mConfigure = AllocatePool(Size);
  if (!mConfigure) return EFI_OUT_OF_RESOURCES;
  Status = FrameBufferBltConfigure((VOID *)(UINTN)mMode.FrameBufferBase, &mInfo, mConfigure, &Size);
  if (!EFI_ERROR(Status)) Status = gBS->InstallMultipleProtocolInterfaces(&Handle, &gEfiGraphicsOutputProtocolGuid, &mGop, &gEfiDevicePathProtocolGuid, &mPath, NULL);
  if (!EFI_ERROR(Status)) LkTraceStage (0x41);
  if (EFI_ERROR(Status)) FreePool(mConfigure);
  return Status;
}
