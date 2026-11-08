// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"
#include <Protocol/SimpleTextIn.h>
#include <Guid/ConsoleInDevice.h>
#include <Library/DebugLib.h>

/* Inherited LK input masks; no pinmux, PMIC configuration or LK calls.
 * W202: volume+ = analogue EIC 164 (high), volume- = GPIO 7 (low),
 * power/Enter = analogue EIC 161 (low).
 * Read through the same bounded ADI transport used by the fuel gauge. */
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mKeys;
STATIC BOOLEAN mInstalled, mEnabled, mKeyExited, mSeeded;
STATIC UINT32 mCandidate, mStable;
STATIC UINT64 mCandidateAt;
STATIC EFI_INPUT_KEY mKeyQueue[16];
STATIC UINTN mKeyHead, mKeyCount;

VOID W202KeysEnable(BOOLEAN Enable) {
  mEnabled=Enable && mInstalled && !mKeyExited;
  mKeyHead=mKeyCount=0; mSeeded=FALSE;
}
VOID W202KeysExit(VOID) { mKeyExited=TRUE; W202KeysEnable(FALSE); }
STATIC VOID EFIAPI KeyWait(EFI_EVENT Event, VOID *Context) {
  if (mEnabled && mKeyCount) gBS->SignalEvent(Event);
}
STATIC EFI_STATUS EFIAPI KeyReset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Extended) {
  EFI_TPL Old;
  if (This!=&mKeys) return EFI_INVALID_PARAMETER;
  if (mKeyExited) return EFI_UNSUPPORTED;
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  W202KeysEnable(mEnabled);
  gBS->RestoreTPL(Old); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI KeyRead(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key) {
  STATIC BOOLEAN Logged;
  EFI_TPL Old;
  EFI_STATUS Status=EFI_NOT_READY;
  if (This!=&mKeys || !Key) return EFI_INVALID_PARAMETER;
  if (mKeyExited) return EFI_UNSUPPORTED;
  if (!Logged) { DEBUG((DEBUG_WARN,"W202_KEYS consumer connected\n")); Logged=TRUE; }
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  if (mEnabled && mKeyCount) {
    *Key=mKeyQueue[mKeyHead]; mKeyHead=(mKeyHead+1)%16; --mKeyCount;
    DEBUG((DEBUG_WARN,"W202_KEY delivered=%u unicode=%u\n",Key->ScanCode,Key->UnicodeChar));
    Status=EFI_SUCCESS;
  }
  gBS->RestoreTPL(Old); return Status;
}
VOID W202KeysPoll(VOID) {
  UINT32 Analog, Raw, Pressed, Bit;
  UINT64 Now;
  if (!mEnabled) return;
  if (EFI_ERROR(W202AdiRead(0x64420280,&Analog))) { mSeeded=FALSE; return; }
  Raw=((Analog & BIT4)?1:0) | ((MmioRead32(0x64170000)&BIT7)?0:2) |
      ((Analog & BIT1)?0:4);
  Now=GetTimeInNanoSecond(GetPerformanceCounter())/1000000;
  if (!mSeeded) { mCandidate=mStable=Raw; mCandidateAt=Now; mSeeded=TRUE; return; }
  if (Raw!=mCandidate) { mCandidate=Raw; mCandidateAt=Now; return; }
  if (Raw==mStable || Now-mCandidateAt<30) return;
  Pressed=Raw & ~mStable; mStable=Raw;
  for (Bit=0; Bit<3; ++Bit) if ((Pressed & (1U<<Bit)) && mKeyCount<16) {
    EFI_INPUT_KEY *Key=&mKeyQueue[(mKeyHead+mKeyCount++)%16];
    Key->ScanCode=Bit==2?SCAN_NULL:(Bit?SCAN_DOWN:SCAN_UP);
    Key->UnicodeChar=Bit==2?L'\r':0;
    DEBUG((DEBUG_WARN,"W202_KEY scan=%u unicode=%u\n",Key->ScanCode,Key->UnicodeChar));
#ifdef W202_TOUCH_IRQ_DIAG
    /* Diagnostic build only: one sample per debounced press, never on held
     * key ticks. This samples independently of the touch IRQ notification. */
    W202TouchProbeKey();
#endif
    gBS->SignalEvent(mKeys.WaitForKey);
  }
}
EFI_STATUS W202KeysInit(VOID) {
  EFI_HANDLE Handle=NULL;
  EFI_STATUS Status;
  UINT32 Mask;
  Status=W202AdiRead(0x64420284,&Mask);
  if (EFI_ERROR(Status)) return Status;
  DEBUG((DEBUG_WARN,"W202_KEYS eic_mask=%x gpio_mask=%x gpio_dir=%x\n",Mask,MmioRead32(0x64170004),MmioRead32(0x64170008)));
  if ((Mask & (BIT1|BIT4)) != (BIT1|BIT4) || !(MmioRead32(0x64170004)&BIT7) ||
      (MmioRead32(0x64170008)&BIT7)) return EFI_UNSUPPORTED;
  mKeys.Reset=KeyReset; mKeys.ReadKeyStroke=KeyRead;
  Status=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_CALLBACK,KeyWait,NULL,&mKeys.WaitForKey);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->InstallMultipleProtocolInterfaces(&Handle,&gEfiSimpleTextInProtocolGuid,&mKeys,
                                               &gEfiConsoleInDeviceGuid,NULL,NULL);
  if (EFI_ERROR(Status)) { gBS->CloseEvent(mKeys.WaitForKey); return Status; }
  mInstalled=TRUE; W202KeysEnable(TRUE); return EFI_SUCCESS;
}
