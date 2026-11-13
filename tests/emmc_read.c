/* Exercise the real SDMA/BlockIo path against a command-driven SDHCI model. */
#include "../Ums9620Pkg/W202ds/drivers/EmmcDxe/EmmcDxe.c"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}

static UINT8 regs[0x100], dma[READ_BYTES], written[1024];
static UINTN write_bytes, cache_before, cache_after;
VOID *EFIAPI CopyMem(VOID *d, CONST VOID *s, UINTN n) { for (UINTN i=0;i<n;++i) ((UINT8 *)d)[i]=((CONST UINT8 *)s)[i]; return d; }
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *p, UINTN n) { ++cache_before; return p; }
VOID *EFIAPI InvalidateDataCacheRange(VOID *p, UINTN n) { ++cache_after; return p; }
VOID EFIAPI MemoryFence(VOID) {}
static UINT32 irq, argument, words, commands, resets, bad_access, scenario;
static UINT64 delayed;
static BOOLEAN busy, data_busy;
static UINT32 command_log[16], argument_log[16];

static UINT32 word(UINT32 lba, UINT32 n) { return 0x12345678U ^ (lba * 0x10203U + n); }
UINT8 EFIAPI MmioRead8(UINTN a) { return regs[a - EMMC_BASE]; }
UINT16 EFIAPI MmioRead16(UINTN a) {
  UINTN n=a-EMMC_BASE;
  return regs[n] | ((UINT16)regs[n+1]<<8);
}
UINT32 EFIAPI MmioRead32(UINTN a) {
  UINTN n=a-EMMC_BASE;
  if (n==INT_STATUS) return irq;
  if (n==PRESENT_STATE) return busy?COMMAND_BUSY:data_busy?BIT1:0;
  if (n==RESPONSE) return scenario==5 || (scenario==6 && command_log[commands-1]==12)?BIT22:0;
  if (n==BUFFER_DATA) {
    UINT32 v=word(argument,words++);
    if (words>128) ++bad_access;
    if (words==128 && scenario!=4) irq|=DATA_DONE;
    return v;
  }
  return regs[n] | ((UINT32)regs[n+1]<<8) | ((UINT32)regs[n+2]<<16) | ((UINT32)regs[n+3]<<24);
}
UINT8 EFIAPI MmioWrite8(UINTN a,UINT8 v) {
  UINTN n=a-EMMC_BASE;
  if (n==SOFTWARE_RESET) {
    if (v!=6) ++bad_access; /* Never reset the card/controller/clock. */
    ++resets;
    return v;
  }
  if (n!=HOST_CONTROL && n!=DATA_TIMEOUT) ++bad_access;
  regs[n]=v;
  return v;
}
UINT16 EFIAPI MmioWrite16(UINTN a,UINT16 v) {
  UINTN n=a-EMMC_BASE;
  if (n!=BLOCK_SIZE) ++bad_access;
  regs[n]=(UINT8)v;regs[n+1]=(UINT8)(v>>8);
  return v;
}
UINT32 EFIAPI MmioWrite32(UINTN a,UINT32 v) {
  UINTN n=a-EMMC_BASE;
  if (n==INT_STATUS) { irq&=~v;return v; } /* W1C interrupt register. */
  if (n==TRANSFER_COMMAND) {
    UINT32 cmd=v>>24;
    UINT32 blocks=MmioRead32(EMMC_BASE+BLOCK_COUNT);
    if (commands>=16 || (cmd!=8 && cmd!=17 && cmd!=18 && cmd!=24 && cmd!=6 && cmd!=12) ||
        (v&0xffffff)!=(cmd==6 || cmd==12?0x1b0003:cmd==24?0x3a0003:cmd==18?0x3a0033:0x3a0013) ||
        MmioRead16(EMMC_BASE+BLOCK_SIZE)!=0x4200 ||
        (cmd==18 ? blocks<2 || blocks>READ_BYTES/512 : cmd!=12 && blocks!=1) ||
        (regs[HOST_CONTROL]&0x18) || MmioRead32(EMMC_BASE+SIGNAL_ENABLE)!=0) ++bad_access;
    if (cmd==18) {
      if (data_busy || (mDmaAddress & (DMA_BOUNDARY-1)) + blocks*512 >= DMA_BOUNDARY) ++bad_access;
      data_busy=TRUE;
    } else if (cmd==12) {
      if (!data_busy || argument!=0) ++bad_access;
      data_busy=FALSE;
    }
    if (commands<16) { command_log[commands]=cmd;argument_log[commands]=argument; }
    ++commands;words=0;
    if (cmd==24) {
      if (write_bytes+512<=sizeof(written)) CopyMem(written+write_bytes,mDma,512); else ++bad_access;
      write_bytes+=512;
    } else if (cmd!=6 && cmd!=12) {
      if (blocks*512>sizeof(dma)) return ++bad_access;
      for (UINTN i=0;i<blocks*512;++i) mDma[i]=(UINT8)(word(argument+i/512,(i%512)/4)>>(8*(i%4)));
      if (cmd==8) { mDma[32]=0; mDma[33]=mCacheEnabled?1:0; }
    }
    irq=scenario==1?BIT17|BIT15:scenario==2?0:(scenario==3 || scenario==4)?CMD_DONE:CMD_DONE|DATA_DONE;
    if (scenario==7 && cmd==12) irq=CMD_DONE;
    return v;
  }
  if (n!=BLOCK_COUNT && n!=ARGUMENT && n!=INT_ENABLE && n!=SIGNAL_ENABLE && n!=DMA_LOW && n!=DMA_HIGH) ++bad_access;
  if (n==ARGUMENT) argument=v;
  regs[n]=(UINT8)v;regs[n+1]=(UINT8)(v>>8);regs[n+2]=(UINT8)(v>>16);regs[n+3]=(UINT8)(v>>24);
  return v;
}
UINTN EFIAPI MicroSecondDelay(UINTN us) { delayed+=us;return us; }

