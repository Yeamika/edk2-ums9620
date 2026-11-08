// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/ArmPlatformLib.h>
#include "LkTrace.h"
// MMIO facts: sprd-linux/lk@47f4d04, qogirn6pro hardware.h.
// DRAM below is a proposed handoff reservation, NOT a discovered full RAM map.
STATIC ARM_MEMORY_REGION_DESCRIPTOR mMap[] = {
  {0x10000000, 0x10000000, 0x20000000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {0x31000000, 0x31000000, 0x00010000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {0x31300000, 0x31300000, 0x00010000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {0x64000000, 0x64000000, 0x01000000, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE},
  {0x82800000, 0x82800000, 0x03800000, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK},
  {0xb0000000, 0xb0000000, 0x00500000, ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED},
  // OS staging only, not UEFI allocator memory. OS applications validate this
  // range against DRAM and every FDT reservation before writing any payload.
  {0xd0000000, 0xd0000000, 0x08200000, ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK},
  {0, 0, 0, 0}
};
VOID ArmPlatformGetVirtualMemoryMap(ARM_MEMORY_REGION_DESCRIPTOR **Map) {
  LkTraceStage (0x32);
  *Map = mMap;
}
