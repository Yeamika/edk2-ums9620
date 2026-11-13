/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Optional, polled LK service. Never transfers display ownership to LK.
 */
#ifndef W202_LK_FASTBOOT_H
#define W202_LK_FASTBOOT_H
#define W202_LK_FB_ADDRESS 0x82ffe240
#define W202_LK_FB_MAGIC 0x3142464c
#define W202_LK_FB_CONTEXT 0x86408000
#define W202_LK_FB_STACK 0x86440000
#define W202_LK_FB_START 1
#define W202_LK_FB_POLL 2
#define W202_LK_FB_STOP 3
#define W202_LK_FB_BUSY 1
#define W202_LK_FB_CONTINUE 2
#define W202_LK_FB_REBOOT 4
#define W202_LK_FB_BOOTLOADER 8
#ifndef __ASSEMBLER__
#ifdef W202_LK_RETURN_EFI
#include <Uefi.h>
typedef UINT32 W202_FB_U32;
typedef UINT64 W202_FB_U64;
#else
#include <stdint.h>
typedef uint32_t W202_FB_U32;
typedef uint64_t W202_FB_U64;
#endif
typedef struct {
    W202_FB_U32 magic, version, size, checksum;
    W202_FB_U64 entry;
    W202_FB_U32 flags, reserved;
} W202_LK_FASTBOOT_DESCRIPTOR;
static inline W202_FB_U32 W202LkFastbootSum(const W202_LK_FASTBOOT_DESCRIPTOR *h) {
    return h->magic+h->version+h->size+h->checksum+(W202_FB_U32)h->entry+
        (W202_FB_U32)(h->entry>>32)+h->flags+h->reserved;
}
static inline int W202LkFastbootValid(const W202_LK_FASTBOOT_DESCRIPTOR *h) {
    return h->magic==W202_LK_FB_MAGIC && h->version==1 && h->size==sizeof(*h) &&
        h->entry>=0x86000020 && h->entry<0x86010000 && !(h->entry&3) &&
        h->flags==3 /* polled + no display */ && !h->reserved && !W202LkFastbootSum(h);
}
#endif
#endif
