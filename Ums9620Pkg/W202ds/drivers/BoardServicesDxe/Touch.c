// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Hardware.h"
#include <Library/DebugLib.h>
#ifdef W202_TOUCH_RELOAD
#include <Pi/PiFirmwareFile.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include "TouchReload.h"
STATIC W202_TOUCH_RELOAD_RESULT mReloadResult;
STATIC EFI_STATUS mReloadStatus=EFI_NOT_READY;
STATIC BOOLEAN mReloadAttempted;
#endif

STATIC UINT32 mStartupWords[6];
STATIC EFI_STATUS mStartupStatus=EFI_NOT_READY;
VOID W202TouchStartupTrace(VOID) {
  DEBUG((DEBUG_WARN,"HX_START status=%r id=%08x state=%08x version=%08x reload=%08x contacts=%08x stylus=%08x\n",mStartupStatus,mStartupWords[0],mStartupWords[1],mStartupWords[2],mStartupWords[3],mStartupWords[4],mStartupWords[5]));
#ifdef W202_TOUCH_RELOAD
  DEBUG((DEBUG_WARN,"HX_UPLOAD status=%r stage=%u entity_crc=%08x config_crc=%08x state=%08x reload=%08x\n",mReloadStatus,mReloadResult.Stage,mReloadResult.EntityCrc,mReloadResult.ConfigCrc,mReloadResult.State,mReloadResult.Reload));
#endif
}
STATIC VOID RecordStartup(EFI_STATUS Status, UINT32 Id, UINT32 State,
                          UINT32 Version, UINT32 Reload, UINT32 Contacts, UINT32 Stylus) {
  mStartupStatus=Status; mStartupWords[0]=Id; mStartupWords[1]=State;
  mStartupWords[2]=Version; mStartupWords[3]=Reload;
  mStartupWords[4]=Contacts; mStartupWords[5]=Stylus;
  W202TouchStartupTrace();
}
/* Native SPI1 replacement for the previously verified LK probe. No fixed LK
 * function calls, panel reset, GPIO reset or DMA. Firmware reload is optional. */
