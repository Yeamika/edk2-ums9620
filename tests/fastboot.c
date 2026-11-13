/* Exercise the production optional DXE protocol and its handoff validation. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include "../Ums9620Pkg/W202ds/drivers/LkFastbootDxe/LkFastbootDxe.c"
#define CHECK(x) do { if(!(x)) return __LINE__; } while(0)
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
EFI_GUID gEfiEventExitBootServicesGuid={1};
static W202_LK_RETURN Return;
static W202_LK_FASTBOOT_DESCRIPTOR Descriptor;
static UINT8 *Gateway;
static unsigned raised,restored,installed,closed;
static EFI_STATUS install_status=EFI_SUCCESS;
static EFI_EVENT_NOTIFY exit_callback;
VOID *EFIAPI CopyMem(VOID *To,CONST VOID *From,UINTN Size) {
  CONST UINT8 *p=From;UINT8 *out=To;
  if ((UINTN)From==W202_LK_RETURN_ADDRESS) p=(VOID *)&Return;
  if ((UINTN)From==W202_LK_FB_ADDRESS) p=(VOID *)&Descriptor;
  for(UINTN i=0;i<Size;++i)out[i]=p[i];
  return To;
}
static EFI_TPL EFIAPI raise_tpl(EFI_TPL Tpl) { ++raised;return TPL_APPLICATION; }
static VOID EFIAPI restore_tpl(EFI_TPL Tpl) { ++restored; }
static EFI_STATUS EFIAPI create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Callback,
    CONST VOID *Context,CONST EFI_GUID *Guid,EFI_EVENT *Out) {
  *Out=(VOID *)1;exit_callback=Callback;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI close_event(EFI_EVENT Event) { ++closed;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI install(EFI_HANDLE *Handle,...) { ++installed;return install_status; }
static void result(UINT32 Value) {
  /* x86-64 mock at a valid identity-mapped ARM handoff entry: mov eax,N;ret. */
  Gateway[0]=0xb8;
  for(unsigned i=0;i<4;++i) Gateway[i+1]=Value>>(8*i);
  Gateway[5]=0xc3;
}
int main(void) {
  UINT32 Flags;
  Gateway=mmap((VOID *)0x86001000,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
               MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
  CHECK(Gateway!=(VOID *)-1);
  Services.RaiseTPL=raise_tpl;Services.RestoreTPL=restore_tpl;
  Services.CreateEventEx=create;Services.CloseEvent=close_event;
  Services.InstallMultipleProtocolInterfaces=install;
  CHECK(Initialize((VOID *)2,NULL)==EFI_UNSUPPORTED && !installed);
  Return=(W202_LK_RETURN){W202_LK_RETURN_MAGIC,1,sizeof(Return),0,
     0x86000020,W202_LK_CONTEXT_ADDRESS,512,3};
  Return.checksum=0-W202LkReturnSum(&Return);
  Descriptor=(W202_LK_FASTBOOT_DESCRIPTOR){W202_LK_FB_MAGIC,1,sizeof(Descriptor),0,0x86001000,3,0};
  Descriptor.checksum=0-W202LkFastbootSum(&Descriptor);
  Descriptor.flags^=1;
  CHECK(Initialize((VOID *)2,NULL)==EFI_UNSUPPORTED && !installed);
  Descriptor.flags^=1;install_status=EFI_DEVICE_ERROR;
  CHECK(Initialize((VOID *)2,NULL)==EFI_DEVICE_ERROR && closed==1);
  install_status=EFI_SUCCESS;CHECK(Initialize((VOID *)2,NULL)==EFI_SUCCESS);
  CHECK(Poll(&mProtocol,&Flags)==EFI_NOT_READY && Flags==0);
  CHECK(Start(NULL)==EFI_INVALID_PARAMETER);
  result(0);CHECK(Start(&mProtocol)==EFI_SUCCESS && mActive);
  CHECK(Start(&mProtocol)==EFI_SUCCESS);
  result(1);CHECK(Poll(&mProtocol,&Flags)==EFI_SUCCESS && Flags==1);
  result(16);CHECK(Poll(&mProtocol,&Flags)==EFI_PROTOCOL_ERROR);
  result(0);CHECK(Stop(&mProtocol)==EFI_SUCCESS && !mActive);
  CHECK(Start(&mProtocol)==EFI_SUCCESS && mActive);
  exit_callback(mExit,NULL);
  CHECK(mExited && !mActive);
  CHECK(Start(&mProtocol)==EFI_UNSUPPORTED);
  CHECK(raised==restored);
  munmap(Gateway,4096);
  return 0;
}
