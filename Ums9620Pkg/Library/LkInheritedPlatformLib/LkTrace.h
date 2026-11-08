// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef W202_LK_TRACE_H
#define W202_LK_TRACE_H

#include <Base.h>

STATIC
VOID
LkTraceStage (
  UINT64  Stage
  )
{
  volatile UINT64  *Trace = (volatile UINT64 *)(UINTN)0x82fff000;
  if (Trace[0] == 0x57323032484e4431ULL) {
    Trace[1] = Stage;
    __asm__ volatile ("dc cvac, %0; dsb sy" :: "r" (Trace) : "memory");
  }
}

#endif
