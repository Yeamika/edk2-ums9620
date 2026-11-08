/* Host regression using the real DXE state machine; only hardware/BS mocked. */
#include "../Ums9620Pkg/Drivers/BoardServicesDxe/BoardServicesDxe.c"
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static unsigned reads, stops, starts, signals, irq_starts, irq_stops, timers;
static EFI_STATUS read_status, start_status, timer_status;
static EFI_STATUS charge_stop_status;
static unsigned power_reads, charge_updates, charge_stops;
static EFI_ABSOLUTE_POINTER_STATE next_state;
static EFI_TPL EFIAPI Raise(EFI_TPL t) { return TPL_APPLICATION; }
static VOID EFIAPI Restore(EFI_TPL t) {}
static EFI_STATUS EFIAPI Timer(EFI_EVENT e,EFI_TIMER_DELAY t,UINT64 n) { ++timers;return timer_status; }
static EFI_STATUS EFIAPI Signal(EFI_EVENT e) { ++signals; return EFI_SUCCESS; }
static EFI_BOOT_SERVICES bs={.RaiseTPL=Raise,.RestoreTPL=Restore,.SetTimer=Timer,.SignalEvent=Signal};
EFI_BOOT_SERVICES *gBS=&bs;
VOID *EFIAPI CopyMem(VOID *d,CONST VOID *s,UINTN n) { UINT8 *a=d;const UINT8 *b=s;while(n--)*a++=*b++;return d; }
VOID *EFIAPI ZeroMem(VOID *d,UINTN n) { UINT8 *a=d;while(n--)*a++=0;return d; }
INTN EFIAPI CompareMem(CONST VOID *a,CONST VOID *b,UINTN n) { const UINT8 *x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;++x;++y;}return 0; }
EFI_STATUS W202TouchStart(VOID) { ++starts;return start_status; }
EFI_STATUS W202KeysInit(VOID) { return EFI_SUCCESS; }
VOID W202KeysPoll(VOID) {}
VOID W202KeysEnable(BOOLEAN Enable) {}
VOID W202KeysExit(VOID) {}
VOID W202TouchStop(VOID) { ++stops; }
EFI_STATUS W202TouchIrqInitialize(EFI_EVENT Event) { return EFI_SUCCESS; }
EFI_STATUS W202TouchIrqStart(VOID) { ++irq_starts;return EFI_SUCCESS; }
VOID W202TouchIrqStop(VOID) { ++irq_stops; }
VOID W202TouchIrqExit(VOID) { ++irq_stops; }
BOOLEAN W202TouchIrqFaulted(VOID) { return FALSE; }
VOID W202TouchIrqTrace(VOID) {}
VOID W202TouchStartupTrace(VOID) {}
VOID W202ChargeTrace(VOID) {}
EFI_STATUS W202TouchRead(EFI_ABSOLUTE_POINTER_STATE *s) { ++reads;*s=next_state;return read_status; }
EFI_STATUS W202ReadPower(CONST W202_LOADER_HANDOFF *h,PJB_POWER_SAMPLE *s) { ++power_reads;s->valid=PJB_POWER_VOLTAGE;s->voltage_mv=4050;return EFI_SUCCESS; }
EFI_STATUS W202NtcVoltage(UINT32 *v) { *v=700;return EFI_SUCCESS; }
EFI_STATUS W202ChargeUpdate(CONST PJB_POWER_SAMPLE *s,UINT32 v) { ++charge_updates;return EFI_SUCCESS; }
EFI_STATUS W202ChargeStop(VOID) { ++charge_stops;return charge_stop_status; }
EFI_STATUS W202ChargeReadEnabled(UINT32 *v) { *v=0; return EFI_SUCCESS; }
EFI_STATUS W202ChargeRequest(CONST PJB_POWER_SAMPLE *s,UINT32 v,UINT32 e) { return EFI_SUCCESS; }
EFI_STATUS W202ChargeReadRequested(UINT32 *v) { *v=0; return EFI_SUCCESS; }
VOID W202PowerReset(VOID) {}
EFI_STATUS W202WatchdogRenew(VOID) { return EFI_SUCCESS; }
int main(void) {
  EFI_ABSOLUTE_POINTER_STATE state;
  PJB_POWER_SAMPLE power={.size=sizeof(power)};
  UINTN i;
  mTouchAvailable=TRUE;mQuiesced=FALSE;
  CHECK(ReadPower(&mPlatform,&power)==EFI_NOT_READY && power_reads==0);
  PowerPoll(NULL,NULL);CHECK(power_reads==1 && charge_updates==1);
  CHECK(ReadPower(&mPlatform,&power)==EFI_SUCCESS && power.voltage_mv==4050 && power_reads==1);
  next_state.CurrentX=100;next_state.CurrentY=200;next_state.ActiveButtons=1;
  Poll(NULL,NULL);CHECK(reads==0 && mCount==0);
  ScheduleTouch(NULL,NULL);ScheduleTouch(NULL,NULL);
  CHECK(reads==0 && timers==0 && signals==1 && mTouchScheduled);
  TouchEvent(NULL,NULL);CHECK(mCount==1 && signals==2);
  CHECK(!mTouchScheduled);
  TouchEvent(NULL,NULL);CHECK(mCount==1);
  CHECK(GetState(&mPointer,&state)==EFI_SUCCESS && state.CurrentX==100 && state.ActiveButtons==1);
  CHECK(GetState(&mPointer,&state)==EFI_NOT_READY);
  read_status=EFI_NOT_READY;TouchEvent(NULL,NULL);
  CHECK(mHaveState && mLast.ActiveButtons==1 && !mCount && mTouchError==EFI_SUCCESS);
  read_status=EFI_SUCCESS;
  next_state.CurrentX=0;next_state.CurrentY=0;next_state.ActiveButtons=0;
  TouchEvent(NULL,NULL);CHECK(GetState(&mPointer,&state)==EFI_SUCCESS && state.CurrentX==100 && !state.ActiveButtons);
  read_status=EFI_COMPROMISED_DATA;TouchEvent(NULL,NULL);
  CHECK(GetState(&mPointer,&state)==EFI_DEVICE_ERROR && !mCount);
  read_status=EFI_SUCCESS;next_state.ActiveButtons=1;
  for(i=0;i<33;++i){next_state.CurrentX=i;TouchEvent(NULL,NULL);}
  CHECK(GetState(&mPointer,&state)==EFI_DEVICE_ERROR && !mCount);
  timer_status=EFI_DEVICE_ERROR;
  CHECK(SetQuiesced(&mPlatform,1)==EFI_DEVICE_ERROR && !mQuiesced);
  timer_status=EFI_SUCCESS;
  charge_stop_status=EFI_DEVICE_ERROR;
  CHECK(SetQuiesced(&mPlatform,1)==EFI_DEVICE_ERROR && !mQuiesced && stops==0);
  charge_stop_status=EFI_SUCCESS;
  CHECK(SetQuiesced(&mPlatform,1)==EFI_SUCCESS && mQuiesced && stops==1 && irq_stops==1);
  i=reads;Poll(NULL,NULL);PowerPoll(NULL,NULL);TouchEvent(NULL,NULL);CHECK(ReadPower(&mPlatform,&power)==EFI_NOT_READY && reads==i && power_reads==1);
  CHECK(SetQuiesced(&mPlatform,1)==EFI_SUCCESS && stops==1);
  start_status=EFI_NOT_READY;CHECK(SetQuiesced(&mPlatform,0)==EFI_NOT_READY && mQuiesced);
  start_status=EFI_SUCCESS;CHECK(SetQuiesced(&mPlatform,0)==EFI_SUCCESS && !mQuiesced && irq_starts==1);
  ExitBootServices(NULL,NULL);i=reads;Poll(NULL,NULL);PowerPoll(NULL,NULL);TouchEvent(NULL,NULL);
  CHECK(mExited && mQuiesced && reads==i);
  CHECK(power_reads==1 && charge_updates==1 && charge_stops>=3);
  CHECK(SetQuiesced(&mPlatform,0)==EFI_UNSUPPORTED);
  CHECK(GetState(&mPointer,&state)==EFI_UNSUPPORTED);
  CHECK(ReadPower(&mPlatform,&power)==EFI_NOT_READY && reads==i && power.valid==0);
  return 0;
}
