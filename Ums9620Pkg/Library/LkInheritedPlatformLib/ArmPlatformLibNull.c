/** @file

  Copyright (c) 2011-2012, ARM Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/ArmLib.h>
#include <Library/ArmPlatformLib.h>
#include <Library/HobLib.h>

#include <Ppi/ArmMpCoreInfo.h>
#include "LkTrace.h"

// The LK handoff currently enters on the boot CPU only; AP startup is not supported.
STATIC ARM_CORE_INFO  mBootCoreInfo[] = {
  { 0, 0, 0, 0, 0 }
};

STATIC
EFI_STATUS
EFIAPI
GetBootCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **CoreTable
  )
{
  LkTraceStage (0x34);
  *CoreCount = 1;
  *CoreTable = mBootCoreInfo;
  return EFI_SUCCESS;
}

STATIC ARM_MP_CORE_INFO_PPI  mMpCoreInfoPpi = { GetBootCoreInfo };
STATIC EFI_PEI_PPI_DESCRIPTOR  mPlatformPpiTable[] = {
  { EFI_PEI_PPI_DESCRIPTOR_PPI, &gArmMpCoreInfoPpiGuid, &mMpCoreInfoPpi }
};

/**
  Return the current Boot Mode

  This function returns the boot reason on the platform

**/
EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  LkTraceStage (0x35);
  // These shared pages are outside the smaller FD. Keep DXE's page allocator
  // from reusing calibration, boot metrics and the RAM serial log.
  BuildMemoryAllocationHob (0x82ffe000, 0x2000, EfiReservedMemoryType);
  return BOOT_WITH_FULL_CONFIGURATION;
}

/**
  Initialize controllers that must setup in the normal world

  This function is called by the ArmPlatformPkg/PrePi or ArmPlatformPkg/PlatformPei
  in the PEI phase.

**/
RETURN_STATUS
ArmPlatformInitialize (
  IN  UINTN  MpId
  )
{
  if (!ArmPlatformIsPrimaryCore (MpId)) {
    return RETURN_SUCCESS;
  }

  // DDR, clocks and DPU must remain initialized by the LK handoff.
  LkTraceStage (0x31);

  return RETURN_SUCCESS;
}

VOID
ArmPlatformGetPlatformPpiList (OUT UINTN *Size, OUT EFI_PEI_PPI_DESCRIPTOR **List)
{
  LkTraceStage (0x33);
  *Size = sizeof (mPlatformPpiTable);
  *List = mPlatformPpiTable;
}
