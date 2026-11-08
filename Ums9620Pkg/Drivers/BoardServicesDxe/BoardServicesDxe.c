// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"
#include <Library/DebugLib.h>
#include <Guid/EventGroup.h>
STATIC PJB_PLATFORM_PROTOCOL mPlatform;
STATIC PJB_CHARGE_CONTROL_PROTOCOL mCharge;
STATIC EFI_GUID mChargeGuid;
STATIC W202_LOADER_HANDOFF mHandoff;
STATIC EFI_GUID mPlatformGuid;
STATIC EFI_EVENT mTimer, mExit, mTouchEvent, mTouchIrqEvent, mPowerTimer;
STATIC BOOLEAN mQuiesced=TRUE, mExited, mTouchAvailable, mHaveState;
STATIC BOOLEAN mTouchScheduled;
STATIC EFI_ABSOLUTE_POINTER_STATE mQueue[32], mLast;
STATIC UINTN mHead, mCount;
STATIC EFI_STATUS mTouchError;
STATIC EFI_STATUS mTouchInit, mPowerStatus=EFI_NOT_READY, mChargeStatus=EFI_NOT_READY;
STATIC PJB_POWER_SAMPLE mPowerSample;
STATIC UINTN mPollLog, mErrorLog;
STATIC UINTN mTouchSignals, mTouchCallbacks, mTouchEmpty, mTouchValid;
STATIC BOOLEAN mWatchdogFailed;
STATIC EFI_ABSOLUTE_POINTER_MODE mMode={0,0,0,1199,1919,0,0};
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL mPointer;

#ifdef W202_TOUCH_IRQ_DIAG
/* Isolated diagnostic read, not a second input provider. A key press must not
 * turn a report it consumes into a synthetic pointer event/click. */
VOID W202TouchProbeKey(VOID) {
  EFI_ABSOLUTE_POINTER_STATE State;
  EFI_STATUS Status=EFI_NOT_READY;
  ZeroMem(&State,sizeof(State));
  if (!mQuiesced && !mExited && mTouchAvailable)
    Status=W202TouchRead(&State);
  DEBUG((DEBUG_WARN,"HX_KEY_PROBE status=%r native=%lu,%lu down=%u\n",
    Status,State.CurrentX,State.CurrentY,State.ActiveButtons));
}
#endif

STATIC VOID EFIAPI PowerPoll(EFI_EVENT Event, VOID *Context) {
  PJB_POWER_SAMPLE Sample;
  UINT32 NtcMv=0;
  EFI_STATUS Status;
  EFI_TPL Old;
  if (mQuiesced || mExited) return;
  ZeroMem(&Sample,sizeof(Sample)); Sample.size=sizeof(Sample);
  Status=W202ReadPower(&mHandoff,&Sample);
  if (Sample.valid&PJB_POWER_TEMPERATURE) W202NtcVoltage(&NtcMv);
  mChargeStatus=W202ChargeUpdate(&Sample,NtcMv);
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  CopyMem(&mPowerSample,&Sample,sizeof(Sample)); mPowerStatus=Status;
  gBS->RestoreTPL(Old);
}

