// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"
#include <Library/DebugLib.h>
#include <Protocol/HardwareInterrupt.h>

/* DT: async EIC pin 16, bank 2 bit 0, routed through SPI 24 (GIC ID 56).
 * The firmware's IRQ word declares falling-edge reports. A high pin sampled
 * later during a read does not establish a rising-edge interrupt source.
 * The SPI work stays outside the ISR. */
#define EIC_BASE 0x642200a0
#define EIC_BIT BIT0
#define EIC_IE 0x00
#define EIC_MASKED 0x08
#define EIC_CLEAR 0x0c
#define EIC_MODE 0x10
#define EIC_BOTH 0x14
#define EIC_POLARITY 0x18
#define EIC_GIC 56

STATIC EFI_HARDWARE_INTERRUPT_PROTOCOL *mInterrupt;
STATIC EFI_EVENT mEvent;
STATIC BOOLEAN mRegistered, mActive, mFault;
STATIC UINTN mInterrupts, mOtherInterrupts;
STATIC UINTN mHighInterrupts, mLowInterrupts;
STATIC UINTN mEmptyInterrupts, mEmptyStreak;
STATIC UINT32 mSavedIe, mSavedMode, mSavedBoth, mSavedPolarity;

STATIC VOID SetBit(UINTN Reg, UINT32 Saved) {
  MmioAndThenOr32(EIC_BASE+Reg,~(UINT32)EIC_BIT,Saved&EIC_BIT);
}

/* All six EIC banks share one GIC line. Do not steal another enabled source. */
STATIC BOOLEAN OtherSourcesEnabled(VOID) {
  UINTN Bank;
  for (Bank=0;Bank<6;++Bank) {
    UINTN Base=0x64200000+Bank*0x10000;
    UINT32 Debounce=MmioRead32(Base+0x18)&0xff;
    UINT32 Latch=MmioRead32(Base+0x80)&0xff;
    UINT32 Sync=MmioRead32(Base+0xc0)&0xff;
    UINT32 Async=MmioRead32(Base+0xa0)&(Bank==2 ? 0xfe : 0xff);
    if (Debounce || Latch || Sync || Async) {
      DEBUG((DEBUG_WARN,"W202_TOUCH IRQ shared bank=%u db=%x latch=%x sync=%x async=%x\n",Bank,Debounce,Latch,Sync,Async));
      return TRUE;
    }
  }
  return FALSE;
}

STATIC BOOLEAN OtherSourcesPending(VOID) {
  UINTN Bank;
  for (Bank=0;Bank<6;++Bank) {
    UINTN Base=0x64200000+Bank*0x10000;
    if ((MmioRead32(Base+0x20)&0xff) ||
        (MmioRead32(Base+0x88)&0xff) ||
        (MmioRead32(Base+0xc8)&0xff) ||
        (MmioRead32(Base+0xa8)&(Bank==2 ? 0xfe : 0xff)))
      return TRUE;
  }
  return FALSE;
}

STATIC VOID EFIAPI TouchInterrupt(HARDWARE_INTERRUPT_SOURCE Source, EFI_SYSTEM_CONTEXT Context) {
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  BOOLEAN Touch=(MmioRead32(EIC_BASE+EIC_MASKED)&EIC_BIT)!=0;
  BOOLEAN Other=OtherSourcesPending();
  ++mInterrupts;
  if (Touch) {
    if (MmioRead32(EIC_BASE+0x1c)&EIC_BIT) ++mHighInterrupts;
    else ++mLowInterrupts;
    MmioWrite32(EIC_BASE+EIC_CLEAR,EIC_BIT);
    /* Flush the peripheral clear before GIC EOI. A stale parent interrupt
     * can still arrive during EIC-to-GIC synchronization. */
    MmioRead32(EIC_BASE+EIC_MASKED);
    mEmptyStreak=0;
  }
  if (Other) {
    /* A masked pending foreign source, not an empty EIC snapshot, establishes
     * that another owner appeared on the shared parent. Never clear it. */
    mFault=TRUE;
    ++mOtherInterrupts;
  } else if (!Touch) {
    ++mEmptyInterrupts;
    /* Tolerate isolated stale parent deliveries, but bound an unexplained
     * empty-source storm. No SPI read or synthetic input for an empty IRQ. */
    if (++mEmptyStreak>=8) mFault=TRUE;
  }
  if (mFault)
    mInterrupt->DisableInterruptSource(mInterrupt,Source);
  mInterrupt->EndOfInterrupt(mInterrupt,Source);
  if (mActive && (Touch || mFault)) gBS->SignalEvent(mEvent);
  gBS->RestoreTPL(Old);
}

