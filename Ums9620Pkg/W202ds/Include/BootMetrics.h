/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Board diagnostics in the already reserved handoff page; no ABI pointers.
 */
#ifndef W202_BOOT_METRICS_H
#define W202_BOOT_METRICS_H
#define W202_BOOT_METRICS_ADDRESS 0x82ffe100u
#define W202_BOOT_METRICS_MAGIC 0x57323032424f4f54ull
typedef struct {
  unsigned long long magic, start_counter, frequency, app_counter;
} W202_BOOT_METRICS;
#endif
