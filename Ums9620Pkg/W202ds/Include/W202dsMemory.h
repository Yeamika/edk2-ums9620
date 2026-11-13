// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef W202DS_MEMORY_H
#define W202DS_MEMORY_H

// Identity-mapped, write-back DRAM. Keep these assembly-safe: EFI handoff
// must clean every allocator bank before disabling caches and translation.
#define W202DS_UEFI_BOOT_MEMORY_BASE   0x82800000
#define W202DS_UEFI_BOOT_MEMORY_END    0x86000000

// Separate 72 MiB bank; total allocator RAM is 56 + 72 = 128 MiB.
#define W202DS_UEFI_EXTRA_MEMORY_BASE  0xc8000000
#define W202DS_UEFI_EXTRA_MEMORY_SIZE  0x04800000
#define W202DS_UEFI_EXTRA_MEMORY_END   (W202DS_UEFI_EXTRA_MEMORY_BASE + W202DS_UEFI_EXTRA_MEMORY_SIZE)

// Cacheable OS payload staging, excluded from the UEFI allocator.
#define W202DS_OS_STAGING_BASE         0xd0000000
#define W202DS_OS_STAGING_END          0xd8200000

#endif