EFI_STATUS W202TouchIrqInitialize(EFI_EVENT Event) {
  EFI_STATUS Status;
  if (!Event || mRegistered) return EFI_INVALID_PARAMETER;
  if (OtherSourcesEnabled()) return EFI_ACCESS_DENIED;
  Status=gBS->LocateProtocol(&gHardwareInterruptProtocolGuid,NULL,(VOID **)&mInterrupt);
  if (EFI_ERROR(Status)) return Status;
  mEvent=Event;
  mSavedIe=MmioRead32(EIC_BASE+EIC_IE);
  mSavedMode=MmioRead32(EIC_BASE+EIC_MODE);
  mSavedBoth=MmioRead32(EIC_BASE+EIC_BOTH);
  mSavedPolarity=MmioRead32(EIC_BASE+EIC_POLARITY);
  SetBit(EIC_IE,0);
  Status=mInterrupt->RegisterInterruptSource(mInterrupt,EIC_GIC,TouchInterrupt);
  if (EFI_ERROR(Status)) { SetBit(EIC_IE,mSavedIe);return Status; }
  mRegistered=TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS W202TouchIrqStart(VOID) {
  EFI_STATUS Status;
  if (!mRegistered) return EFI_NOT_READY;
  if (mActive) return EFI_SUCCESS;
  mFault=FALSE;
  mEmptyStreak=0;
  SetBit(EIC_IE,0);
  SetBit(EIC_BOTH,0); SetBit(EIC_MODE,0); SetBit(EIC_POLARITY,0);
  MmioWrite32(EIC_BASE+EIC_CLEAR,EIC_BIT);
  Status=mInterrupt->EnableInterruptSource(mInterrupt,EIC_GIC);
  if (EFI_ERROR(Status)) {
    SetBit(EIC_MODE,mSavedMode); SetBit(EIC_BOTH,mSavedBoth);
    SetBit(EIC_POLARITY,mSavedPolarity); SetBit(EIC_IE,mSavedIe);
    return Status;
  }
  mActive=TRUE;
  SetBit(EIC_IE,EIC_BIT);
  return EFI_SUCCESS;
}

VOID W202TouchIrqStop(VOID) {
  if (!mRegistered || !mActive) return;
  SetBit(EIC_IE,0); mActive=FALSE;
  mInterrupt->DisableInterruptSource(mInterrupt,EIC_GIC);
  MmioWrite32(EIC_BASE+EIC_CLEAR,EIC_BIT);
  SetBit(EIC_MODE,mSavedMode); SetBit(EIC_BOTH,mSavedBoth);
  SetBit(EIC_POLARITY,mSavedPolarity); SetBit(EIC_IE,mSavedIe);
}

VOID W202TouchIrqExit(VOID) {
  if (!mRegistered) return;
  W202TouchIrqStop();
  mInterrupt->RegisterInterruptSource(mInterrupt,EIC_GIC,NULL);
  mRegistered=FALSE;
}

BOOLEAN W202TouchIrqFaulted(VOID) { return mFault; }

VOID W202TouchIrqTrace(VOID) {
  DEBUG((DEBUG_WARN,"W202_IRQ count=%u high=%u low=%u other=%u empty=%u active=%u fault=%u ie=%08x raw=%08x masked=%08x mode=%08x both=%08x polarity=%08x data=%08x gic_enable=%08x gic_pending=%08x gic_config=%08x\n",
    mInterrupts,mHighInterrupts,mLowInterrupts,mOtherInterrupts,mEmptyInterrupts,mActive,mFault,
    MmioRead32(EIC_BASE+EIC_IE),MmioRead32(EIC_BASE+4),MmioRead32(EIC_BASE+EIC_MASKED),
    MmioRead32(EIC_BASE+EIC_MODE),MmioRead32(EIC_BASE+EIC_BOTH),
    MmioRead32(EIC_BASE+EIC_POLARITY),MmioRead32(EIC_BASE+0x1c),
    MmioRead32(0x12000104),MmioRead32(0x12000204),MmioRead32(0x12000c0c)));
}
