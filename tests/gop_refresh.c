/* Real refresh function with a stopped, streaming or stalled DPU. */
#include <Uefi.h>
#define W202_LK_TRACE_H
STATIC VOID LkTraceStage(UINT64 Stage) {}
#include "../Ums9620Pkg/Drivers/LkGopDxe/LkGopDxe.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
VOID EFIAPI MemoryFence(VOID) {}
static UINT32 ctrl,raw,ack;
static UINTN delay;
static BOOLEAN stalled,bad;
UINT32 EFIAPI MmioRead32(UINTN a) {
  if(a==DPU_REG(8)) return ctrl;
  if(a==DPU_REG(0x25c)) return raw;
  bad=TRUE; return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  if(a==DPU_REG(0x254)) {ack=v;raw&=~v;}
  else if(a==DPU_REG(8)) {
    ctrl=v;
    if (!stalled && (ctrl&BIT0)) raw|=(mDpuCfg&(BIT0|BIT1))?BIT0:BIT18;
  } else bad=TRUE;
  return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN n) {delay+=n;return n;}
int main(void) {
  mDpuCfg=0;ctrl=0;CHECK(Refresh()==EFI_SUCCESS && (ctrl&BIT0) && (ack&BIT18));
  ctrl=BIT0;CHECK(Refresh()==EFI_SUCCESS && (ctrl&BIT4));
  mDpuCfg=BIT1;ctrl=0;CHECK(Refresh()==EFI_SUCCESS && (ack&BIT0));
  mDpuCfg=BIT0;ctrl=0;CHECK(Refresh()==EFI_SUCCESS);
  stalled=TRUE;raw=BIT0|BIT18;delay=0;
  CHECK(Refresh()==EFI_TIMEOUT && delay==150000 && !bad);
  return 0;
}
