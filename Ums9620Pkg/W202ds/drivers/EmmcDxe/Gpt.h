// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef W202_EMMC_GPT_H
#define W202_EMMC_GPT_H
#include <Uefi.h>
typedef EFI_STATUS (*W202_READ_SECTOR)(UINT8 Command, UINT32 Lba, UINT8 *Data);
EFI_STATUS W202FindWritableEsp(W202_READ_SECTOR Read, UINT32 Sectors, EFI_LBA *First, EFI_LBA *Last);
#endif
