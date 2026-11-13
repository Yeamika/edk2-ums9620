/* Actual key state machine: entry holds, bounce, errors and handoff. */
#include "../Ums9620Pkg/W202ds/drivers/BoardServicesDxe/Keys.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
static UINT64 now;
static UINT32 analog=BIT1, gpio=BIT7;
static EFI_STATUS adi_status;
static unsigned signals, reads;
#ifdef W202_TOUCH_IRQ_DIAG
static unsigned probes;
VOID W202TouchProbeKey(VOID) { ++probes; }
#endif
static EFI_TPL EFIAPI Raise(EFI_TPL t) { return TPL_APPLICATION; }
static VOID EFIAPI Restore(EFI_TPL t) {}
static EFI_STATUS EFIAPI Signal(EFI_EVENT e) { ++signals; return EFI_SUCCESS; }
static EFI_BOOT_SERVICES bs={.RaiseTPL=Raise,.RestoreTPL=Restore,.SignalEvent=Signal};
EFI_BOOT_SERVICES *gBS=&bs;
UINT32 EFIAPI MmioRead32(UINTN a) { return gpio; }
EFI_STATUS W202AdiRead(UINT32 a,UINT32 *v) { ++reads; *v=analog; return adi_status; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return now*1000000; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 ticks) { return ticks; }
static void poll(UINT64 t,UINT32 a,UINT32 g) { now=t;analog=a;gpio=g;W202KeysPoll(); }
int main(void) {
  EFI_INPUT_KEY key;
  unsigned before;
  mInstalled=TRUE; W202KeysEnable(TRUE);
  /* Volume-up and power held at entry must not select a boot target. */
  poll(0,BIT4,BIT7);poll(40,BIT4,BIT7);CHECK(!mKeyCount);
  poll(50,BIT1,BIT7);poll(90,BIT1,BIT7);
  poll(100,BIT1|BIT4,BIT7);poll(110,BIT1,BIT7);poll(120,BIT1|BIT4,BIT7);
  poll(149,BIT1|BIT4,BIT7);CHECK(!mKeyCount);
  poll(150,BIT1|BIT4,BIT7);CHECK(mKeyCount==1 && signals==1);
  CHECK(KeyRead(&mKeys,&key)==EFI_SUCCESS && key.ScanCode==SCAN_UP && !key.UnicodeChar);
  poll(200,BIT1|BIT4,BIT7);CHECK(!mKeyCount);
#ifdef W202_TOUCH_IRQ_DIAG
  CHECK(probes==1); /* Entry hold, bounce and continued hold never sample. */
#endif
  poll(210,BIT1|BIT4,0);poll(240,BIT1|BIT4,0);
  CHECK(KeyRead(&mKeys,&key)==EFI_SUCCESS && key.ScanCode==SCAN_DOWN);
  CHECK(KeyRead(&mKeys,&key)==EFI_NOT_READY);
  poll(250,BIT1,BIT7);poll(280,BIT1,BIT7);
  /* Active-low power bounces, then produces a Unicode CR (UEFI Enter). */
  poll(290,0,BIT7);poll(300,BIT1,BIT7);poll(310,0,BIT7);
  poll(339,0,BIT7);CHECK(!mKeyCount);
  poll(340,0,BIT7);CHECK(mKeyCount==1 && signals==3);
  CHECK(KeyRead(&mKeys,&key)==EFI_SUCCESS && key.ScanCode==SCAN_NULL && key.UnicodeChar==L'\r');
  poll(400,0,BIT7);CHECK(KeyRead(&mKeys,&key)==EFI_NOT_READY); /* No hold repeat. */
  poll(410,BIT1,BIT7);poll(440,BIT1,BIT7);
  poll(450,0,BIT7);poll(480,0,BIT7);
  CHECK(KeyRead(&mKeys,&key)==EFI_SUCCESS && key.ScanCode==SCAN_NULL && key.UnicodeChar==L'\r');
  W202KeysEnable(FALSE);before=reads;poll(500,BIT1,BIT7);CHECK(reads==before);
  W202KeysEnable(TRUE);poll(510,0,0);poll(550,0,0);CHECK(!mKeyCount);
  adi_status=EFI_TIMEOUT;poll(560,BIT1,BIT7);CHECK(!mSeeded);
  adi_status=EFI_SUCCESS;poll(600,0,0);poll(640,0,0);CHECK(!mKeyCount);
  W202KeysExit();before=reads;W202KeysEnable(TRUE);poll(700,BIT1,BIT7);
  CHECK(reads==before && KeyRead(&mKeys,&key)==EFI_UNSUPPORTED);
#ifdef W202_TOUCH_IRQ_DIAG
  CHECK(probes==4); /* Two volume and two power presses, none while stopped. */
#endif
  return 0;
}
