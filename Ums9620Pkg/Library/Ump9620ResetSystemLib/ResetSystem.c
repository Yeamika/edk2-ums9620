// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/DebugLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <IndustryStandard/ArmStdSmc.h>

EFI_STATUS W202PowerOff(VOID);

VOID EFIAPI ResetCold(VOID) {
  ArmCallSmc0(ARM_SMC_ID_PSCI_SYSTEM_RESET,NULL,NULL,NULL);
}
VOID EFIAPI ResetWarm(VOID) {
  UINTN Command=ARM_SMC_ID_PSCI_SYSTEM_RESET2_AARCH64;
  if (ArmCallSmc0(ARM_SMC_ID_PSCI_FEATURES,&Command,NULL,NULL)==ARM_SMC_PSCI_RET_SUCCESS)
    ArmCallSmc0(Command,NULL,NULL,NULL);
  else ResetCold();
}
VOID EFIAPI ResetShutdown(VOID) {
  EFI_TPL Old;
  EFI_STATUS Status;
  /* Native MMIO is a pre-OS service. Keep the existing PSCI runtime path;
   * never dereference physical ADI addresses after virtual address mapping. */
  if (EfiAtRuntime()) {
    ArmCallSmc0(ARM_SMC_ID_PSCI_SYSTEM_OFF,NULL,NULL,NULL);
    return;
  }
  Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Status=W202PowerOff();
  gBS->RestoreTPL(Old);
  DEBUG((DEBUG_ERROR,"W202_SHUTDOWN returned=%r\n",Status));
}
VOID EFIAPI ResetPlatformSpecific(UINTN DataSize, VOID *ResetData) { ResetCold(); }
VOID EFIAPI ResetSystem(EFI_RESET_TYPE Type,EFI_STATUS Status,UINTN Size,VOID *Data OPTIONAL) {
  switch (Type) {
    case EfiResetCold: ResetCold(); break;
    case EfiResetWarm: ResetWarm(); break;
    case EfiResetShutdown: ResetShutdown(); break;
    case EfiResetPlatformSpecific: ResetPlatformSpecific(Size,Data); break;
    default: break;
  }
}
