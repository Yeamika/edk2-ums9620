/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Board-owned LK -> EDK2 record. No function pointers or LK runtime access.
 * Two separately reserved RAM pages hold this record and the trace. Legacy
 * 8 MiB FDs included these pages in their erased tail; new 2 MiB FDs do not.
 */
#ifndef UMS9620_LOADER_HANDOFF_H
#define UMS9620_LOADER_HANDOFF_H
#ifndef PJB_EDK2
#include <stdint.h>
#endif
#define W202_HANDOFF_ADDRESS 0x82ffe000u
#define W202_HANDOFF_MAGIC 0x57483232u
#define W202_HANDOFF_ADI 1u
#define W202_HANDOFF_CALIBRATION 2u
#define W202_HANDOFF_BATDET_CALIBRATION 4u
#define W202_HANDOFF_RESIDENT_WATCHDOG 8u
typedef struct {
  uint32_t magic, version, size, checksum;
  uint32_t flags;
  int32_t voltage_gain, voltage_offset;
  uint32_t reserved;
  /* Version 2: stock LK's chip-calibrated channel-0 transfer function. */
  uint32_t ntc_high_mv, ntc_high_raw, ntc_low_mv, ntc_low_raw;
} W202_LOADER_HANDOFF;
/* Version 1 covered eight words; version 2 also covers the four NTC words. */
static inline uint32_t W202HandoffSum(const W202_LOADER_HANDOFF *h) {
  uint32_t sum=h->magic+h->version+h->size+h->checksum+h->flags+
    (uint32_t)h->voltage_gain+(uint32_t)h->voltage_offset+h->reserved;
  if (h->version==2) sum+=h->ntc_high_mv+h->ntc_high_raw+h->ntc_low_mv+h->ntc_low_raw;
  return sum;
}
static inline int W202HandoffValid(const W202_LOADER_HANDOFF *h) {
  return h->magic==W202_HANDOFF_MAGIC &&
    ((h->version==1 && h->size==32 && (h->flags & ~3u)==0) ||
     (h->version==2 && h->size==sizeof(*h) && (h->flags & ~15u)==0)) &&
    h->reserved==0 && W202HandoffSum(h)==0;
}
#endif
