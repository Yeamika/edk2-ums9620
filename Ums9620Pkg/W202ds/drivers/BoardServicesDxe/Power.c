// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"
#include <Library/DebugLib.h>

/* UMP9620 through inherited ADI. Only the controller read-command register is
 * written; no PMIC configuration, RTC SOC, charger enable or HIZ is changed.
 * Source: sprd-linux/lk qogirn6pro hardware.h / adi.c / ump9620 registers.
 */
EFI_STATUS W202AdiRead(UINT32 Address, UINT32 *Value) {
  UINT32 Data;
  UINTN Count;
  if (Value==NULL || Address < 0x64420000 || Address >= 0x64440000 || (Address & 3))
    return EFI_INVALID_PARAMETER;
  if (MmioRead32(0x6440002c) & BIT31) return EFI_NOT_READY;
  MmioWrite32(0x64400028, Address);
  for (Count=0; Count<2000; ++Count) {
    Data=MmioRead32(0x6440002c);
    if (!(Data & BIT31)) { *Value=Data & 0xffff; return EFI_SUCCESS; }
    MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}
#define AdiRead W202AdiRead

/* The RTC user area is the last UI capacity saved by Android, in 0.1%.
 * Track only the subsequent counter change; the anchor may already be stale.
 * Resistance and design capacity come from the stock W202DS battery DT. */
#define W202_CALIB_RESIST_UOHM 4850
#define W202_IDEAL_RESIST_UOHM 20000
/* Stock FGU uses charge-full-microamp-hours, not the design nameplate.
 * A learned/aged capacity can differ: the percentage remains an estimate. */
#define W202_FULL_CAPACITY_MAH 7286
#define W202_FGU_SAMPLE_HZ 2
STATIC BOOLEAN mHaveCapacityAnchor;
STATIC UINT32 mSavedAnchor, mCounterAnchor;
STATIC INT32 mCapacityAnchor;

VOID W202PowerReset(VOID) { mHaveCapacityAnchor=FALSE; }

STATIC INT64 DivideRounded(INT64 Value, INT64 Divisor) {
  return Value>=0 ? (Value+Divisor/2)/Divisor : -((-Value+Divisor/2)/Divisor);
}

STATIC BOOLEAN DecodeSavedCapacity(UINT32 Saved, INT32 *Tenths) {
  UINT32 Mode=(Saved>>12)&0xf, Whole=Saved&0xff, Fraction=(Saved>>8)&0xf;
  if (Mode!=5 || (Saved&0xfff)==0xfff || (Saved&0xfff)==0xa05 ||
      Whole>100 || Fraction>9 || (Whole==100 && Fraction!=0)) return FALSE;
  *Tenths=(INT32)(Whole*10+Fraction);
  return TRUE;
}

STATIC EFI_STATUS ReadVoltage(UINT32 *Raw) {
  UINT32 Previous;
  UINTN Try;
  EFI_STATUS Status=AdiRead(0x64420820,&Previous);
  if (EFI_ERROR(Status)) return Status;
  for (Try=0;Try<8;++Try) {
    Status=AdiRead(0x64420820,Raw);
    if (EFI_ERROR(Status)) return Status;
    if (*Raw==Previous) return EFI_SUCCESS;
    Previous=*Raw;
  }
  return EFI_NOT_READY;
}

STATIC EFI_STATUS ReadCounter(UINT32 *Counter) {
  UINT32 High, Low, Verify;
  EFI_STATUS Status;
  UINTN Try;
  for (Try=0;Try<4;++Try) {
    Status=AdiRead(0x64420868,&High); if (EFI_ERROR(Status)) return Status;
    Status=AdiRead(0x6442086c,&Low); if (EFI_ERROR(Status)) return Status;
    Status=AdiRead(0x64420868,&Verify); if (EFI_ERROR(Status)) return Status;
    if (High==Verify) { *Counter=(High<<16)|Low; return EFI_SUCCESS; }
  }
  return EFI_NOT_READY;
}

EFI_STATUS W202ReadPower(CONST W202_LOADER_HANDOFF *Handoff, PJB_POWER_SAMPLE *Sample) {
  STATIC BOOLEAN Logged;
  STATIC UINTN Samples;
  UINT32 Raw=0, Saved=0, Charger, Counter, Current;
  BOOLEAN HaveVoltage, HaveSaved;
  INT32 Tenths, Gain;
  INT64 Mv, Change, Estimated;
  EFI_STATUS Status, FirstError=EFI_NOT_READY;
  if (Handoff==NULL || Sample==NULL) return EFI_INVALID_PARAMETER;
  Sample->valid=0;
  if (!(Handoff->flags & W202_HANDOFF_ADI)) return EFI_UNSUPPORTED;
  Status=ReadVoltage(&Raw); HaveVoltage=!EFI_ERROR(Status);
  if (!HaveVoltage) FirstError=Status;
  Status=AdiRead(0x644208a8, &Saved);
  HaveSaved=!EFI_ERROR(Status);
  if (HaveVoltage) Sample->voltage_raw=Raw;
  if (HaveSaved) Sample->saved_soc_raw=Saved;
  if (HaveVoltage && HaveSaved) Sample->valid|=PJB_POWER_RAW_FGU;
  if (!Logged) {
    UINT32 Normal=0, Pocv=0, Oci=0, FguStatus=0;
    EFI_STATUS A=AdiRead(0x644208c8,&Normal), B=AdiRead(0x64420828,&Pocv);
    EFI_STATUS C=AdiRead(0x64420874,&Oci), D=AdiRead(0x6442080c,&FguStatus);
    DEBUG((DEBUG_WARN,"W202_FGU normal=%04x pocv=%04x oci=%04x status=%04x reads=%r,%r,%r,%r\n",Normal,Pocv,Oci,FguStatus,A,B,C,D));
    Logged=TRUE;
  }
  if ((Handoff->flags & W202_HANDOFF_CALIBRATION) &&
      Handoff->voltage_gain>=500 && Handoff->voltage_gain<=1500 &&
      Handoff->voltage_offset>=-1000 && Handoff->voltage_offset<=1000) {
    Mv=((INT64)Raw+Handoff->voltage_offset)*1000/Handoff->voltage_gain;
    if (HaveVoltage && Mv>=2000 && Mv<=5500) {
      Sample->voltage_mv=(UINT32)Mv;
      Sample->valid|=PJB_POWER_VOLTAGE;
    }
    Gain=(Handoff->voltage_gain*4*W202_CALIB_RESIST_UOHM+W202_IDEAL_RESIST_UOHM/2)/W202_IDEAL_RESIST_UOHM;
    Status=AdiRead(0x6442082c,&Current);
    if (!EFI_ERROR(Status) && Current<=0x3fff && Gain>0) {
      INT64 Ma=DivideRounded(((INT64)Current-8192)*1000,Gain);
      if (Ma>=-12000 && Ma<=12000) { Sample->current_ma=(INT32)Ma; Sample->valid|=PJB_POWER_CURRENT; }
    }
    if (HaveSaved && !DecodeSavedCapacity(Saved,&Tenths)) mHaveCapacityAnchor=FALSE;
    if (Gain>0 && HaveSaved && DecodeSavedCapacity(Saved,&Tenths) && !EFI_ERROR(ReadCounter(&Counter))) {
      if (!mHaveCapacityAnchor || mSavedAnchor!=Saved) {
        mSavedAnchor=Saved; mCounterAnchor=Counter; mCapacityAnchor=Tenths; mHaveCapacityAnchor=TRUE;
      }
      Change=DivideRounded((INT64)(INT32)(Counter-mCounterAnchor)*10000,
                           36*W202_FGU_SAMPLE_HZ*(INT64)Gain*W202_FULL_CAPACITY_MAH);
      if (Change>=-1000 && Change<=1000) {
        Estimated=(INT64)mCapacityAnchor+Change;
        if (Estimated<0) Estimated=0;
        if (Estimated>1000) Estimated=1000;
        Sample->percent=(UINT32)((Estimated+5)/10);
        Sample->valid|=PJB_POWER_PERCENT|PJB_POWER_PERCENT_ESTIMATED;
      }
    }
  }
  Status=AdiRead(0x6442239c, &Charger);
  if (!EFI_ERROR(Status)) {
    Sample->external_power=(Charger & BIT2) != 0;
    Sample->valid|=PJB_POWER_EXTERNAL;
  }
  Status=W202ReadTemperature(Handoff,Sample,&Sample->temperature_decic);
  if (!EFI_ERROR(Status)) Sample->valid|=PJB_POWER_TEMPERATURE;
  else if (Samples<3) DEBUG((DEBUG_WARN,"W202_NTC read=%r\n",Status));
  Status=W202ReadCharging(&Sample->charging);
  if (!EFI_ERROR(Status)) Sample->valid|=PJB_POWER_CHARGING;
  Sample->sample_ms=GetTimeInNanoSecond(GetPerformanceCounter())/1000000;
  if (Samples++<3) DEBUG((DEBUG_WARN,"W202_POWER valid=%x mv=%u current_ma=%d temp_decic=%d charging=%u percent=%u external=%u\n",Sample->valid,Sample->voltage_mv,Sample->current_ma,Sample->temperature_decic,Sample->charging,Sample->percent,Sample->external_power));
  return Sample->valid ? EFI_SUCCESS : FirstError;
}