#define SPI_BASE 0x20270000
STATIC CONST UINT32 mOffsets[]={4,8,12,16,20,24,28,32,64,76,84,88,92,96};
STATIC UINT32 mSaved[ARRAY_SIZE(mOffsets)], mGate, mClock;
STATIC BOOLEAN mOwned;
STATIC UINT32 mSuspendSaved;
#ifdef W202_TOUCH_IRQ_DIAG
STATIC BOOLEAN mCodeChecked;
STATIC UINT32 mCodeWords[7], mCodeMatches;
STATIC EFI_STATUS mCodeStatus;
#endif
STATIC UINT32 Read(UINTN Off) { return MmioRead32(SPI_BASE+Off); }
STATIC VOID Write(UINTN Off, UINT32 Value) { MmioWrite32(SPI_BASE+Off,Value); }
STATIC VOID ChipSelect(BOOLEAN Active) {
  Write(8, Active ? Read(8)&~BIT8 : Read(8)|BIT8);
}
STATIC EFI_STATUS Wait(UINTN Off, UINT32 Mask, UINT32 Value, UINTN *Budget) {
  while (*Budget) {
    if ((Read(Off)&Mask)==Value) return EFI_SUCCESS;
    --*Budget; MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Transaction(CONST UINT8 *Tx, UINT8 *Rx, UINTN N) {
  UINTN I, Chunk, Budget=4000;
  EFI_STATUS Status=EFI_SUCCESS;
  if (!N || N>132) return EFI_INVALID_PARAMETER;
  Write(0x48,1); Write(0x48,0); ChipSelect(TRUE);
  while (N) {
    Chunk=N>32 ? 32 : N;
    Write(0x24,0xffffffff); Write(0x54,0); Write(0x58,Chunk);
    Write(0x5c,0); Write(0x60,Chunk);
    for (I=0;I<Chunk;++I) {
      Status=Wait(0x34,BIT6,0,&Budget); if (EFI_ERROR(Status)) goto Done;
      Write(0,Tx[I]);
    }
    Status=Wait(0x34,BIT7|BIT8,BIT7,&Budget); if (EFI_ERROR(Status)) goto Done;
    Status=Wait(0x28,BIT9,BIT9,&Budget); if (EFI_ERROR(Status)) goto Done;
    for (I=0;I<Chunk;++I) {
      Status=Wait(0x34,BIT5,0,&Budget); if (EFI_ERROR(Status)) goto Done;
      Rx[I]=(UINT8)Read(0);
    }
    Tx+=Chunk; Rx+=Chunk; N-=Chunk;
  }
Done:
  ChipSelect(FALSE); Write(0x24,0xffffffff);
  if (EFI_ERROR(Status)) { Write(0x48,1); Write(0x48,0); }
  return Status;
}
STATIC EFI_STATUS BusWrite(UINT8 Command, CONST VOID *Data, UINTN N) {
  UINT8 Tx[132]={0}, Rx[132];
  if (N>130) return EFI_INVALID_PARAMETER;
  Tx[0]=0xf2; Tx[1]=Command; CopyMem(Tx+2,Data,N);
  return Transaction(Tx,Rx,N+2);
}
STATIC EFI_STATUS BusRead(UINT8 Command, VOID *Data, UINTN N) {
  UINT8 Tx[132]={0}, Rx[132];
  EFI_STATUS Status;
  if (N>129) return EFI_INVALID_PARAMETER;
  Tx[0]=0xf3; Tx[1]=Command;
  Status=Transaction(Tx,Rx,N+3);
  if (!EFI_ERROR(Status)) CopyMem(Data,Rx+3,N);
  return Status;
}
STATIC EFI_STATUS RegisterRead(UINT32 Address, UINT32 *Value) {
  UINT8 Continuous=0x31, Increment=0x10, Direction=0;
  EFI_STATUS S;
  S=BusWrite(0x13,&Continuous,1); if (EFI_ERROR(S)) return S;
  S=BusWrite(0x0d,&Increment,1); if (EFI_ERROR(S)) return S;
  S=BusWrite(0,&Address,4); if (EFI_ERROR(S)) return S;
  S=BusWrite(0x0c,&Direction,1); if (EFI_ERROR(S)) return S;
  return BusRead(8,Value,4);
}
STATIC EFI_STATUS RegisterWrite(UINT32 Address, UINT32 Value) {
  UINT8 Continuous=0x31, Increment=0x10;
  UINT32 Payload[2]={Address,Value};
  EFI_STATUS S;
  S=BusWrite(0x13,&Continuous,1); if (EFI_ERROR(S)) return S;
  S=BusWrite(0x0d,&Increment,1); if (EFI_ERROR(S)) return S;
  return BusWrite(0,Payload,sizeof(Payload));
}
#ifdef W202_TOUCH_RELOAD
STATIC EFI_STATUS WriteFirmwareMemory(UINT32 Address, CONST UINT8 *Data, UINTN Size) {
  UINT8 Continuous=0x31, Increment=0x11, Payload[128];
  UINTN Chunk;
  EFI_STATUS Status;
  Status=BusWrite(0x13,&Continuous,1); if (EFI_ERROR(Status)) return Status;
  Status=BusWrite(0x0d,&Increment,1); if (EFI_ERROR(Status)) return Status;
  while (Size) {
    Chunk=MIN(Size,sizeof(Payload)-4);
    CopyMem(Payload,&Address,4); CopyMem(Payload+4,Data,Chunk);
    Status=BusWrite(0,Payload,Chunk+4); if (EFI_ERROR(Status)) return Status;
    Address+=(UINT32)Chunk; Data+=Chunk; Size-=Chunk;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS SetSense(BOOLEAN Safe) {
  UINT8 Data[2]={Safe?0x27:0,Safe?0x95:0};
  return BusWrite(0x31,Data,sizeof(Data));
}
STATIC VOID ReloadDelay(UINTN Us) { MicroSecondDelay(Us); }
STATIC EFI_STATUS ReloadFromFirmwareVolume(VOID) {
  STATIC CONST EFI_GUID FirmwareGuid={0x24bf1f0e,0xe35a,0x4814,{0xa3,0xdf,0xf8,0x31,0x02,0x0a,0x20,0x26}};
  STATIC CONST W202_TOUCH_RELOAD_IO Io={RegisterRead,RegisterWrite,WriteFirmwareMemory,SetSense,ReloadDelay};
  VOID *Firmware=NULL;
  UINT8 *Config;
  UINTN Size=0;
  EFI_STATUS Status;
  Status=GetSectionFromAnyFv(&FirmwareGuid,EFI_SECTION_RAW,0,&Firmware,&Size);
  if (EFI_ERROR(Status)) return Status;
  Config=AllocateZeroPool(W202_TOUCH_CONFIG_SIZE);
  if (!Config) { FreePool(Firmware); return EFI_OUT_OF_RESOURCES; }
  Status=W202TouchReload(Firmware,Size,Config,&Io,&mReloadResult);
  FreePool(Config); FreePool(Firmware);
  return Status;
}
#endif
#ifdef W202_TOUCH_IRQ_DIAG
STATIC VOID TraceRetainedImage(VOID) {
  /* Parsed descriptor TP_CONFIG_TABLE=0x28400, entity 0x400 -> 0x20000000.
   * guoxian firmware SHA256 bdcd327d48628dd703b1d0a8ea778ef7f7aacd7beb227efdb78ea788928f0156.
   * Immutable code samples only: matching does not establish a full-image CRC. */
  STATIC CONST UINT32 Addresses[]={0x20000000,0x20000004,0x20000100,
    0x20001000,0x20004000,0x20008000,0x2000c000};
  STATIC CONST UINT32 Expected[]={0x16600048,0x30000048,0x3c880f3a,
    0x7252e800,0x0f2e81ad,0x30809450,0xeac807f0};
  UINTN I;
  if (!mCodeChecked) {
    mCodeChecked=TRUE;mCodeMatches=0;mCodeStatus=EFI_SUCCESS;
    for (I=0;I<ARRAY_SIZE(Addresses);++I) {
      mCodeWords[I]=MAX_UINT32;
      mCodeStatus=RegisterRead(Addresses[I],&mCodeWords[I]);
      if (EFI_ERROR(mCodeStatus)) break;
      if (mCodeWords[I]==Expected[I]) mCodeMatches|=(1U<<I);
    }
  }
  DEBUG((DEBUG_WARN,"HX_CODE match=%02x words=%08x,%08x,%08x,%08x,%08x,%08x,%08x status=%r display_ctrl=%08x display_cfg=%08x\n",
    mCodeMatches,mCodeWords[0],mCodeWords[1],mCodeWords[2],mCodeWords[3],
    mCodeWords[4],mCodeWords[5],mCodeWords[6],mCodeStatus,
    MmioRead32(0x31000008),MmioRead32(0x3100000c)));
}
#endif
/* Read only, on an empty IRQ report. HX83102J overrides the generic rawout
 * address with 0x100072ec. Keep the firmware's IRQ/idle word and mailboxes
 * intact while distinguishing its report mode from the EIC configuration. */
STATIC VOID TraceReportMode(VOID) {
  STATIC CONST UINT32 Addresses[]={0x10007088,0x100072ec,0x10007f04,
                                  0x10007f10,0x10007ffc,0x10007fd0,0x10007294,
                                  0x10000000};
  UINT32 Values[ARRAY_SIZE(Addresses)];
  UINTN I;
  EFI_STATUS Status=EFI_SUCCESS;
  for (I=0;I<ARRAY_SIZE(Addresses);++I) {
    EFI_STATUS ReadStatus;
    Values[I]=MAX_UINT32;
    ReadStatus=RegisterRead(Addresses[I],&Values[I]);
    if (EFI_ERROR(ReadStatus)) Status=ReadStatus;
  }
  DEBUG((DEBUG_WARN,"HX_MODE irq_idle=%08x rawout=%08x sorting=%08x smwp=%08x overlay=%08x ap=%08x nframe=%08x dsram=%08x status=%r\n",
    Values[0],Values[1],Values[2],Values[3],Values[4],Values[5],Values[6],Values[7],Status));
#ifdef W202_TOUCH_IRQ_DIAG
  TraceRetainedImage();
#endif
}
/* Normal-report portion of the vendor power_on_init, without its shared-IC
 * reset, firmware upload or reload-status clearing. A retained version/layout
 * does not establish the report settings. Sorting 0x9999 is the firmware's
 * raw/noise-mode completion value, not an error code. */
STATIC EFI_STATUS ConfigureNormalReports(VOID) {
  STATIC CONST UINT32 Addresses[]={0x100072ec,0x10007f04,0x10007294};
  STATIC CONST UINT32 Wanted[]={0,0,1};
  UINTN I, Retry;
  UINT32 IrqMode, Before[ARRAY_SIZE(Addresses)], Got;
  EFI_STATUS Status;
  Status=RegisterRead(0x10007088,&IrqMode);
  if (EFI_ERROR(Status)) return Status;
  /* This board path owns the firmware-declared falling-edge IRQ only. */
  if (!(IrqMode&BIT8)) return EFI_UNSUPPORTED;
  for (I=0;I<ARRAY_SIZE(Addresses);++I) {
    Status=RegisterRead(Addresses[I],&Before[I]);
    if (EFI_ERROR(Status)) return Status;
  }
  DEBUG((DEBUG_WARN,"HX_REPORT inherited irq=%08x rawout=%08x sorting=%08x nframe=%08x\n",
    IrqMode,Before[0],Before[1],Before[2]));
  for (I=0;I<ARRAY_SIZE(Addresses);++I) {
    for (Retry=0;Retry<3;++Retry) {
      Status=RegisterWrite(Addresses[I],Wanted[I]);
      if (EFI_ERROR(Status)) return Status;
      MicroSecondDelay(1000);
      Status=RegisterRead(Addresses[I],&Got);
      if (EFI_ERROR(Status)) return Status;
      if (Got==Wanted[I] || (I==1 && Got==0x9999)) break;
    }
    if (Retry==3) return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
/* HX83102J vendor sense_on(FlashMode=1) and reload_to_active. The SRAM
 * image must already pass the ID/reload/layout gates. No panel/GPIO reset. */
STATIC EFI_STATUS ResumeRetained(VOID) {
  UINT8 Sense[2]={0,0};
  UINT32 Got;
  UINTN I;
  EFI_STATUS S;
  S=RegisterWrite(0x9000005c,0); if (EFI_ERROR(S)) return S;
  MicroSecondDelay(10000);
  S=BusWrite(0x31,Sense,2); if (EFI_ERROR(S)) return S;
  S=RegisterWrite(0x80010000,0xecce); if (EFI_ERROR(S)) return S;
  S=RegisterRead(0x80010000,&Got); if (EFI_ERROR(S)) return S;
  DEBUG((DEBUG_WARN,"HX_CRC readback=%08x\n",Got));
  if ((Got&0xffff)!=0xecce) return EFI_DEVICE_ERROR;
  for (I=0;I<8;++I) {
    S=RegisterWrite(0x90000048,0xec); if (EFI_ERROR(S)) return S;
    MicroSecondDelay(2000);
    S=RegisterRead(0x90000048,&Got);
    DEBUG((DEBUG_WARN,"HX_ACTIVATE readback=%08x status=%r\n",Got,S));
    if (!EFI_ERROR(S) && (Got&0xffff)==0x1ec) return EFI_SUCCESS;
  }
  return EFI_NOT_READY;
}
/* Match resume_proc's explicit AP-awake notification even when its inherited
 * mailbox already reads zero. A stored value is not proof of a new command.
 * Only use this after the retained image and non-suspended owner checks. */
STATIC EFI_STATUS NotifyHostState(BOOLEAN Asleep) {
  UINTN I;
  UINT32 Wanted=Asleep ? 0xa55aa55a : 0;
  UINT32 Got=MAX_UINT32;
  EFI_STATUS Status=EFI_NOT_READY;
  for (I=0;I<8;++I) {
    Status=RegisterWrite(0x10007fd0,Wanted); if (EFI_ERROR(Status)) return Status;
    MicroSecondDelay(1000);
    Status=RegisterRead(0x10007fd0,&Got);
    if (!EFI_ERROR(Status) && Got==Wanted) return EFI_SUCCESS;
  }
  return EFI_DEVICE_ERROR;
}
#ifdef W202_TOUCH_RESUME_TEST
/* Diagnostic retained-image handshake, using the same AP mailbox as the
 * Linux shared-panel coordination. Always attempt awake even if sleep fails.
 * No pin/reset/TCON command or firmware-memory write belongs to this cycle. */
STATIC EFI_STATUS CycleHostState(UINT32 ExpectedVersion) {
  EFI_STATUS Sleep, Awake, Status;
  UINT32 Version=0, Reload=0;
  Sleep=NotifyHostState(TRUE);
  if (!EFI_ERROR(Sleep)) MicroSecondDelay(20000);
  Awake=NotifyHostState(FALSE);
  DEBUG((DEBUG_WARN,"HX_HOST_CYCLE sleep=%r awake=%r\n",Sleep,Awake));
  if (EFI_ERROR(Awake)) return Awake;
  if (EFI_ERROR(Sleep)) return Sleep;
  MicroSecondDelay(20000);
  Status=RegisterRead(0x10007004,&Version);
  if (EFI_ERROR(Status)) return Status;
  Status=RegisterRead(0x100072c0,&Reload);
  if (EFI_ERROR(Status)) return Status;
  if (Version!=ExpectedVersion || Reload!=0x72c0) return EFI_NOT_READY;
  return EFI_SUCCESS;
}
#endif
VOID W202TouchStop(VOID) {
  UINTN I;
  if (!mOwned) return;
  ChipSelect(FALSE); Write(0x48,1); Write(0x48,0);
  for (I=0;I<ARRAY_SIZE(mOffsets);++I) Write(mOffsets[I],mSaved[I]);
  MmioWrite32(0x649202c8,mClock);
  /* Restore only our gate bit, preserving unrelated peripherals. */
  MmioAndThenOr32(0x20100000,~(UINT32)BIT6,mGate & BIT6);
  mOwned=FALSE;
}
EFI_STATUS W202TouchStart(VOID) {
  UINTN I;
  UINT32 Id=0, State=0, Version=0, Reload=0, Contacts=0, Stylus=0;
  EFI_STATUS Status;
  if (mOwned) return EFI_SUCCESS;
#ifdef W202_TOUCH_IRQ_DIAG
  mCodeChecked=FALSE;
#endif
  mGate=MmioRead32(0x20100000); mClock=MmioRead32(0x649202c8);
  MmioOr32(0x20100000,BIT6);
  for (I=0;I<ARRAY_SIZE(mOffsets);++I) mSaved[I]=Read(mOffsets[I]);
  mOwned=TRUE;
  MmioWrite32(0x649202c8,0); // 26 MHz, divided below to 1 MHz.
  Write(4,12); Write(8,0x2f21); Write(12,0x3000); Write(16,0);
  Write(20,0x208); Write(24,0); Write(28,9); Write(32,0);
  Write(64,0x208); Write(76,0);
  Status=RegisterRead(0x900000d0,&Id); if (EFI_ERROR(Status)) goto Fail;
  if (Id!=0x83102900) { Status=EFI_UNSUPPORTED; goto Fail; }
#ifdef W202_TOUCH_RELOAD
  // One validated upload per UEFI boot; never from an IRQ/timer.
  if (!mReloadAttempted) {
    mReloadAttempted=TRUE;
    mReloadStatus=ReloadFromFirmwareVolume();
  }
  Status=mReloadStatus;
  if (EFI_ERROR(Status)) goto Fail;
#endif
  Status=RegisterRead(0x900000a8,&State); if (EFI_ERROR(Status)) goto Fail;
  Status=RegisterRead(0x10007004,&Version); if (EFI_ERROR(Status)) goto Fail;
  /* State 0x0c is MCU safe mode. Do not pretend it can report touch. */
  if ((State&0xff)==0x0c || !Version || Version==0xffffffff) {
    Status=EFI_NOT_READY; goto Fail;
  }
  Status=RegisterRead(0x100072c0,&Reload); if (EFI_ERROR(Status)) goto Fail;
  Status=RegisterRead(0x100070f8,&Contacts); if (EFI_ERROR(Status)) goto Fail;
  Status=RegisterRead(0x1000719c,&Stylus); if (EFI_ERROR(Status)) goto Fail;
  /* Match the vendor reload handshake and packet layout before exposing input.
   * A version word alone can be stale SRAM, including after a failed upload. */
  if (Reload!=0x000072c0 || (Contacts&0xff)!=10 || (Stylus>>24)!=0) {
    Status=EFI_NOT_READY; goto Fail;
  }
  Status=RegisterRead(0x10007fd0,&mSuspendSaved); if (EFI_ERROR(Status)) goto Fail;
  DEBUG((DEBUG_WARN,"HX_NOTIFY inherited=%08x\n",mSuspendSaved));
  /* Do not take over firmware already marked asleep by another owner. */
  if (mSuspendSaved!=0) { Status=EFI_NOT_READY; goto Fail; }
  Status=ConfigureNormalReports();
  DEBUG((DEBUG_WARN,"HX_REPORT normal=%r\n",Status));
  if (EFI_ERROR(Status)) goto Fail;
  if ((State&0xff)==4) {
    Status=ResumeRetained();
    DEBUG((DEBUG_WARN,"HX_RESUME status=%r\n",Status));
    if (EFI_ERROR(Status)) goto Fail;
    MicroSecondDelay(20000);
    Status=RegisterRead(0x900000a8,&State); if (EFI_ERROR(Status)) goto Fail;
  }
#ifdef W202_TOUCH_RESUME_TEST
  Status=CycleHostState(Version);
#else
  Status=NotifyHostState(FALSE);
#endif
  DEBUG((DEBUG_WARN,"HX_NOTIFY awake=%r\n",Status));
  if (EFI_ERROR(Status)) goto Fail;
  DEBUG((DEBUG_WARN,"HX_IRQ data=%08x\n",MmioRead32(0x642200bc)));
  DEBUG((DEBUG_WARN,"HX_STATE id=%08x state=%08x version=%08x reload=%08x contacts=%08x stylus=%08x\n",Id,State,Version,Reload,Contacts,Stylus));
  RecordStartup(EFI_SUCCESS,Id,State,Version,Reload,Contacts,Stylus);
  return EFI_SUCCESS;
Fail:
  RecordStartup(Status,Id,State,Version,Reload,Contacts,Stylus);
  W202TouchStop(); return Status;
}
EFI_STATUS W202TouchRead(EFI_ABSOLUTE_POINTER_STATE *State) {
  STATIC UINTN Samples, Polls, HighSamples, Presses, Releases, Bad;
  UINT32 Irq;
  UINT8 Packet[128]={0};
  UINTN I, Length=56;
  UINT8 Nonzero=0;
  unsigned X=0,Y=0,Down=0;
  EFI_STATUS Status;
  if (!mOwned) return EFI_NOT_READY;
  /* Read at most twice per IRQ, never from a periodic timer. An empty header
   * gets one complete-stack read to diagnose a retained/partial event. */
  Irq=MmioRead32(0x642200bc)&BIT0;
  ++Polls;
  if (!(Polls%1000)) DEBUG((DEBUG_WARN,"HX_STATS polls=%u packets=%u high=%u down=%u up=%u bad=%u\n",Polls,Samples,HighSamples,Presses,Releases,Bad));
  Status=BusRead(0x30,Packet,Length); if (EFI_ERROR(Status)) return Status;
  for (I=0;I<56;++I) Nonzero|=Packet[I];
  if (!Nonzero) {
    Length=sizeof(Packet);
    Status=BusRead(0x30,Packet,Length); if (EFI_ERROR(Status)) return Status;
    for (I=0;I<56;++I) Nonzero|=Packet[I];
  }
  if (Samples==0)
    DEBUG((DEBUG_WARN,"HX_PACKET first=%02x%02x%02x%02x count=%02x checksum=%02x\n",
      Packet[0],Packet[1],Packet[2],Packet[3],Packet[52],Packet[55]));
  ++Samples;
  if (!Nonzero) {
    if (Samples==1 || Samples%16==0) {
      UINT32 Run=0, Fw=0, Dd=0, Reset=0;
      EFI_STATUS A=RegisterRead(0x900000a8,&Run);
      EFI_STATUS B=RegisterRead(0x10007f40,&Fw);
      EFI_STATUS C=RegisterRead(0x900000e8,&Dd);
      EFI_STATUS D=RegisterRead(0x900000e4,&Reset);
      DEBUG((DEBUG_WARN,"HX_EMPTY len=%u samples=%u irq=%u tail=%02x%02x%02x%02x state=%08x fw=%08x dd=%08x reset=%08x status=%r,%r,%r,%r\n",Length,Samples,Irq,Packet[56],Packet[57],Packet[126],Packet[127],Run,Fw,Dd,Reset,A,B,C,D));
      TraceReportMode();
    }
    return EFI_NOT_READY;
  }
  if (W202ParseTouch(Packet,sizeof(Packet),&X,&Y,&Down)<0) {
    if (++Bad<=3) DEBUG((DEBUG_WARN,"HX_BAD irq=%u first=%02x%02x%02x%02x tail=%02x%02x%02x%02x\n",Irq,Packet[0],Packet[1],Packet[2],Packet[3],Packet[52],Packet[53],Packet[54],Packet[55]));
    return EFI_COMPROMISED_DATA;
  }
  if (Irq) ++HighSamples;
  if (Down) ++Presses; else ++Releases;
  if (Presses+Releases<=12) DEBUG((DEBUG_WARN,"HX_EVENT native=%u,%u down=%u irq=%u len=%u\n",X,Y,Down,Irq,Length));
  /* TouchScope physical calibration: the native axes already match GOP.
   * Inferring another 180-degree transform from DPU mounting inverted them. */
  State->CurrentX=X; State->CurrentY=Y; State->CurrentZ=0;
  State->ActiveButtons=Down ? EFI_ABSP_TouchActive : 0;
  return EFI_SUCCESS;
}
