// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"
#include <Library/DebugLib.h>

/* Qogirn6Pro I2C protocol from sprd-linux/lk 47f4d04, sprd_i2c.c.
 * Reads/stops cover both chips. Only the stock I2C6 main charger may be
 * enabled after validating its retained W202DS factory profile.
 * Input/rate/voltage/HIZ, the slave enable GPIO and OTG remain with LK.
 */
#define I2C_GATE 0x20000000
#define I2C_START BIT0
#define I2C_ENABLE BIT2
#define I2C_READ BIT3
#define I2C_IRQ BIT2
#define I2C_NACK BIT1
#define I2C_BUSY BIT0
STATIC BOOLEAN mBusFaulted[2];
STATIC BOOLEAN mChargeOwned, mChargeRequested;
/* Cache the last enable decision. Re-printing this costs no I2C traffic and
 * keeps the rejection visible after the small firmware trace ring wraps. */
STATIC struct {
  UINT8 Id, Input, Enable, Rate, Voltage, Timer, Dpm, State, Fault, FaultNow;
  EFI_STATUS Status;
  UINT32 Attempts;
} mDecision;

VOID W202ChargeTrace(VOID) {
  DEBUG((DEBUG_WARN,"W202_CHARGE requested=%u owned=%u attempts=%u result=%r id=%02x regs=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x bus_fault=%u,%u\n",
    mChargeRequested,mChargeOwned,mDecision.Attempts,mDecision.Status,mDecision.Id,
    mDecision.Input,mDecision.Enable,mDecision.Rate,mDecision.Voltage,mDecision.Timer,
    mDecision.Dpm,mDecision.State,mDecision.Fault,mDecision.FaultNow,mBusFaulted[0],mBusFaulted[1]));
}

