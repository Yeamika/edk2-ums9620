/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Explicit LK/EFI handoff ABI, supplied to LK by HANDOFF_INCLUDE.
 * The suspended loader owns the context; EFI only consumes this descriptor.
 */
#ifndef W202_LK_RETURN_H
#define W202_LK_RETURN_H
#define W202_LK_RETURN_ADDRESS 0x82ffe200
#define W202_LK_CONTEXT_ADDRESS 0x86400000
#define W202_LK_CONTEXT_LIMIT 0x10000
#define W202_LK_RETURN_MAGIC 0x4c4b5231
#define W202_LK_RETURN_VERSION 1
#define W202_LK_RETURN_PROBE 0
#define W202_LK_RETURN_ANDROID 1
#ifndef __ASSEMBLER__
#ifdef W202_LK_RETURN_EFI
#include <Uefi.h>
typedef UINT32 W202_LK_U32;
typedef UINT64 W202_LK_U64;
#else
#include <stdint.h>
typedef uint32_t W202_LK_U32;
typedef uint64_t W202_LK_U64;
#endif
typedef struct {
  W202_LK_U32 magic, version, size, checksum;
  W202_LK_U64 resume, context;
  W202_LK_U32 context_size, capabilities;
} W202_LK_RETURN;
static inline W202_LK_U32 W202LkReturnSum(const W202_LK_RETURN *h) {
  return h->magic+h->version+h->size+h->checksum+(W202_LK_U32)h->resume+
    (W202_LK_U32)(h->resume>>32)+(W202_LK_U32)h->context+(W202_LK_U32)(h->context>>32)+
    h->context_size+h->capabilities;
}
static inline int W202LkReturnValid(const W202_LK_RETURN *h) {
  return h->magic==W202_LK_RETURN_MAGIC && h->version==W202_LK_RETURN_VERSION &&
    h->size==sizeof(*h) && h->context==W202_LK_CONTEXT_ADDRESS &&
    h->context_size>=512 && h->context_size<=W202_LK_CONTEXT_LIMIT &&
    h->resume>=0x86000020 && h->resume<0x86010000 && !(h->resume&3) &&
    h->capabilities==3 && W202LkReturnSum(h)==0;
}
#endif
#endif
