// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"
#include <Library/DebugLib.h>

/* UMP9620 channel 0, scale 1, 15 samples with three discarded at each end.
 * Matches stock LK 0xb506c4d8/0xb506cd20. Calibration is copied before LK
 * cleanup; EDK2 owns each bounded conversion and restores inherited state.
 */
#define ADC_CTL 0x64420504
#define ADC_CHANNEL 0x64420508
#define ADC_DATA 0x64420550
#define ADC_INT_EN 0x64420554
#define ADC_INT_CLR 0x64420558
#define ADC_INT_RAW 0x64420560
STATIC BOOLEAN mAdcFaulted;
STATIC BOOLEAN mNtcValid;
STATIC UINT32 mNtcMv;

EFI_STATUS W202NtcVoltage(UINT32 *Mv) {
  if (Mv==NULL) return EFI_INVALID_PARAMETER;
  if (!mNtcValid) return EFI_NOT_READY;
  *Mv=mNtcMv; return EFI_SUCCESS;
}

STATIC EFI_STATUS AdcWrite(UINT32 Address, UINT32 Value) {
  UINTN Budget;
  if ((Address!=ADC_CTL && Address!=ADC_CHANNEL && Address!=ADC_INT_CLR) || Value>0xffff)
    return EFI_INVALID_PARAMETER;
  for (Budget=0;Budget<2000;++Budget) {
    UINT32 Fifo=MmioRead32(0x64400030);
    if ((Fifo&BIT10) && !(Fifo&(BIT11|BIT31))) break;
    MicroSecondDelay(1);
  }
  if (Budget==2000) return EFI_TIMEOUT;
  MmioWrite32(Address,Value);
  for (Budget=0;Budget<2000;++Budget) {
    UINT32 Fifo=MmioRead32(0x64400030);
    if ((Fifo&BIT10) && !(Fifo&BIT31)) return EFI_SUCCESS;
    MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS ReadNtc(UINT32 *Raw) {
  UINT32 Control, Channel, Irq, Data, Values[15], Sum=0, Verify;
  UINTN I,J;
  EFI_STATUS Status, Restore=EFI_SUCCESS, S;
  if (mAdcFaulted) return EFI_DEVICE_ERROR;
  Status=W202AdiRead(ADC_CTL,&Control); if (EFI_ERROR(Status)) return Status;
  Status=W202AdiRead(ADC_CHANNEL,&Channel); if (EFI_ERROR(Status)) return Status;
  Status=W202AdiRead(ADC_INT_EN,&Irq); if (EFI_ERROR(Status)) return Status;
  /* Only borrow an initialized, idle software channel without an IRQ owner. */
  if ((Control&(BIT0|BIT1|BIT2))!=(BIT0|BIT2) || Irq) return EFI_NOT_READY;
  Status=AdcWrite(ADC_INT_CLR,1); if (EFI_ERROR(Status)) goto Done;
  Status=AdcWrite(ADC_CHANNEL,(Channel&~0x61fu)|BIT9); if (EFI_ERROR(Status)) goto Done;
  MicroSecondDelay(500);
  Status=AdcWrite(ADC_CTL,(Control&~0xf0u)|0xe0u|BIT1); if (EFI_ERROR(Status)) goto Done;
  MicroSecondDelay(1000);
  for (I=0;I<500;++I) {
    Status=W202AdiRead(ADC_INT_RAW,&Data); if (EFI_ERROR(Status)) goto Done;
    if (Data&BIT0) break;
    MicroSecondDelay(10);
  }
  if (I==500) { Status=EFI_TIMEOUT; goto Done; }
  MicroSecondDelay(1000);
  for (I=0;I<ARRAY_SIZE(Values);++I) {
    MicroSecondDelay(5);
    Status=W202AdiRead(ADC_DATA,&Data); if (EFI_ERROR(Status)) goto Done;
    Values[I]=Data&0xfff;
  }
  for (I=1;I<ARRAY_SIZE(Values);++I) {
    Data=Values[I]; J=I;
    while (J && Values[J-1]>Data) { Values[J]=Values[J-1]; --J; }
    Values[J]=Data;
  }
  for (I=3;I<12;++I) Sum+=Values[I];
  *Raw=(Sum+4)/9;
  if (*Raw==0 || *Raw==0xfff) Status=EFI_DEVICE_ERROR;
Done:
  /* Run is always cleared before restoring channel/interrupt state. Attempt
   * all cleanup writes even after a timeout; a failed restore latches a fault. */
  S=AdcWrite(ADC_CTL,Control); if (EFI_ERROR(S)) Restore=S;
  S=AdcWrite(ADC_CHANNEL,Channel); if (EFI_ERROR(S)) Restore=S;
  S=AdcWrite(ADC_INT_CLR,1); if (EFI_ERROR(S)) Restore=S;
  S=W202AdiRead(ADC_CTL,&Verify);
  if (EFI_ERROR(S) || Verify!=Control) Restore=EFI_DEVICE_ERROR;
  S=W202AdiRead(ADC_CHANNEL,&Verify);
  if (EFI_ERROR(S) || Verify!=Channel) Restore=EFI_DEVICE_ERROR;
  if (EFI_ERROR(Restore)) { mAdcFaulted=TRUE; return Restore; }
  return Status;
}

STATIC INT64 Rounded(INT64 N, INT64 D) {
  return N>=0 ? (N+D/2)/D : -((-N+D/2)/D);
}

EFI_STATUS W202ReadTemperature(CONST W202_LOADER_HANDOFF *Handoff,
                               CONST PJB_POWER_SAMPLE *Sample, INT32 *Temperature) {
  /* Both stock battery variants use this NTC table (uV, deci-C + 1000). */
  STATIC CONST UINT32 Microvolts[]={1591473,1555286,1512176,1459276,1395886,
    1322179,1238711,1147116,1049658,949081,848416,750447,657569,571537,
    493518,423845,362641,309430,263582,221438,188468};
  STATIC UINTN Logs;
  UINT32 Raw;
  INT64 Uv,Mv,Current,Resistance,Calib,Denominator;
  UINTN I;
  EFI_STATUS Status;
  mNtcValid=FALSE;
  if (!(Handoff->flags&W202_HANDOFF_BATDET_CALIBRATION) || Handoff->version!=2)
    return EFI_UNSUPPORTED;
  if (Handoff->ntc_high_mv!=1400 || Handoff->ntc_low_mv!=200 ||
      Handoff->ntc_high_raw<3000 || Handoff->ntc_high_raw>4000 ||
      Handoff->ntc_low_raw<200 || Handoff->ntc_low_raw>800)
    return EFI_COMPROMISED_DATA;
  /* Stock DT compensation depends on measured current, not an assumed zero. */
  if (!(Sample->valid&PJB_POWER_CURRENT)) return EFI_NOT_READY;
  Status=ReadNtc(&Raw); if (EFI_ERROR(Status)) return Status;
  Mv=(INT64)(Handoff->ntc_high_mv-Handoff->ntc_low_mv)*
     ((INT64)Raw-Handoff->ntc_low_raw)/(Handoff->ntc_high_raw-Handoff->ntc_low_raw)+
     Handoff->ntc_low_mv;
  Uv=Mv*1000;
  Current=Sample->current_ma;
  /* Stock FGU NTC compensation: comp-resistance=11 mOhm, shunt=4850 uOhm. */
  Resistance=Rounded(Current*11,10);
  Calib=Rounded(Current*485,1000)+Resistance;
  Denominator=187500-Calib;
  if (Denominator<=0) return EFI_DEVICE_ERROR;
  Uv=Rounded((Uv/10-Resistance)*Calib,Denominator)*10+Uv-Resistance*10;
  /* Open/short or readings beyond the characterized table remain unknown. */
  if (Uv>Microvolts[0] || Uv<Microvolts[ARRAY_SIZE(Microvolts)-1]) return EFI_DEVICE_ERROR;
  for (I=1;I<ARRAY_SIZE(Microvolts);++I) if (Uv>=Microvolts[I]) break;
  *Temperature=-250+(INT32)(I-1)*50+
    (INT32)Rounded(((INT64)Microvolts[I-1]-Uv)*50,Microvolts[I-1]-Microvolts[I]);
  mNtcMv=(UINT32)Mv; mNtcValid=TRUE;
  if (Logs++<2) DEBUG((DEBUG_WARN,"W202_NTC raw=%u mv=%d compensated_uv=%ld temp_decic=%d\n",Raw,(INT32)Mv,Uv,*Temperature));
  return EFI_SUCCESS;
}