STATIC EFI_STATUS WaitTransfer(UINTN Base, UINTN *Budget) {
  UINT32 State;
  while (*Budget) {
    State=MmioRead32(Base+0x14);
    if (State&I2C_NACK) return EFI_NO_RESPONSE;
    if (State&I2C_IRQ) return EFI_SUCCESS;
    --*Budget; MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS ReadByte(UINTN Base, UINT32 Control, UINT8 Register,
                           UINT8 *Value, UINTN *Budget) {
  EFI_STATUS Status;
  UINT32 State;
  /* A STOP after the register pointer write avoids leaving the shared bus
   * held if the second phase fails. BQ2560x retains its register pointer. */
  State=MmioRead32(Base+0x14);
  MmioWrite32(Base+0x14,State&~(I2C_IRQ|I2C_NACK));
  MmioWrite32(Base+0x2c,1);
  MmioWrite32(Base+4,0x6b<<1); MmioWrite32(Base+8,1);
  MmioWrite32(Base+0x10,Register);
  MmioWrite32(Base,Control|I2C_START);
  Status=WaitTransfer(Base,Budget); if (EFI_ERROR(Status)) return Status;
  MmioWrite32(Base,Control);
  State=MmioRead32(Base+0x14);
  MmioWrite32(Base+0x14,State&~(I2C_IRQ|I2C_NACK));
  MmioWrite32(Base+0x2c,1);
  MmioWrite32(Base+8,1); MmioWrite32(Base,Control|I2C_READ|I2C_START);
  Status=WaitTransfer(Base,Budget); if (EFI_ERROR(Status)) return Status;
  *Value=(UINT8)MmioRead32(Base+0x0c);
  MmioWrite32(Base,Control);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS WriteByte(UINTN Base, UINT32 Control, UINT8 Register,
                            UINT8 Value, UINTN *Budget) {
  UINT32 State=MmioRead32(Base+0x14);
  MmioWrite32(Base+0x14,State&~(I2C_IRQ|I2C_NACK));
  MmioWrite32(Base+0x2c,1);
  MmioWrite32(Base+4,0x6b<<1); MmioWrite32(Base+8,2);
  MmioWrite32(Base+0x10,Register); MmioWrite32(Base+0x10,Value);
  MmioWrite32(Base,Control|I2C_START);
  return WaitTransfer(Base,Budget);
}

/* Command -1 reads; 0 stops; 1 resumes the already configured main path. */
STATIC EFI_STATUS AccessCharger(UINTN Index, UINT8 *ChargerStatus, INT32 Command) {
  STATIC CONST UINTN Bases[]={0x222a0000,0x22260000};
  STATIC CONST UINT32 Gates[]={BIT16,BIT12};
  STATIC CONST UINTN Offsets[]={0,4,8,0x18,0x20,0x24,0x28};
  STATIC UINTN Logs[2], BusyLogs[2];
  UINTN Base=Bases[Index],I,Budget=Command<0?4000:8000;
  UINT32 Gate, Saved[ARRAY_SIZE(Offsets)],State,Control;
  UINT8 Id=0,Raw=0;
  EFI_STATUS Status;
  if (mBusFaulted[Index]) return EFI_DEVICE_ERROR;
  Gate=MmioRead32(I2C_GATE);
  MmioWrite32(I2C_GATE,Gate|Gates[Index]);
  State=MmioRead32(Base+0x14);
  Control=MmioRead32(Base);
  if ((State&(I2C_BUSY|I2C_IRQ)) || (Control&I2C_START)) {
    if (BusyLogs[Index]++<2) DEBUG((DEBUG_WARN,"W202_I2C base=%lx ctl=%x status=%x unavailable\n",Base,Control,State));
    Status=EFI_NOT_READY; goto GateDone;
  }
  for (I=0;I<ARRAY_SIZE(Offsets);++I) Saved[I]=MmioRead32(Base+Offsets[I]);
  MmioWrite32(Base,0);
  /* Inherited source is 26 MHz. Use the same safe 100 kHz divisors as LK. */
  MmioWrite32(Base+0x20,0x0033004c); MmioWrite32(Base+0x24,0);
  MmioWrite32(Base+0x28,104); MmioWrite32(Base+0x18,0);
  Control=BIT22|BIT21|BIT19|BIT13|BIT8|BIT1|I2C_ENABLE;
  MmioWrite32(Base,Control);
  Status=ReadByte(Base,Control,0x0b,&Id,&Budget);
  if (!EFI_ERROR(Status) && ((Id>>3)&0xf)!=2 && ((Id>>3)&0xf)!=9)
    Status=EFI_UNSUPPORTED;
  if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,0x08,&Raw,&Budget);
  if (!EFI_ERROR(Status) && Command==-2) Status=ReadByte(Base,Control,1,&Raw,&Budget);
  if (!EFI_ERROR(Status) && Command>=0) {
    UINT8 Enable=0, Input=0, Rate=0, Voltage=0, Timer=0, Dpm=0, Fault=0, FaultNow=0, Got;
    Status=ReadByte(Base,Control,1,&Enable,&Budget);
    if (!EFI_ERROR(Status) && Command) {
      /* Exact profile observed in stock startup and r36. Never enable from
       * an unknown/reset profile or clear HIZ/OTG to force power through. */
      Status=ReadByte(Base,Control,0,&Input,&Budget);
      if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,2,&Rate,&Budget);
      if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,4,&Voltage,&Budget);
      if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,5,&Timer,&Budget);
      if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,6,&Dpm,&Budget);
      /* REG09 is latched: consider both the old and the current fault. */
      if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,9,&Fault,&Budget);
      if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,9,&FaultNow,&Budget);
      if (!EFI_ERROR(Status) && (Index!=0 || Id!=0x14 || Input!=0x14 ||
          Rate!=0xa3 || Voltage!=0x90 || Timer!=0x87 || Dpm!=0x66 ||
          (Enable&~(BIT4|BIT6))!=0x8a || Fault || FaultNow || ((Raw>>5)&7)==7))
        Status=EFI_ACCESS_DENIED;
      /* PMIC cable-detect can precede the charger's power-good / source
       * detection after replug. Keep both channels off and retry next sample.
       * An unknown profile, OTG or a fault remains a hard rejection above. */
      if (!EFI_ERROR(Status) && (!(Raw&BIT2) || ((Raw>>5)&7)==0))
        Status=EFI_NOT_READY;
      if (Index==0) {
        ++mDecision.Attempts; mDecision.Status=Status; mDecision.Id=Id;
        mDecision.Input=Input; mDecision.Enable=Enable; mDecision.Rate=Rate;
        mDecision.Voltage=Voltage; mDecision.Timer=Timer; mDecision.Dpm=Dpm;
        mDecision.State=Raw; mDecision.Fault=Fault; mDecision.FaultNow=FaultNow;
      }
    }
    if (!EFI_ERROR(Status)) {
      UINT8 Desired=(UINT8)((Enable&~(BIT4|BIT6))|(Command?BIT4:0));
      if ((Enable&BIT4)!=(Desired&BIT4)) {
        /* A failed transfer may still have reached the chip. Retain the
         * obligation to stop until a readback confirms CHG_CONFIG is clear. */
        if (Command) mChargeOwned=TRUE;
        Status=WriteByte(Base,Control,1,Desired,&Budget);
        if (!EFI_ERROR(Status)) Status=ReadByte(Base,Control,1,&Got,&Budget);
        if (!EFI_ERROR(Status) && (Got&BIT4)!=(Desired&BIT4)) Status=EFI_DEVICE_ERROR;
      }
      if (!EFI_ERROR(Status) && Command) mChargeOwned=TRUE;
    }
  }
  if (!EFI_ERROR(Status) && Logs[Index]++<2) {
    UINT8 Input=0,Enable=0,Timer=0;
    EFI_STATUS A=ReadByte(Base,Control,0,&Input,&Budget);
    EFI_STATUS B=EFI_ERROR(A)?A:ReadByte(Base,Control,1,&Enable,&Budget);
    EFI_STATUS C=EFI_ERROR(B)?B:ReadByte(Base,Control,5,&Timer,&Budget);
    DEBUG((DEBUG_WARN,"W202_BQ_CONFIG bus=%u id=%02x input=%02x enable=%02x timer=%02x reads=%r,%r,%r\n",(UINT32)Index,Id,Input,Enable,Timer,A,B,C));
    if (Index==1) DEBUG((DEBUG_WARN,"W202_BQ_GPIO187 data=%x mask=%x dir=%x\n",MmioRead32(0x64170580),MmioRead32(0x64170584),MmioRead32(0x64170588)));
  }
  /* This IP ignores interrupt-status writes while I2C_EN is clear. Retire
   * the completion while enabled, then disable before restoring registers. */
  MmioWrite32(Base,Control);
  State=MmioRead32(Base+0x14);
  MmioWrite32(Base+0x14,State&~(I2C_IRQ|I2C_NACK));
  MmioWrite32(Base+0x2c,1);
  for (I=0;I<100 && (MmioRead32(Base+0x14)&I2C_BUSY);++I) MicroSecondDelay(1);
  if (MmioRead32(Base+0x14)&I2C_BUSY) {
    mBusFaulted[Index]=TRUE; Status=EFI_DEVICE_ERROR;
  }
  MmioWrite32(Base,0);
  for (I=1;I<ARRAY_SIZE(Offsets);++I) MmioWrite32(Base+Offsets[I],Saved[I]);
  MmioWrite32(Base,Saved[0]);
  if (!EFI_ERROR(Status)) *ChargerStatus=Raw;