STATIC VOID ClearInput(VOID) {
  mHead=0; mCount=0; mHaveState=FALSE; mTouchError=EFI_SUCCESS;
}
STATIC VOID EFIAPI Poll(EFI_EVENT Event, VOID *Context) {
  STATIC UINTN TraceTicks;
  if (!mQuiesced && !mExited) W202KeysPoll();
  if (!mQuiesced && !mExited && ++TraceTicks%1000==0) {
    if ((mHandoff.flags&W202_HANDOFF_RESIDENT_WATCHDOG) && !mWatchdogFailed) {
      EFI_STATUS WatchdogStatus=W202WatchdogRenew();
      mWatchdogFailed=EFI_ERROR(WatchdogStatus);
      DEBUG((DEBUG_WARN,"W202_RESIDENT watchdog=%r\n",WatchdogStatus));
    }
    volatile W202_BOOT_METRICS *Metrics=(VOID *)(UINTN)W202_BOOT_METRICS_ADDRESS;
    if (Metrics->magic==W202_BOOT_METRICS_MAGIC && Metrics->frequency>=1000000 &&
        Metrics->frequency<=1000000000 && Metrics->app_counter>=Metrics->start_counter &&
        Metrics->app_counter-Metrics->start_counter<Metrics->frequency*30)
      DEBUG((DEBUG_WARN,"W202_BOOT loader_to_app_ms=%Lu\n",(Metrics->app_counter-Metrics->start_counter)*1000/Metrics->frequency));
    if (mTouchAvailable) W202TouchIrqTrace();
    else W202TouchStartupTrace();
#ifdef W202_TOUCH_RELOAD
    if (mTouchAvailable) W202TouchStartupTrace();
#endif
    DEBUG((DEBUG_WARN,"W202_TOUCH init=%r available=%u signals=%u callbacks=%u empty=%u valid=%u\n",mTouchInit,mTouchAvailable,mTouchSignals,mTouchCallbacks,mTouchEmpty,mTouchValid));
    DEBUG((DEBUG_WARN,"W202_POWER_CACHE status=%r charge_policy=%r valid=%x charging=%u\n",mPowerStatus,mChargeStatus,mPowerSample.valid,mPowerSample.charging));
    W202ChargeTrace();
  }
}
STATIC VOID EFIAPI ScheduleTouch(EFI_EVENT Event, VOID *Context) {
  EFI_STATUS Status;
  if (mQuiesced || mExited || !mTouchAvailable) return;
  ++mTouchSignals;
  if (mTouchScheduled) return;
  mTouchScheduled=TRUE;
  /* Queue the SPI work at callback TPL as soon as the IRQ returns. A relative
   * timer adds the platform timer's granularity to the report latency. */
  Status=gBS->SignalEvent(mTouchEvent);
  if (EFI_ERROR(Status)) {
    mTouchScheduled=FALSE; ClearInput(); mTouchError=EFI_DEVICE_ERROR;
    gBS->SignalEvent(mPointer.WaitForInput);
  }
}
STATIC VOID EFIAPI TouchEvent(EFI_EVENT Event, VOID *Context) {
  EFI_ABSOLUTE_POINTER_STATE State;
  EFI_STATUS Status;
  mTouchScheduled=FALSE;
  if (mQuiesced || mExited) return;
  if (!mTouchAvailable) return;
  ++mTouchCallbacks;
  if (W202TouchIrqFaulted()) {
    ClearInput(); mTouchError=EFI_DEVICE_ERROR;
    gBS->SignalEvent(mPointer.WaitForInput); return;
  }
  Status=W202TouchRead(&State);
  if (Status==EFI_NOT_READY) { ++mTouchEmpty; return; }
  if (EFI_ERROR(Status)) {
    if (mErrorLog++<2) DEBUG((DEBUG_WARN,"W202_TOUCH read=%r\n",Status));
    /* Discard a partial gesture. The consumer must not synthesize a click. */
    ClearInput(); mTouchError=EFI_DEVICE_ERROR;
    gBS->SignalEvent(mPointer.WaitForInput); return;
  }
  ++mTouchValid;
  if (mPollLog++==0 || (State.ActiveButtons && mPollLog<8))
    DEBUG((DEBUG_WARN,"W202_TOUCH event x=%lu y=%lu buttons=%u\n",State.CurrentX,State.CurrentY,State.ActiveButtons));
  if (!State.ActiveButtons && mHaveState) {
    State.CurrentX=mLast.CurrentX; State.CurrentY=mLast.CurrentY;
  }
  if (mHaveState && CompareMem(&State,&mLast,sizeof(State))==0) return;
  if (mCount==ARRAY_SIZE(mQueue)) {
    ClearInput(); mTouchError=EFI_DEVICE_ERROR;
  } else {
    mQueue[(mHead+mCount)%ARRAY_SIZE(mQueue)]=State; ++mCount;
    mLast=State; mHaveState=TRUE;
  }
  gBS->SignalEvent(mPointer.WaitForInput);
}
STATIC VOID EFIAPI WaitInput(EFI_EVENT Event, VOID *Context) {
  if (!mQuiesced && (mCount || EFI_ERROR(mTouchError))) gBS->SignalEvent(Event);
}
STATIC EFI_STATUS EFIAPI Reset(EFI_ABSOLUTE_POINTER_PROTOCOL *This, BOOLEAN Extended) {
  EFI_TPL Old;
  if (This!=&mPointer) return EFI_INVALID_PARAMETER;
  if (mExited) return EFI_UNSUPPORTED;
  Old=gBS->RaiseTPL(TPL_NOTIFY); ClearInput(); gBS->RestoreTPL(Old);
  return EFI_SUCCESS; // Software state only. Never reset the shared panel IC.
}
STATIC EFI_STATUS EFIAPI GetState(EFI_ABSOLUTE_POINTER_PROTOCOL *This,
                                 EFI_ABSOLUTE_POINTER_STATE *State) {
  STATIC BOOLEAN Logged;
  EFI_TPL Old;
  EFI_STATUS Status=EFI_NOT_READY;
  if (This!=&mPointer || State==NULL) return EFI_INVALID_PARAMETER;
  if (mExited) return EFI_UNSUPPORTED;
  if (!Logged) { DEBUG((DEBUG_WARN,"W202_TOUCH consumer connected\n")); Logged=TRUE; }
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  if (!mQuiesced && EFI_ERROR(mTouchError)) {
    Status=mTouchError; mTouchError=EFI_SUCCESS;
  } else if (!mQuiesced && mCount) {
    *State=mQueue[mHead]; mHead=(mHead+1)%ARRAY_SIZE(mQueue); --mCount;
    Status=EFI_SUCCESS;
  }
  gBS->RestoreTPL(Old); return Status;
}
STATIC PJB_STATUS EFIAPI ReadPower(VOID *This, PJB_POWER_SAMPLE *Sample) {
  EFI_STATUS Status;
  EFI_TPL Old;
  if (This!=&mPlatform || Sample==NULL || Sample->size<sizeof(*Sample))
    return EFI_INVALID_PARAMETER;
  ZeroMem(Sample,sizeof(*Sample)); Sample->size=sizeof(*Sample);
  if (mExited || mQuiesced) return EFI_NOT_READY;
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  CopyMem(Sample,&mPowerSample,sizeof(*Sample)); Sample->size=sizeof(*Sample);
  Status=mPowerStatus;
  gBS->RestoreTPL(Old);
  return Status;
}
STATIC PJB_STATUS EFIAPI ReadChargeEnabled(VOID *This,UINT32 *Enabled) {
  EFI_TPL Old; EFI_STATUS Status;
  if (This!=&mCharge || Enabled==NULL) return EFI_INVALID_PARAMETER;
  if (mQuiesced || mExited) return EFI_NOT_READY;
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  Status=W202ChargeReadEnabled(Enabled);
  gBS->RestoreTPL(Old); return Status;
}
STATIC PJB_STATUS EFIAPI ReadChargeRequested(VOID *This,UINT32 *Requested) {
  EFI_TPL Old; EFI_STATUS Status;
  if (This!=&mCharge || Requested==NULL) return EFI_INVALID_PARAMETER;
  if (mQuiesced || mExited) return EFI_NOT_READY;
  Old=gBS->RaiseTPL(TPL_NOTIFY); Status=W202ChargeReadRequested(Requested);
  gBS->RestoreTPL(Old); return Status;
}
STATIC PJB_STATUS EFIAPI SetChargeEnabled(VOID *This,UINT32 Enabled) {
  EFI_TPL Old; EFI_STATUS Status; PJB_POWER_SAMPLE Sample; UINT32 NtcMv=0,Actual=2;
  if (This!=&mCharge || Enabled>1) return EFI_INVALID_PARAMETER;
  if (mQuiesced || mExited) return EFI_NOT_READY;
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  ZeroMem(&Sample,sizeof(Sample)); Sample.size=sizeof(Sample);
  if (Enabled) {
    W202ReadPower(&mHandoff,&Sample);
    if (Sample.valid&PJB_POWER_TEMPERATURE) W202NtcVoltage(&NtcMv);
  }
  Status=W202ChargeRequest(&Sample,NtcMv,Enabled);
  if (!EFI_ERROR(Status)) Status=W202ChargeReadEnabled(&Actual);
  if (!EFI_ERROR(Status) && Actual!=Enabled) Status=EFI_DEVICE_ERROR;
  mChargeStatus=Status;
  gBS->RestoreTPL(Old);
  DEBUG((DEBUG_WARN,"W202_CHARGE_SET request=%u actual=%u status=%r\n",Enabled,Actual,Status));
  return Status;
}
STATIC PJB_STATUS EFIAPI SetQuiesced(VOID *This, UINT32 Quiesced) {
  EFI_TPL Old;
  EFI_STATUS Status=EFI_SUCCESS;
  if (This!=&mPlatform || Quiesced>1) return EFI_INVALID_PARAMETER;
  if (mExited) return EFI_UNSUPPORTED;
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  if ((BOOLEAN)Quiesced==mQuiesced) goto Done;
  if (Quiesced) {
    Status=W202ChargeStop();
    DEBUG((DEBUG_WARN,"W202_QUIESCE charge_stop=%r cached_charging=%u\n",Status,mPowerSample.charging));
    if (EFI_ERROR(Status)) goto Done;
    Status=gBS->SetTimer(mTimer,TimerCancel,0);
    if (EFI_ERROR(Status)) goto Done;
    Status=gBS->SetTimer(mPowerTimer,TimerCancel,0);
    if (EFI_ERROR(Status)) { gBS->SetTimer(mTimer,TimerPeriodic,100000); goto Done; }
    mQuiesced=TRUE; W202KeysEnable(FALSE); W202TouchIrqStop();
    mTouchScheduled=FALSE;
    W202TouchStop(); W202PowerReset(); mPowerStatus=EFI_NOT_READY;
    ZeroMem(&mPowerSample,sizeof(mPowerSample)); ClearInput();
  } else {
    if (mTouchAvailable) {
      Status=W202TouchStart(); if (EFI_ERROR(Status)) goto Done;
      Status=W202TouchIrqStart(); if (EFI_ERROR(Status)) { W202TouchStop(); goto Done; }
    }
    Status=gBS->SetTimer(mTimer,TimerPeriodic,100000);
    if (EFI_ERROR(Status)) { W202TouchIrqStop(); W202TouchStop(); goto Done; }
    Status=gBS->SetTimer(mPowerTimer,TimerPeriodic,10000000);
    if (EFI_ERROR(Status)) {
      gBS->SetTimer(mTimer,TimerCancel,0); W202TouchIrqStop(); W202TouchStop(); goto Done;
    }
    W202KeysEnable(TRUE);
    ClearInput(); mQuiesced=FALSE;
  }
Done:
  gBS->RestoreTPL(Old); return Status;
}
STATIC VOID EFIAPI ExitBootServices(EFI_EVENT Event, VOID *Context) {
  // No allocation, logging, protocol lookup or Boot Services calls here.
  mExited=TRUE; mQuiesced=TRUE; W202ChargeStop(); W202KeysExit(); W202TouchIrqExit(); W202TouchStop(); W202PowerReset(); ClearInput();
  mPowerStatus=EFI_NOT_READY; ZeroMem(&mPowerSample,sizeof(mPowerSample));
}
EFI_STATUS EFIAPI Initialize(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System) {
  EFI_STATUS Status;
  PJB_GUID Guid=PJB_PLATFORM_GUID,ChargeGuid=PJB_CHARGE_CONTROL_GUID;
  CopyMem(&mHandoff,(VOID *)(UINTN)W202_HANDOFF_ADDRESS,sizeof(mHandoff));
  if (!W202HandoffValid(&mHandoff)) return EFI_UNSUPPORTED;
  DEBUG((DEBUG_WARN,"W202_HANDOFF flags=%x gain=%d offset=%d\n",mHandoff.flags,mHandoff.voltage_gain,mHandoff.voltage_offset));
  CopyMem(&mPlatformGuid,&Guid,sizeof(Guid));
  mPlatform.revision=PJB_PLATFORM_REVISION; mPlatform.size=sizeof(mPlatform);
  mPlatform.read_power=ReadPower; mPlatform.set_quiesced=SetQuiesced;
  CopyMem(&mChargeGuid,&ChargeGuid,sizeof(ChargeGuid));
  mCharge.revision=PJB_CHARGE_CONTROL_REVISION; mCharge.size=sizeof(mCharge);
  mCharge.read_enabled=ReadChargeEnabled; mCharge.set_enabled=SetChargeEnabled;
  mCharge.read_requested=ReadChargeRequested;
  mChargeStatus=W202ChargeStop();
  mPointer.Reset=Reset; mPointer.GetState=GetState; mPointer.Mode=&mMode;
  Status=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_CALLBACK,WaitInput,NULL,&mPointer.WaitForInput);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Poll,NULL,&mTimer);
  if (EFI_ERROR(Status)) goto FailWait;
  Status=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,PowerPoll,NULL,&mPowerTimer);
  if (EFI_ERROR(Status)) goto FailTimer;
  Status=gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,TouchEvent,NULL,&mTouchEvent);
  if (EFI_ERROR(Status)) goto FailPowerTimer;
  Status=gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,ScheduleTouch,NULL,&mTouchIrqEvent);
  if (EFI_ERROR(Status)) goto FailTouchEvent;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBootServices,NULL,
                          &gEfiEventExitBootServicesGuid,&mExit);
  if (EFI_ERROR(Status)) goto FailTouchIrqEvent;
  Status=W202TouchStart();
  mTouchInit=Status;
  DEBUG((DEBUG_WARN,"W202_TOUCH init=%r\n",Status));
  mTouchAvailable=!EFI_ERROR(Status);
  if (mTouchAvailable) {
    Status=W202TouchIrqInitialize(mTouchIrqEvent);
    if (!EFI_ERROR(Status)) Status=W202TouchIrqStart();
    DEBUG((DEBUG_WARN,"W202_TOUCH IRQ init=%r\n",Status));
    mTouchInit=Status;
    if (EFI_ERROR(Status)) { W202TouchIrqExit(); W202TouchStop(); mTouchAvailable=FALSE; }
  }
  Status=gBS->SetTimer(mTimer,TimerPeriodic,100000);
  if (EFI_ERROR(Status)) goto FailExit;
  Status=gBS->SetTimer(mPowerTimer,TimerPeriodic,10000000);
  if (EFI_ERROR(Status)) goto FailExit;
  mQuiesced=FALSE;
  if (mTouchAvailable)
    Status=gBS->InstallMultipleProtocolInterfaces(&Image,&mPlatformGuid,&mPlatform,&mChargeGuid,&mCharge,
                  &gEfiAbsolutePointerProtocolGuid,&mPointer,NULL);
  else
    Status=gBS->InstallMultipleProtocolInterfaces(&Image,&mPlatformGuid,&mPlatform,&mChargeGuid,&mCharge,NULL);
  if (!EFI_ERROR(Status)) {
    EFI_STATUS KeyStatus=W202KeysInit();
    DEBUG((DEBUG_WARN,"W202_KEYS init=%r\n",KeyStatus));
    return Status;
  }
FailExit:
  mQuiesced=TRUE; W202TouchIrqExit(); W202TouchStop(); gBS->CloseEvent(mExit);
FailTouchIrqEvent:
  gBS->CloseEvent(mTouchIrqEvent);
FailTouchEvent:
  gBS->CloseEvent(mTouchEvent);
FailPowerTimer:
  gBS->CloseEvent(mPowerTimer);
FailTimer:
  gBS->CloseEvent(mTimer);
FailWait:
  gBS->CloseEvent(mPointer.WaitForInput);
  return Status;
}
