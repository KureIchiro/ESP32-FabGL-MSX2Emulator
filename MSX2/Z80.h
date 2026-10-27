/** Z80: portable Z80 emulator *******************************/
/**                                                         **/
/**                           Z80.h                         **/
/**                                                         **/
/** This file contains declarations relevant to emulation   **/
/** of Z80 CPU.                                             **/
/**                                                         **/
/** Copyright (C) Marat Fayzullin 1994-2007                 **/
/**     You are not allowed to distribute this software     **/
/**     commercially. Please, notify me, if you make any    **/
/**     changes to this file.                               **/
/*************************************************************/
#ifndef Z80_H
#define Z80_H

#ifdef __cplusplus
extern "C" {
#endif

#define LSB_FIRST
#define EXECZ80

#define INT_RST00   0x00C7
#define INT_RST08   0x00CF
#define INT_RST10   0x00D7
#define INT_RST18   0x00DF
#define INT_RST20   0x00E7
#define INT_RST28   0x00EF
#define INT_RST30   0x00F7
#define INT_RST38   0x00FF
#define INT_IRQ     INT_RST38
#define INT_NMI     0xFFFD
#define INT_NONE    0xFFFF
#define INT_QUIT    0xFFFE

#define S_FLAG      0x80
#define Z_FLAG      0x40
#define H_FLAG      0x10
#define P_FLAG      0x04
#define V_FLAG      0x04
#define N_FLAG      0x02
#define C_FLAG      0x01

#define IFF_1       0x01
#define IFF_IM1     0x02
#define IFF_IM2     0x04
#define IFF_2       0x08
#define IFF_EI      0x20
#define IFF_HALT    0x80

#ifndef BYTE_TYPE_DEFINED
#define BYTE_TYPE_DEFINED
typedef unsigned char byte;
#endif
#ifndef WORD_TYPE_DEFINED
#define WORD_TYPE_DEFINED
typedef unsigned short zword;
#endif
typedef signed char offset;

typedef union
{
#ifdef LSB_FIRST
  struct { byte l,h; } B;
#else
  struct { byte h,l; } B;
#endif
  zword W;
} pair;

typedef struct
{
  pair AF,BC,DE,HL,IX,IY,PC,SPtr;
  pair AF1,BC1,DE1,HL1;
  byte IFF,I;
  byte R;

  int IPeriod,ICount;
  int IBackup;
  zword IRequest;
  byte IAutoReset;
  byte TrapBadOps;
  zword Trap;
  byte Trace;
  void *User;
} Z80;

void ResetZ80(register Z80 *R);

#ifdef EXECZ80
int ExecZ80(register Z80 *R,register int RunCycles);
#endif

void IntZ80(register Z80 *R,register zword Vector);

#ifdef RUNZ80
zword RunZ80(register Z80 *R);
#endif

#ifdef STEPZ80
void StepZ80(register Z80 *R);
#endif

void WrZ80(register zword Addr,register byte Value);
byte RdZ80(register zword Addr);

void OutZ80(register zword Port,register byte Value);
byte InZ80(register zword Port);

void PatchZ80(register Z80 *R);

#ifdef DEBUG
byte DebugZ80(register Z80 *R);
#endif

zword LoopZ80(register Z80 *R);

#ifndef JUMPZ80
#define JumpZ80(PC)
#else
void JumpZ80(zword PC);
#endif

#ifdef __cplusplus
}
#endif
#endif
