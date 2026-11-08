// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DebugLib.h>

/* qogirn6pro/UMP9620: sprd-linux/lk reset.c, rtc_reg_v3.h and
 * __regs_ana_ump9620_glb.h. Only called while Boot Services own physical MMIO.
 * The caller excludes other ADI users. Every hardware wait is bounded. */
#define ADI_READ 0x64400028
#define ADI_DATA 0x6440002c
#define ADI_FIFO 0x64400030
#define RTC_STATUS 0x64420234
#define RTC_CLEAR 0x64420238
#define RTC_MASK 0xff1f
#define POWER_PD 0x64422020
#define POWER_UNLOCK 0x644223d0

STATIC EFI_STATUS Read(UINT32 Address, UINT32 *Value) {
  UINTN Count;
  if (MmioRead32(ADI_DATA)&BIT31) return EFI_NOT_READY;
  MmioWrite32(ADI_READ,Address);
  for (Count=0;Count<2000;++Count) {
    UINT32 Data=MmioRead32(ADI_DATA);
    if (!(Data&BIT31)) { *Value=Data&0xffff; return EFI_SUCCESS; }
    MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS Write(UINT32 Address, UINT32 Value) {
  UINTN Count;
  if ((Address!=RTC_CLEAR && Address!=POWER_UNLOCK && Address!=POWER_PD) || Value>0xffff)
    return EFI_INVALID_PARAMETER;
  for (Count=0;Count<2000;++Count) {
    UINT32 Fifo=MmioRead32(ADI_FIFO);
    if ((Fifo&BIT10) && !(Fifo&(BIT11|BIT31))) break;
    MicroSecondDelay(1);
  }
  if (Count==2000) return EFI_TIMEOUT;
  MmioWrite32(Address,Value);
  for (Count=0;Count<2000;++Count) {
    UINT32 Fifo=MmioRead32(ADI_FIFO);
    if ((Fifo&BIT10) && !(Fifo&BIT31)) return EFI_SUCCESS;
    MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS W202PowerOff(VOID) {
  EFI_STATUS Status;
  UINT32 Value;
  UINTN Count;
  Status=Write(RTC_CLEAR,RTC_MASK);
  if (EFI_ERROR(Status)) return Status;
  for (Count=0;Count<200;++Count) {
    Status=Read(RTC_STATUS,&Value);
    if (EFI_ERROR(Status)) return Status;
    if (!(Value&RTC_MASK)) break;
    MicroSecondDelay(10);
  }
  if (Count==200) return EFI_TIMEOUT;
  Status=Write(POWER_UNLOCK,0x6e7f);
  if (EFI_ERROR(Status)) goto Relock;
  for (Count=0;Count<200;++Count) {
    Status=Read(POWER_UNLOCK,&Value);
    if (EFI_ERROR(Status)) goto Relock;
    if (Value&BIT15) break;
    MicroSecondDelay(10);
  }
  if (Count==200) { Status=EFI_TIMEOUT; goto Relock; }
  Status=Read(POWER_PD,&Value);
  if (EFI_ERROR(Status)) goto Relock;
  DEBUG((DEBUG_WARN,"W202_SHUTDOWN pmic-trigger old=%04x\n",Value));
  Status=Write(POWER_PD,Value|BIT0);
  if (EFI_ERROR(Status)) goto Relock;
  MicroSecondDelay(100000);
  /* Power should be gone. Returning permits the UI to report failure. */
  Status=EFI_DEVICE_ERROR;
Relock:
  Write(POWER_UNLOCK,0);
  return Status;
}
