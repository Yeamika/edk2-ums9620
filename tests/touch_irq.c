/* Exercise the actual IRQ driver with a mocked GIC and EIC register bank. */
#include "../Ums9620Pkg/W202ds/drivers/BoardServicesDxe/TouchIrq.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static UINT32 regs[6][0x100/4];
static unsigned signals, eois, disables, enables, registered, clears;
static EFI_STATUS enable_status;
static HARDWARE_INTERRUPT_HANDLER handler;
EFI_GUID gHardwareInterruptProtocolGuid;
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
static UINT32 *reg(UINTN address) {
  UINTN offset=address-0x64200000,bank=offset/0x10000,local=offset%0x10000;
  if (bank>=6 || local>=0x100 || (local&3)) return 0;
  return &regs[bank][local/4];
}
UINT32 EFIAPI MmioRead32(UINTN address) { UINT32 *p=reg(address);return p?*p:0; }
UINT32 EFIAPI MmioWrite32(UINTN address, UINT32 value) {
  UINT32 *p=reg(address);if(p)*p=value;
  if(address==EIC_BASE+EIC_CLEAR) {++clears;regs[2][(0xa0+EIC_MASKED)/4]&=~value;}
  return value;
}
UINT32 EFIAPI MmioAndThenOr32(UINTN address, UINT32 and, UINT32 or) {
  return MmioWrite32(address,(MmioRead32(address)&and)|or);
}
static EFI_STATUS EFIAPI Register(EFI_HARDWARE_INTERRUPT_PROTOCOL *p,HARDWARE_INTERRUPT_SOURCE source,HARDWARE_INTERRUPT_HANDLER h) {
  if(source!=EIC_GIC)return EFI_INVALID_PARAMETER;handler=h;registered=h!=NULL;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Enable(EFI_HARDWARE_INTERRUPT_PROTOCOL *p,HARDWARE_INTERRUPT_SOURCE source) {++enables;return enable_status;}
static EFI_STATUS EFIAPI Disable(EFI_HARDWARE_INTERRUPT_PROTOCOL *p,HARDWARE_INTERRUPT_SOURCE source) {++disables;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Eoi(EFI_HARDWARE_INTERRUPT_PROTOCOL *p,HARDWARE_INTERRUPT_SOURCE source) {++eois;return EFI_SUCCESS;}
static EFI_HARDWARE_INTERRUPT_PROTOCOL irq={.RegisterInterruptSource=Register,.EnableInterruptSource=Enable,.DisableInterruptSource=Disable,.EndOfInterrupt=Eoi};
static EFI_STATUS EFIAPI Locate(EFI_GUID *g,VOID *registration,VOID **out) {*out=&irq;return EFI_SUCCESS;}
static EFI_TPL EFIAPI Raise(EFI_TPL t) {return TPL_APPLICATION;}
static VOID EFIAPI Restore(EFI_TPL t) {}
static EFI_STATUS EFIAPI Signal(EFI_EVENT e) {++signals;return EFI_SUCCESS;}
static EFI_BOOT_SERVICES bs={.LocateProtocol=Locate,.RaiseTPL=Raise,.RestoreTPL=Restore,.SignalEvent=Signal};
EFI_BOOT_SERVICES *gBS=&bs;
int main(void) {
  EFI_EVENT event=(EFI_EVENT)(UINTN)1;
  EFI_SYSTEM_CONTEXT context={0};
  regs[1][0xa0/4]=2;
  CHECK(W202TouchIrqInitialize(event)==EFI_ACCESS_DENIED && !registered);
  regs[1][0xa0/4]=0;
  regs[2][(0xa0+EIC_MODE)/4]=1;regs[2][(0xa0+EIC_BOTH)/4]=1;regs[2][(0xa0+EIC_POLARITY)/4]=1;
  CHECK(W202TouchIrqInitialize(event)==EFI_SUCCESS && registered && handler);
  enable_status=EFI_DEVICE_ERROR;
  CHECK(W202TouchIrqStart()==EFI_DEVICE_ERROR && !mActive);
  CHECK(MmioRead32(EIC_BASE+EIC_MODE)==1 && MmioRead32(EIC_BASE+EIC_BOTH)==1 && MmioRead32(EIC_BASE+EIC_POLARITY)==1);
  enable_status=EFI_SUCCESS;
  CHECK(W202TouchIrqStart()==EFI_SUCCESS && mActive && MmioRead32(EIC_BASE+EIC_IE)==1);
  CHECK(MmioRead32(EIC_BASE+EIC_MODE)==0 && MmioRead32(EIC_BASE+EIC_BOTH)==0 && MmioRead32(EIC_BASE+EIC_POLARITY)==0);
  regs[2][(0xa0+EIC_MASKED)/4]=1;
  handler(EIC_GIC,context);
  CHECK(signals==1 && eois==1 && !W202TouchIrqFaulted() && MmioRead32(EIC_BASE+EIC_MASKED)==0);
  CHECK(mLowInterrupts==1 && mHighInterrupts==0);
  regs[2][(0xa0+EIC_MASKED)/4]=1;regs[2][(0xa0+0x1c)/4]=1;
  handler(EIC_GIC,context);
  CHECK(mLowInterrupts==1 && mHighInterrupts==1 && signals==2 && eois==2);
  handler(EIC_GIC,context);
  CHECK(signals==2 && eois==3 && disables==0 && !W202TouchIrqFaulted());
  CHECK(mEmptyInterrupts==1 && mEmptyStreak==1);
  regs[1][0xa8/4]=2; /* An actual foreign masked pending source. */
  handler(EIC_GIC,context);
  CHECK(signals==3 && eois==4 && disables==1 && W202TouchIrqFaulted());
  CHECK(regs[1][0xa8/4]==2); /* The other owner is never acknowledged here. */
  W202TouchIrqStop();
  CHECK(!mActive && MmioRead32(EIC_BASE+EIC_IE)==0 && MmioRead32(EIC_BASE+EIC_MODE)==1);
  regs[1][0xa8/4]=0;
  CHECK(W202TouchIrqStart()==EFI_SUCCESS && !W202TouchIrqFaulted() && mEmptyStreak==0);
  for(unsigned i=0;i<7;++i) handler(EIC_GIC,context);
  CHECK(!W202TouchIrqFaulted() && signals==3 && mEmptyInterrupts==8);
  handler(EIC_GIC,context);
  CHECK(W202TouchIrqFaulted() && signals==4 && mEmptyInterrupts==9);
  W202TouchIrqExit();
  CHECK(!registered && !mActive && MmioRead32(EIC_BASE+EIC_IE)==0);
  return 0;
}
