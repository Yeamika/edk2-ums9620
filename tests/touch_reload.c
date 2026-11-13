/* Execute the real bounded loader against the pinned image and a fault model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Ums9620Pkg/W202ds/drivers/BoardServicesDxe/TouchReload.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static UINT8 *image, config[W202_TOUCH_CONFIG_SIZE], entity[108544], device_config[W202_TOUCH_CONFIG_SIZE];
static unsigned writes, memory_writes, activated, delays, fault, forbidden;
static UINT32 crc_address, run_state, reload, active_word, enabled_crc;
static EFI_STATUS rd(UINT32 a,UINT32 *v) {
  *v=0;
  switch(a) {
  case 0x900000d0: *v=fault==1?0:0x83102900; break;
  case 0x900000a8: *v=run_state; break;
  case 0x80010000: *v=enabled_crc; break;
  case 0x80050000: *v=fault==4?1:0; break;
  case 0x80050018:
    *v=crc_address==0x20000000?Crc(entity,sizeof(entity)):Crc(device_config,sizeof(device_config));
    if(fault==3) *v^=1;
    break;
  case 0x90000048: *v=active_word; break;
  case 0x100072c0: *v=reload; break;
  default: ++forbidden; return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS wr(UINT32 a,UINT32 v) {
  ++writes;
  switch(a) {
  case 0x90000018: run_state=1; break;
  case 0x9000005c: break;
  case 0x80010000: enabled_crc=v; break;
  case 0x80050020: crc_address=v; break;
  case 0x80050028: break;
  case 0x10007f00: case 0x100072ec: case 0x10007f04: case 0x10007294: break;
  case 0x100072c0: reload=v; break;
  case 0x90000048:
    ++activated; active_word=fault==5?0:0x1ec;
    if(fault!=5) {reload=fault==6?0:0x72c0;run_state=5;}
    break;
  default: ++forbidden; return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS memory(UINT32 a,CONST UINT8 *p,UINTN n) {
  ++memory_writes;
  if(fault==7) return EFI_TIMEOUT;
  if(a==0x20000000 && n==sizeof(entity)) memcpy(entity,p,n);
  else if(a==0x10002ca4 && n==sizeof(device_config)) memcpy(device_config,p,n);
  else {++forbidden;return EFI_DEVICE_ERROR;}
  return EFI_SUCCESS;
}
static EFI_STATUS sense(BOOLEAN safe) {
  ++writes;
  if(safe && fault!=2) run_state=0x0c;
  return EFI_SUCCESS;
}
static VOID delay(UINTN us) {delays+=(unsigned)us;}
static W202_TOUCH_RELOAD_IO io={rd,wr,memory,sense,delay};
static W202_TOUCH_RELOAD_RESULT result;
static VOID reset(unsigned which) {
  writes=memory_writes=activated=delays=forbidden=0;
  fault=which;run_state=1;reload=active_word=enabled_crc=0;
  memset(entity,0,sizeof(entity));memset(device_config,0,sizeof(device_config));
}
int main(int argc,char **argv) {
  FILE *f;
  unsigned i;
  CHECK(argc==2);
  image=malloc(261120);CHECK(image);
  f=fopen(argv[1],"rb");CHECK(f);
  CHECK(fread(image,1,261120,f)==261120);fclose(f);
  reset(0);CHECK(W202TouchReload(image,261119,config,&io,&result)==EFI_COMPROMISED_DATA && !writes);
  image[0x66]^=1;reset(0);
  CHECK(EFI_ERROR(W202TouchReload(image,261120,config,&io,&result)) && !writes);image[0x66]^=1;
  image[0x500]^=1;reset(0);
  CHECK(W202TouchReload(image,261120,config,&io,&result)==EFI_CRC_ERROR && !writes);image[0x500]^=1;
  image[0x29410]^=1;reset(0);
  CHECK(W202TouchReload(image,261120,config,&io,&result)==EFI_CRC_ERROR && !writes);image[0x29410]^=1;
  reset(0);CHECK(W202TouchReload(image,261120,config,&io,&result)==EFI_SUCCESS);
  CHECK(result.Stage==7 && !result.EntityCrc && result.ConfigCrc==0x5ced9ac0);
  CHECK(memory_writes==2 && activated==1 && !forbidden);
  CHECK(!memcmp(entity,image+0x400,sizeof(entity)));
  for(i=1;i<=7;++i) {
    reset(i);CHECK(EFI_ERROR(W202TouchReload(image,261120,config,&io,&result)));
    CHECK(!forbidden && delays<=600000);
    if(i<=4 || i==7) CHECK(!activated);
    if(i==1) CHECK(!writes);
    if(i==3 || i==4 || i==7) CHECK(memory_writes==1);
    if(i==5) CHECK(activated==8);
  }
  free(image);puts("touch reload: real firmware CRC, exact memory data, bounded fault paths passed");
  return 0;
}
