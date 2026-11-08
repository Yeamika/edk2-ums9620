// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef W202_HARDWARE_H
#define W202_HARDWARE_H
#include <Uefi.h>
#include <Protocol/AbsolutePointer.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#define PJB_EDK2
#include <pocketjs_boot_context.h>
#include "../../Include/LoaderHandoff.h"
#include "../../Include/BootMetrics.h"
EFI_STATUS W202ReadPower(CONST W202_LOADER_HANDOFF *Handoff, PJB_POWER_SAMPLE *Sample);
VOID W202PowerReset(VOID);
EFI_STATUS W202ReadTemperature(CONST W202_LOADER_HANDOFF *Handoff,
                               CONST PJB_POWER_SAMPLE *Sample, INT32 *Temperature);
EFI_STATUS W202ReadCharging(UINT32 *Charging);
EFI_STATUS W202NtcVoltage(UINT32 *Mv);
EFI_STATUS W202ChargeUpdate(CONST PJB_POWER_SAMPLE *Sample, UINT32 NtcMv);
EFI_STATUS W202ChargeStop(VOID);
VOID W202ChargeTrace(VOID);
EFI_STATUS W202ChargeReadEnabled(UINT32 *Enabled);
EFI_STATUS W202ChargeReadRequested(UINT32 *Requested);
EFI_STATUS W202ChargeRequest(CONST PJB_POWER_SAMPLE *Sample,UINT32 NtcMv,UINT32 Enabled);
EFI_STATUS W202TouchStart(VOID);
VOID W202TouchStop(VOID);
EFI_STATUS W202TouchRead(EFI_ABSOLUTE_POINTER_STATE *State);
EFI_STATUS W202TouchIrqInitialize(EFI_EVENT Event);
EFI_STATUS W202TouchIrqStart(VOID);
VOID W202TouchIrqStop(VOID);
VOID W202TouchIrqExit(VOID);
BOOLEAN W202TouchIrqFaulted(VOID);
VOID W202TouchIrqTrace(VOID);
VOID W202TouchStartupTrace(VOID);
#ifdef W202_TOUCH_IRQ_DIAG
VOID W202TouchProbeKey(VOID);
#endif
EFI_STATUS W202AdiRead(UINT32 Address, UINT32 *Value);
EFI_STATUS W202WatchdogRenew(VOID);
EFI_STATUS W202KeysInit(VOID);
VOID W202KeysPoll(VOID);
VOID W202KeysEnable(BOOLEAN Enable);
VOID W202KeysExit(VOID);
/* Pure parser, shared by firmware and host tests. 10 contacts, 56 byte header. */
int W202ParseTouch(const unsigned char *Packet, unsigned Length,
                  unsigned *X, unsigned *Y, unsigned *Down);
#endif
