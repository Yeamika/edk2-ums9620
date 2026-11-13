// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/ArmPlatformLib.h>
#include "LkTrace.h"
#include "../../Include/W202dsMemory.h"
// MMIO facts: sprd-linux/lk@47f4d04, qogirn6pro hardware.h.
// DRAM below is a proposed handoff reservation, NOT a discovered full RAM map.
STATIC ARM_MEMORY_REGION_DESCRIPTOR mMap[] = {
  {0x10000000, 0x10000000, 0x20000000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {0x31000000, 0x31000000, 0x00010000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {0x31300000, 0x31300000, 0x00010000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {0x64000000, 0x64000000, 0x01000000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {W202DS_UEFI_BOOT_MEMORY_BASE, W202DS_UEFI_BOOT_MEMORY_BASE,
   W202DS_UEFI_BOOT_MEMORY_END - W202DS_UEFI_BOOT_MEMORY_BASE,
   ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK},
  // Full reserved logobuffer, including the 9,216,000-byte 32-bit scanout.
  // Still excluded from all allocator banks and OS payload staging.
  {0xb0000000, 0xb0000000, 0x009e4000, ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED},
#ifdef W202_LK_FASTBOOT
  // Suspended LK bridge/context only; not allocator memory. Native LK text,
  // heap, USB DMA and download buffers are accessed under its own TTBR.
  {0x86000000, 0x86000000, 0x00040000, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK},
  {0x86400000, 0x86400000, 0x00040000, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK},
#endif
  {W202DS_UEFI_EXTRA_MEMORY_BASE, W202DS_UEFI_EXTRA_MEMORY_BASE,
   W202DS_UEFI_EXTRA_MEMORY_SIZE, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK},
  // OS staging only, not UEFI allocator memory. OS applications validate this
  // range against DRAM and every FDT reservation before writing any payload.
  {W202DS_OS_STAGING_BASE, W202DS_OS_STAGING_BASE,
   W202DS_OS_STAGING_END - W202DS_OS_STAGING_BASE,
   ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK},
  {0, 0, 0, 0}
};
VOID ArmPlatformGetVirtualMemoryMap(ARM_MEMORY_REGION_DESCRIPTOR **Map) {
  LkTraceStage (0x32);
  *Map = mMap;
}