GateDone:
  /* Preserve unrelated AP gate changes; restore just the borrowed I2C bit. */
  State=MmioRead32(I2C_GATE);
  MmioWrite32(I2C_GATE,(State&~Gates[Index])|(Gate&Gates[Index]));
  return Status;
}

STATIC EFI_STATUS ReadCharger(UINTN Index, UINT8 *Status) {
  return AccessCharger(Index,Status,-1);
}

EFI_STATUS W202ChargeReadEnabled(UINT32 *Enabled) {
  UINT8 A=0,B=0;
  EFI_STATUS SA,SB;
  if (Enabled==NULL) return EFI_INVALID_PARAMETER;
  SA=AccessCharger(0,&A,-2); SB=AccessCharger(1,&B,-2);
  if (EFI_ERROR(SA)) return SA;
  if (EFI_ERROR(SB)) return SB;
  *Enabled=((A|B)&BIT4)?1:0;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS StopChannels(VOID) {
  UINT8 Raw;
  EFI_STATUS A,B;
  /* Stop inherited charging too, and always attempt both independent buses. */
  A=AccessCharger(0,&Raw,0); B=AccessCharger(1,&Raw,0);
  if (!EFI_ERROR(A)) mChargeOwned=FALSE;
  return EFI_ERROR(A)?A:B;
}

EFI_STATUS W202ChargeReadRequested(UINT32 *Requested) {
  if (Requested==NULL) return EFI_INVALID_PARAMETER;
  *Requested=mChargeRequested?1:0; return EFI_SUCCESS;
}
EFI_STATUS W202ChargeStop(VOID) {
  mChargeRequested=FALSE;
  return StopChannels();
}

EFI_STATUS W202ChargeUpdate(CONST PJB_POWER_SAMPLE *Sample, UINT32 NtcMv) {
  UINT32 Needed=PJB_POWER_VOLTAGE|PJB_POWER_CURRENT|PJB_POWER_TEMPERATURE|PJB_POWER_EXTERNAL;
  UINT32 Mask=0,Data=0,Enabled=0;
  UINT8 Raw;
  EFI_STATUS Status,Stop;
  BOOLEAN Safe;
  if (Sample==NULL) return EFI_INVALID_PARAMETER;
  if (!mChargeRequested) return StopChannels();
  if ((Sample->valid&PJB_POWER_EXTERNAL) && !Sample->external_power) {
    Status=StopChannels();
    if (EFI_ERROR(Status)) mChargeRequested=FALSE;
    return EFI_ERROR(Status)?Status:EFI_NOT_READY;
  }
  /* Retained stock 4432mV profile, stock NTC window and battery presence. */
  Safe=(Sample->valid&Needed)==Needed && Sample->external_power &&
       NtcMv>=357 && NtcMv<=1304 && Sample->voltage_mv>=3450 && Sample->voltage_mv<4450;
  if (Safe) Safe=!EFI_ERROR(W202AdiRead(0x64420284,&Mask)) && (Mask&BIT3) &&
                !EFI_ERROR(W202AdiRead(0x64420280,&Data)) && (Data&BIT3);
  /* Never enable the secondary path. Confirm it is stopped before main enable. */
  Status=Safe?AccessCharger(1,&Raw,0):EFI_ACCESS_DENIED;
  if (!EFI_ERROR(Status)) Status=AccessCharger(0,&Raw,1);
  if (!EFI_ERROR(Status)) Status=W202ChargeReadEnabled(&Enabled);
  if (!EFI_ERROR(Status) && Enabled!=1) Status=EFI_DEVICE_ERROR;
  if (EFI_ERROR(Status)) {
    /* EFI_NOT_READY is a busy bus or source detection, never permission to
     * charge. Resume only after a fresh sample passes every check again. */
    Stop=Status==EFI_NOT_READY?StopChannels():W202ChargeStop();
    if (EFI_ERROR(Stop)) { mChargeRequested=FALSE; Status=Stop; }
  }
  return Status;
}

EFI_STATUS W202ChargeRequest(CONST PJB_POWER_SAMPLE *Sample,UINT32 NtcMv,UINT32 Enabled) {
  if (Enabled>1 || Sample==NULL) return EFI_INVALID_PARAMETER;
  if (!Enabled) return W202ChargeStop();
  mChargeRequested=TRUE;
  return W202ChargeUpdate(Sample,NtcMv);
}

EFI_STATUS W202ReadCharging(UINT32 *Charging) {
  STATIC UINTN Logs;
  UINT8 Raw[2]={0};
  EFI_STATUS A=ReadCharger(0,&Raw[0]), B=ReadCharger(1,&Raw[1]);
  BOOLEAN ActiveA=!EFI_ERROR(A) && (Raw[0]&BIT2) && ((Raw[0]>>5)&7)!=7 &&
                  (((Raw[0]>>3)&3)==1 || ((Raw[0]>>3)&3)==2);
  BOOLEAN ActiveB=!EFI_ERROR(B) && (Raw[1]&BIT2) && ((Raw[1]>>5)&7)!=7 &&
                  (((Raw[1]>>3)&3)==1 || ((Raw[1]>>3)&3)==2);
  if (Logs++<2) DEBUG((DEBUG_WARN,"W202_BQ main=%02x slave=%02x reads=%r,%r\n",Raw[0],Raw[1],A,B));
  if (ActiveA || ActiveB) { *Charging=1; return EFI_SUCCESS; }
  /* A failed peer may still be charging: never report false from one chip. */
  if (EFI_ERROR(A)) return A;
  if (EFI_ERROR(B)) return B;
  *Charging=0;
  return EFI_SUCCESS;
}
