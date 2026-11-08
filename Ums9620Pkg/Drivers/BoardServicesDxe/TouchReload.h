// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef W202_TOUCH_RELOAD_H
#define W202_TOUCH_RELOAD_H
#include <Uefi.h>

#define W202_TOUCH_CONFIG_SIZE 40720U
typedef struct {
  EFI_STATUS (*Read)(UINT32 Address, UINT32 *Value);
  EFI_STATUS (*Write)(UINT32 Address, UINT32 Value);
  EFI_STATUS (*WriteMemory)(UINT32 Address, CONST UINT8 *Data, UINTN Size);
  EFI_STATUS (*Sense)(BOOLEAN Safe);
  VOID (*Delay)(UINTN Microseconds);
} W202_TOUCH_RELOAD_IO;
typedef struct {
  UINT32 Stage, EntityCrc, ConfigCrc, State, Reload;
} W202_TOUCH_RELOAD_RESULT;

EFI_STATUS W202TouchReload(CONST UINT8 *Firmware, UINTN Size, UINT8 *Config,
                          CONST W202_TOUCH_RELOAD_IO *Io,
                          W202_TOUCH_RELOAD_RESULT *Result);
#endif
