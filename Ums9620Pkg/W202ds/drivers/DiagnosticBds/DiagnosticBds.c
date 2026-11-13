// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Protocol/Bds.h>
#include <Protocol/FirmwareVolume2.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include "../../Include/BootMetrics.h"
#include "../../Library/LkInheritedPlatformLib/LkTrace.h"
VOID InspectEsp (VOID);
#ifdef W202_LK_RETURN_TEST
STATIC EFI_GUID mAppGuid = {0x35daf84c,0x4c7a,0x49a3,{0x92,0x44,0xd4,0xe7,0xf2,0x8b,0x55,0xb3}};
#elif defined(W202_ANDROID_BOOT_TEST)
STATIC EFI_GUID mAppGuid = {0xa5e97492,0x26a4,0x4a82,{0xa3,0xc3,0x96,0xb3,0x88,0x26,0x95,0x20}};
#elif defined(W202_TOUCH_DIRECT)
STATIC EFI_GUID mAppGuid = {0x31f7ea83,0xe392,0x48da,{0xa1,0xf7,0xf0,0x1a,0xad,0x25,0xb1,0x7d}};
#else
STATIC EFI_GUID mAppGuid = {0xedb979d9,0x310e,0x4145,{0x95,0x0e,0xac,0x16,0x91,0xa1,0x01,0x0a}};
#endif
STATIC VOID EFIAPI Entry(EFI_BDS_ARCH_PROTOCOL *This) {
  EFI_HANDLE *Handles, App;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  UINTN Count, Index, Size;
  VOID *Data;
  UINT32 Authentication;
  EFI_STATUS Status;
  LkTraceStage (0x43);
  // Complete DXE before loading external EFI images. SecurityStub defers
  // filesystem images until this event, while FV diagnostics are exempt.
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  DEBUG ((DEBUG_WARN, "UEFI_BDS EndOfDxe signaled\n"));
  // Connect GOP to GraphicsConsole before entering the independent application.
  if (!EFI_ERROR(gBS->LocateHandleBuffer(AllHandles,NULL,NULL,&Count,&Handles))) {
    for (Index=0; Index<Count; ++Index) gBS->ConnectController(Handles[Index],NULL,NULL,TRUE);
    FreePool(Handles);
  }
  InspectEsp ();
#ifdef W202_STORAGE_TEST
  // RAM diagnostic only: preserve the storage log until the recovery watchdog.
  CpuDeadLoop ();
#endif
  gBS->SetWatchdogTimer(0,0,0,NULL);
  if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gEfiFirmwareVolume2ProtocolGuid,NULL,&Count,&Handles))) {
    for (Index=0; Index<Count; ++Index) {
      Status=gBS->HandleProtocol(Handles[Index],&gEfiFirmwareVolume2ProtocolGuid,(VOID **)&Fv);
      if (EFI_ERROR(Status)) continue;
      Data=NULL; Size=0;
      Status=Fv->ReadSection(Fv,&mAppGuid,EFI_SECTION_PE32,0,&Data,&Size,&Authentication);
      if (EFI_ERROR(Status)) continue;
      Status=gBS->LoadImage(FALSE,gImageHandle,NULL,Data,Size,&App);
      FreePool(Data);
      // A returning EFI application is unloaded by StartImage/Exit.
      if (!EFI_ERROR(Status)) {
        volatile W202_BOOT_METRICS *Metrics=(VOID *)(UINTN)W202_BOOT_METRICS_ADDRESS;
        if (Metrics->magic==W202_BOOT_METRICS_MAGIC) Metrics->app_counter=GetPerformanceCounter();
        LkTraceStage (0x44); Status=gBS->StartImage(App,NULL,NULL); LkTraceStage (0x45);
        DEBUG((DEBUG_WARN,"UEFI_BDS child returned=%r\n",Status));
      }
      break;
    }
    FreePool(Handles);
  }
  // Diagnostic-only: no unverified storage/Android or PSCI fallback.
#if defined(W202_ANDROID_BOOT_TEST) || defined(W202_LK_RETURN_TEST)
  // Preserve the result in the small retained trace until PMIC recovery.
  // Periodic board callbacks otherwise overwrite it while this loop waits.
  DisableInterrupts ();
#endif
  CpuDeadLoop();
}
STATIC EFI_BDS_ARCH_PROTOCOL mBds = {Entry};
EFI_STATUS EFIAPI Initialize(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System) {
  LkTraceStage (0x42);
  return gBS->InstallMultipleProtocolInterfaces(&Image,&gEfiBdsArchProtocolGuid,&mBds,NULL);
}
