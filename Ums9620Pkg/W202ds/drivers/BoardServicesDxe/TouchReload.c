// SPDX-License-Identifier: BSD-2-Clause-Patent
// HX83102J zero-flash RAM diagnostic. No GPIO, TCON or display register writes.
#include "TouchReload.h"

STATIC UINT32 Le32(CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1]<<8) | ((UINT32)P[2]<<16) | ((UINT32)P[3]<<24);
}
STATIC UINT32 Crc(CONST UINT8 *P, UINTN Size) {
  UINT32 Value=MAX_UINT32;
  UINTN I, Bit;
  for (I=0;I<Size;++I) {
    Value^=P[I];
    for (Bit=0;Bit<8;++Bit) Value=(Value>>1)^((Value&1)?0x82f63b78U:0);
  }
  return Value;
}
STATIC EFI_STATUS Prepare(CONST UINT8 *Fw, UINTN Size, UINT8 *Config) {
  // This is the pinned guoxian image, not a generic Himax firmware loader.
  STATIC CONST UINT32 Map[8][3]={
    {0x20000000,108544,0x400}, {0x10007000,120,0x29400},
    {0x10007084,528,0x294fe}, {0x10007300,180,0x29e00},
    {0x100072f0,16,0x29dd0}, {0x1000bf30,3204,0x2b000},
    {0x100073f0,32,0x29de0}, {0x10002ca4,184,0x2d180}
  };
  UINTN I,J;
  UINT32 Table=0, Sum;
  if (Size!=261120 || Fw[14]!=0x87) return EFI_COMPROMISED_DATA;
  for (I=0;I<1024;I+=16) {
    Sum=0;
    for (J=0;J<16;++J) Sum+=Fw[I+J];
    if (!Sum) break; // The descriptor stream ends before the padded 1 KiB header.
    if (Sum&255) return EFI_COMPROMISED_DATA;
    if (Le32(Fw+I)==0xa00) {
      if (Table) return EFI_COMPROMISED_DATA;
      Table=Le32(Fw+I+4);
    }
  }
  if (Table!=0x28400) return EFI_COMPROMISED_DATA;
  for (I=0;I<8;++I) {
    CONST UINT8 *Entry=Fw+Table+16*I;
    for (J=0;J<3;++J) if (Le32(Entry+J*4)!=Map[I][J]) return EFI_COMPROMISED_DATA;
    if (Le32(Entry+12)!=(I?0:8)) return EFI_COMPROMISED_DATA;
  }
  if (Crc(Fw+0x400,108544)!=0) return EFI_CRC_ERROR;
  for (I=0;I<W202_TOUCH_CONFIG_SIZE;++I) Config[I]=0;
  for (I=1;I<8;++I)
    for (J=0;J<Map[I][1];++J) Config[Map[I][0]-0x10002ca4+J]=Fw[Map[I][2]+J];
  if (Crc(Config,W202_TOUCH_CONFIG_SIZE)!=0x5ced9ac0) return EFI_CRC_ERROR;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS HardwareCrc(CONST W202_TOUCH_RELOAD_IO *Io,
                              UINT32 Address, UINT32 Size, UINT32 *CrcOut) {
  UINT32 Value;
  UINTN I;
  EFI_STATUS S;
  S=Io->Write(0x80050020,Address); if (EFI_ERROR(S)) return S;
  S=Io->Write(0x80050028,0x00990000|(Size/4)); if (EFI_ERROR(S)) return S;
  for (I=0;I<100;++I) {
    S=Io->Read(0x80050000,&Value); if (EFI_ERROR(S)) return S;
    if (!(Value&1)) return Io->Read(0x80050018,CrcOut);
    Io->Delay(1000);
  }
  return EFI_TIMEOUT;
}
EFI_STATUS W202TouchReload(CONST UINT8 *Firmware, UINTN Size, UINT8 *Config,
                          CONST W202_TOUCH_RELOAD_IO *Io,
                          W202_TOUCH_RELOAD_RESULT *R) {
  UINTN I;
  UINT32 Value, Id;
  EFI_STATUS S;
  if (!Firmware || !Config || !Io || !R || !Io->Read || !Io->Write ||
      !Io->WriteMemory || !Io->Sense || !Io->Delay) return EFI_INVALID_PARAMETER;
  R->Stage=1; R->EntityCrc=R->ConfigCrc=MAX_UINT32; R->State=R->Reload=0;
  S=Prepare(Firmware,Size,Config); if (EFI_ERROR(S)) return S;
  S=Io->Read(0x900000d0,&Id); if (EFI_ERROR(S)) return S;
  if (Id!=0x83102900) return EFI_UNSUPPORTED;
  // Deliberately narrower than vendor cold init: reset the touch MCU only.
  // The shared panel/TCON and GPIO reset must remain under their own owner.
  R->Stage=2;
  S=Io->Write(0x90000018,0x55); if (EFI_ERROR(S)) return S;
  Io->Delay(5000);
  S=Io->Write(0x9000005c,0xa5); if (EFI_ERROR(S)) return S;
  for (I=0;I<5;++I) {
    S=Io->Sense(TRUE); if (EFI_ERROR(S)) return S;
    Io->Delay(1000);
    S=Io->Read(0x900000a8,&R->State); if (EFI_ERROR(S)) return S;
    if ((R->State&255)==0x0c) break;
  }
  if (I==5) return EFI_NOT_READY;
  S=Io->Write(0x80010000,0xecce); if (EFI_ERROR(S)) return S;
  S=Io->Read(0x80010000,&Value); if (EFI_ERROR(S)) return S;
  if ((Value&0xffff)!=0xecce) return EFI_DEVICE_ERROR;
  R->Stage=3;
  S=Io->WriteMemory(0x20000000,Firmware+0x400,108544); if (EFI_ERROR(S)) return S;
  S=HardwareCrc(Io,0x20000000,108544,&R->EntityCrc); if (EFI_ERROR(S)) return S;
  if (R->EntityCrc) return EFI_CRC_ERROR;
  R->Stage=4;
  S=Io->WriteMemory(0x10002ca4,Config,W202_TOUCH_CONFIG_SIZE); if (EFI_ERROR(S)) return S;
  S=HardwareCrc(Io,0x10002ca4,W202_TOUCH_CONFIG_SIZE,&R->ConfigCrc); if (EFI_ERROR(S)) return S;
  if (R->ConfigCrc!=0x5ced9ac0) return EFI_CRC_ERROR;
  R->Stage=5;
  S=Io->Write(0x10007f00,0); if (EFI_ERROR(S)) return S;
  S=Io->Write(0x100072ec,0); if (EFI_ERROR(S)) return S;
  S=Io->Write(0x10007f04,0); if (EFI_ERROR(S)) return S;
  S=Io->Write(0x10007294,1); if (EFI_ERROR(S)) return S;
  S=Io->Write(0x100072c0,0); if (EFI_ERROR(S)) return S;
  S=Io->Write(0x9000005c,0); if (EFI_ERROR(S)) return S;
  Io->Delay(10000);
  S=Io->Sense(FALSE); if (EFI_ERROR(S)) return S;
  S=Io->Write(0x80010000,0xecce); if (EFI_ERROR(S)) return S;
  for (I=0;I<8;++I) {
    S=Io->Write(0x90000048,0xec); if (EFI_ERROR(S)) return S;
    Io->Delay(2000);
    S=Io->Read(0x90000048,&Value); if (EFI_ERROR(S)) return S;
    if ((Value&0xffff)==0x1ec) break;
  }
  if (I==8) return EFI_NOT_READY;
  R->Stage=6;
  for (I=0;I<50;++I) {
    S=Io->Read(0x100072c0,&R->Reload); if (EFI_ERROR(S)) return S;
    S=Io->Read(0x900000a8,&R->State); if (EFI_ERROR(S)) return S;
    if (R->Reload==0x72c0 && (R->State&255)==5) {
      R->Stage=7; return EFI_SUCCESS;
    }
    Io->Delay(10000);
  }
  return EFI_NOT_READY;
}