static void setup(void) {
  UINTN n;
  for (n=0;n<sizeof(regs);++n) regs[n]=(UINT8)(n*3+7);
  commands=resets=bad_access=scenario=words=irq=0;delayed=0;busy=data_busy=FALSE;
  mAvailable=TRUE;mBusy=FALSE;mDma=dma;mDmaAddress=0x83100000;
  mWritable=FALSE;mCacheEnabled=FALSE;mDirty=FALSE;write_bytes=cache_before=cache_after=0;
  mMedia.MediaId=42;mMedia.MediaPresent=TRUE;mMedia.ReadOnly=TRUE;
  mMedia.BlockSize=512;mMedia.LastBlock=0x123456;
}
static int restored(void) {
  UINTN n;
  for (n=0;n<sizeof(regs);++n) {
    if (n>=ARGUMENT && n<ARGUMENT+4) continue;
    if (regs[n]!=(UINT8)(n*3+7)) return 0;
  }
  return !bad_access;
}
static int matches(CONST UINT8 *p,UINT32 lba) {
  UINTN n;
  for (n=0;n<512;++n) if (p[n]!=(UINT8)(word(lba,(UINT32)n/4)>>(8*(n%4)))) return 0;
  return 1;
}

int main(void) {
  UINT8 buffer[1025]; /* Deliberately unaligned: BlockIo advertises IoAlign=1. */
  UINT32 s, before;
  setup();
  CHECK(ReadBlocks(&mBlockIo,42,5,1024,buffer+1)==EFI_SUCCESS);
  CHECK(commands==2 && command_log[0]==18 && command_log[1]==12 &&
        argument_log[0]==5 && argument_log[1]==0 && resets==0 && restored() && !data_busy);
  CHECK(matches(buffer+1,5) && matches(buffer+513,6));
  CHECK(ReadBlocks(&mBlockIo,42,mMedia.LastBlock,512,buffer)==EFI_SUCCESS && restored());
  before=commands;
  CHECK(ReadBlocks(&mBlockIo,42,mMedia.LastBlock,1024,buffer)==EFI_INVALID_PARAMETER);
  CHECK(ReadBlocks(&mBlockIo,42,MAX_UINT64,512,buffer)==EFI_INVALID_PARAMETER);
  CHECK(ReadBlocks(&mBlockIo,42,1,511,buffer)==EFI_BAD_BUFFER_SIZE);
  CHECK(ReadBlocks(&mBlockIo,41,1,512,buffer)==EFI_MEDIA_CHANGED);
  CHECK(ReadBlocks(&mBlockIo,42,1,512,NULL)==EFI_INVALID_PARAMETER);
  CHECK(ReadBlocks(NULL,42,1,512,buffer)==EFI_INVALID_PARAMETER);
  CHECK(ReadBlocks(&mBlockIo,42,MAX_UINT64,0,NULL)==EFI_SUCCESS);
  CHECK(WriteBlocks(&mBlockIo,42,1,512,buffer)==EFI_WRITE_PROTECTED);
  CHECK(Reset(&mBlockIo,TRUE)==EFI_SUCCESS && Flush(&mBlockIo)==EFI_SUCCESS);
  mBusy=TRUE;CHECK(ReadBlocks(&mBlockIo,42,1,512,buffer)==EFI_NOT_READY);mBusy=FALSE;
  CHECK(commands==before && resets==0 && restored());
  CHECK(ReadSector(24,1,buffer)==EFI_INVALID_PARAMETER && commands==before);
  CHECK(ReadSector(8,0,buffer)==EFI_SUCCESS && command_log[commands-1]==8 && restored());

  /* Split a large, unaligned caller buffer at the DMA chunk limit. Verify
   * every byte and the untouched guards, including the final single sector.
   */
  {
    static UINT8 large[2*READ_BYTES+514];
    setup();large[0]=0x5a;large[sizeof(large)-1]=0xa5;
    CHECK(ReadBlocks(&mBlockIo,42,100,sizeof(large)-2,large+1)==EFI_SUCCESS);
    CHECK(commands==5 && command_log[4]==17 && restored() && !data_busy);
    CHECK(large[0]==0x5a && large[sizeof(large)-1]==0xa5);
    for (s=0;s<(sizeof(large)-2)/512;++s) CHECK(matches(large+1+s*512,100+s));
    before=commands;
    CHECK(TransferBlocks(18,1,buffer,1)==EFI_INVALID_PARAMETER);
    CHECK(TransferBlocks(18,1,buffer,READ_BYTES/512+1)==EFI_INVALID_PARAMETER);
    CHECK(TransferBlocks(24,1,buffer,2)==EFI_INVALID_PARAMETER && commands==before);
  }
  /* A failed CMD12 must not publish the otherwise successful read or allow
   * further requests on the controller. This also covers its busy timeout.
   */
  for (s=6;s<=7;++s) {
    setup();scenario=s;
    for (before=0;before<sizeof(buffer);++before) buffer[before]=0x5a;
    CHECK(EFI_ERROR(ReadBlocks(&mBlockIo,42,5,1024,buffer)));
    CHECK(commands==2 && resets==1 && !mAvailable && !mBusy && restored());
    for (before=0;before<sizeof(buffer);++before) CHECK(buffer[before]==0x5a);
  }

  setup();mWritable=TRUE;mMedia.ReadOnly=FALSE;mEspFirst=50;mEspLast=60;mCacheEnabled=TRUE;
  for (s=0;s<1024;++s) buffer[s+1]=(UINT8)(s*19);
  CHECK(WriteBlocks(&mBlockIo,42,49,512,buffer)==EFI_WRITE_PROTECTED);
  CHECK(WriteBlocks(&mBlockIo,42,60,1024,buffer)==EFI_WRITE_PROTECTED);
  CHECK(WriteBlocks(&mBlockIo,42,61,512,buffer)==EFI_WRITE_PROTECTED && commands==0);
  CHECK(WriteBlocks(&mBlockIo,42,50,1024,buffer+1)==EFI_SUCCESS && mDirty);
  CHECK(commands==2 && command_log[0]==24 && argument_log[0]==50 && argument_log[1]==51);
  CHECK(cache_before==2 && cache_after==0 && restored());
  for (s=0;s<1024;++s) CHECK(written[s]==(UINT8)(s*19));
  CHECK(Flush(&mBlockIo)==EFI_SUCCESS && !mDirty && command_log[2]==6 && argument_log[2]==0x03200100);
  CHECK(Flush(&mBlockIo)==EFI_SUCCESS && commands==4 && command_log[3]==8 && restored());
  mDirty=TRUE;scenario=5;CHECK(EFI_ERROR(Flush(&mBlockIo)) && mDirty && !mAvailable && !mBusy);
  setup();mWritable=TRUE;mMedia.ReadOnly=FALSE;mEspFirst=50;mEspLast=60;scenario=4;
  CHECK(EFI_ERROR(WriteBlocks(&mBlockIo,42,50,1024,buffer)) && commands==1 && !mAvailable && !mBusy);
  setup();mDirty=TRUE;CHECK(Flush(&mBlockIo)==EFI_SUCCESS && !mDirty && commands==0);

  for (s=1;s<=5;++s) {
    setup();scenario=s;
    CHECK(EFI_ERROR(ReadBlocks(&mBlockIo,42,1,1024,buffer)));
    CHECK(commands==1 && resets==1 && !mAvailable && !mMedia.MediaPresent && !mBusy && restored());
    CHECK(delayed<=WAIT_US+1000);
    CHECK(ReadBlocks(&mBlockIo,42,1,512,buffer)==EFI_NO_MEDIA && commands==1);
  }
  setup();busy=TRUE;
  CHECK(ReadBlocks(&mBlockIo,42,1,512,buffer)==EFI_TIMEOUT);
  CHECK(commands==0 && resets==0 && delayed==WAIT_US && restored());
  setup();ExitBootServices(NULL,NULL);before=commands;
  CHECK(ReadBlocks(&mBlockIo,42,1,512,buffer)==EFI_NO_MEDIA);
  CHECK(Reset(&mBlockIo,FALSE)==EFI_NO_MEDIA && commands==before && restored());
  return 0;
}
