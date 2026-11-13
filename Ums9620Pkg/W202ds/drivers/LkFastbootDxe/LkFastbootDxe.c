// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/EventGroup.h>
#include "pj4edk2_fastboot.h"
#define W202_LK_RETURN_EFI
#include "../../Include/LkFastboot.h"
#include "../../Include/LkReturn.h"

STATIC W202_LK_FASTBOOT_DESCRIPTOR mDescriptor;
STATIC BOOLEAN mActive, mExited;
STATIC EFI_EVENT mExit;
STATIC PJ4EDK2_FASTBOOT_PROTOCOL mProtocol;

STATIC EFI_STATUS Call(UINT64 Operation, UINT32 *Flags) {
  EFI_TPL Old;
  UINT64 Result;
  typedef UINT64 (*GATEWAY)(UINT64);
  if (mExited) return EFI_UNSUPPORTED;
  Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  Result=((GATEWAY)(UINTN)mDescriptor.entry)(Operation);
  gBS->RestoreTPL(Old);
  if (Result==MAX_UINT64) return EFI_DEVICE_ERROR;
  if (Result & ~(UINT64)(PJ4EDK2_FASTBOOT_BUSY|PJ4EDK2_FASTBOOT_CONTINUE|
                        PJ4EDK2_FASTBOOT_REBOOT|PJ4EDK2_FASTBOOT_BOOTLOADER))
    return EFI_PROTOCOL_ERROR;
  if (Flags!=NULL) *Flags=(UINT32)Result;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Start(PJ4EDK2_FASTBOOT_PROTOCOL *This) {
  EFI_STATUS Status;
  if (This!=&mProtocol) return EFI_INVALID_PARAMETER;
  if (mExited) return EFI_UNSUPPORTED;
  if (mActive) return EFI_SUCCESS;
  Status=Call(W202_LK_FB_START,NULL);
  if (!EFI_ERROR(Status)) mActive=TRUE;
  return Status;
}
STATIC EFI_STATUS EFIAPI Poll(PJ4EDK2_FASTBOOT_PROTOCOL *This,UINT32 *Flags) {
  if (This!=&mProtocol || Flags==NULL) return EFI_INVALID_PARAMETER;
  *Flags=0;
  if (!mActive || mExited) return EFI_NOT_READY;
  return Call(W202_LK_FB_POLL,Flags);
}
STATIC EFI_STATUS EFIAPI Stop(PJ4EDK2_FASTBOOT_PROTOCOL *This) {
  EFI_STATUS Status;
  if (This!=&mProtocol) return EFI_INVALID_PARAMETER;
  if (!mActive) return EFI_SUCCESS;
  Status=Call(W202_LK_FB_STOP,NULL);
  if (!EFI_ERROR(Status)) mActive=FALSE;
  return Status;
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  /* A normal menu handoff already stopped USB. Cover direct EFI clients. */
  Stop(&mProtocol);
  mExited=TRUE;
}
EFI_STATUS EFIAPI Initialize(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  EFI_GUID Guid=PJ4EDK2_FASTBOOT_GUID;
  W202_LK_RETURN Return;
  EFI_STATUS Status;
  (VOID)System;
  CopyMem(&Return,(VOID *)(UINTN)W202_LK_RETURN_ADDRESS,sizeof(Return));
  CopyMem(&mDescriptor,(VOID *)(UINTN)W202_LK_FB_ADDRESS,sizeof(mDescriptor));
  if (!W202LkReturnValid(&Return) || !W202LkFastbootValid(&mDescriptor))
    return EFI_UNSUPPORTED; // Optional service absent in older LK builds.
  mProtocol=(PJ4EDK2_FASTBOOT_PROTOCOL){PJ4EDK2_FASTBOOT_REVISION,sizeof(mProtocol),Start,Poll,Stop};
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,
                          &gEfiEventExitBootServicesGuid,&mExit);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->InstallMultipleProtocolInterfaces(&Image,&Guid,&mProtocol,NULL);
  if (EFI_ERROR(Status)) gBS->CloseEvent(mExit);
  return Status;
}
