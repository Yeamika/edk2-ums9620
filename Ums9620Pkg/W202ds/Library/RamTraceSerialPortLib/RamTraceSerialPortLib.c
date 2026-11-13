// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Base.h>
#include <Library/SerialPortLib.h>

#define TRACE_BASE  0x82fff000ULL
#define TRACE_MAGIC 0x57323032484e4431ULL
#define LOG_CAPACITY 4032U

RETURN_STATUS EFIAPI SerialPortInitialize (VOID) { return RETURN_SUCCESS; }

UINTN EFIAPI SerialPortWrite (IN UINT8 *Buffer, IN UINTN NumberOfBytes) {
  volatile UINT64 *Trace = (volatile UINT64 *)(UINTN)TRACE_BASE;
  volatile UINT8 *Log = (volatile UINT8 *)(UINTN)(TRACE_BASE + 64);
  UINT64 Total, Ctr;
  UINTN Count, Index, Line, Address;
  if (Buffer == NULL || Trace[0] != TRACE_MAGIC) return 0;
  Total = Trace[7];
  if (Total > 0xffffffffULL || NumberOfBytes > 0xffffffffULL - Total) return 0;
  Count = NumberOfBytes;
  if (Count > LOG_CAPACITY) {
    Buffer += Count - LOG_CAPACITY;
    Total += Count - LOG_CAPACITY;
    Count = LOG_CAPACITY;
  }
  for (Index = 0; Index < Count; ++Index) Log[(Total + Index) % LOG_CAPACITY] = Buffer[Index];
  Trace[7] = Total + Count;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (Ctr));
  Line = (UINTN)4 << ((Ctr >> 16) & 15);
  if (Line < 16 || Line > 2048) return 0;
  for (Address = (UINTN)TRACE_BASE; Address < (UINTN)TRACE_BASE + 4096; Address += Line)
    __asm__ volatile ("dc cvac, %0" :: "r" (Address) : "memory");
  __asm__ volatile ("dsb sy" ::: "memory");
  return NumberOfBytes;
}

UINTN EFIAPI SerialPortRead (OUT UINT8 *Buffer, IN UINTN NumberOfBytes) { return 0; }
BOOLEAN EFIAPI SerialPortPoll (VOID) { return FALSE; }
RETURN_STATUS EFIAPI SerialPortSetControl (IN UINT32 Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortGetControl (OUT UINT32 *Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortSetAttributes (
  IN OUT UINT64 *BaudRate, IN OUT UINT32 *ReceiveFifoDepth, IN OUT UINT32 *Timeout,
  IN OUT EFI_PARITY_TYPE *Parity, IN OUT UINT8 *DataBits, IN OUT EFI_STOP_BITS_TYPE *StopBits
  ) { return RETURN_UNSUPPORTED; }
