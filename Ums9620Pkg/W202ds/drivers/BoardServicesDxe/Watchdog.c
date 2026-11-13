// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"

/* UMP9620 watchdog, verified against stock start_watchdog and sprd_wdt.h.
 * Renew only a loader-owned, already running reset watchdog. Never enable
 * clocks, change its control/interrupt mode or disarm recovery on failure.
 * The event stops renewing while quiesced and after ExitBootServices.
 */
#define WDG_LOW  0x64420040u
#define WDG_HIGH 0x64420044u
#define WDG_CTRL 0x64420048u
#define WDG_RAW  0x64420050u
#define WDG_LOCK 0x64420060u

STATIC EFI_STATUS Write(UINT32 Address, UINT32 Value) {
  UINTN Budget;
  if ((Address!=WDG_LOW && Address!=WDG_HIGH && Address!=WDG_LOCK) || Value>0xffff)
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

EFI_STATUS W202WatchdogRenew(VOID) {
  UINT32 Control, Raw;
  UINTN Budget;
  EFI_STATUS Status, LockStatus;
  Status=W202AdiRead(WDG_CTRL,&Control);
  if (EFI_ERROR(Status)) return Status;
  if ((Control&(BIT0|BIT1|BIT3))!=(BIT1|BIT3)) return EFI_UNSUPPORTED;
  Status=Write(WDG_LOCK,0xe551);
  if (EFI_ERROR(Status)) { Write(WDG_LOCK,0x1aae); return Status; }
  for (Budget=0;Budget<2000;++Budget) {
    Status=W202AdiRead(WDG_RAW,&Raw);
    if (EFI_ERROR(Status) || !(Raw&BIT4)) break;
    MicroSecondDelay(1);
  }
  if (Budget==2000) Status=EFI_TIMEOUT;
  // 180 seconds at 32768 Hz = 0x005a0000; load high before low.
  if (!EFI_ERROR(Status)) Status=Write(WDG_HIGH,0x5a);
  if (!EFI_ERROR(Status)) Status=Write(WDG_LOW,0);
  LockStatus=Write(WDG_LOCK,0x1aae);
  if (!EFI_ERROR(Status)) Status=LockStatus;
  return Status;
}
