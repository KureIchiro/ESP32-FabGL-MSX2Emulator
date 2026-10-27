// ============================================================================
//  V9958 (MSX2+ VDP)  ※V9938の上位互換。YJK表示・横スクロール・R#25〜27を追加
//
//  出力: 1ラインずつ、FabGL VGAController のビューポート行(512バイト、
//        ネイティブ8bpp = 同期ビット | B<<4 | G<<2 | R、各2bit)へ直接書く。
//        4バイト内の並びは px2,px3,px0,px1 (FabGLのDMA順)。
//  VRAM: 128KB。G6/G7(SCREEN 7/8)は実機どおり2バンクのインターリーブ配置で持つ。
//  コマンド(LMMM等)は MSX2_VDP_CMD_SPEED (msx2_config.h) の速さで、CPU が VDP を触るたびと
//  ラインの頭でその時点まで進める(0 なら書き込まれた時点で一括実行し CE は即0に戻る)。
//  R#18(画面位置調整): 横はラインごと(ライン全体を左右にずらす)、縦はフレームごとに効かせる。
//  vdpSetV9938(true) で V9938 として振る舞う。
// ============================================================================
#pragma once
#include "msx2_config.h"

#define VDP_VRAM_SIZE 0x20000

struct VdpState {
  uint8_t  reg[64];
  uint8_t  status[10];
  uint16_t palette[16];
  uint32_t vramAddr;
  uint8_t  readAhead;
  uint8_t  latch;
  bool     latchFull;
  uint8_t  palLatch;
  bool     palLatchFull;
  int      blinkCounter;
  bool     blinkOn;
};

void vdpInit(uint8_t *vram, uint8_t **rows, uint8_t syncBits);
void vdpReset();
void vdpSetV9938(bool v9938);
bool vdpIsV9938();
void vdpSetSpriteNoLimit(bool on);
bool vdpSpriteNoLimit();

uint8_t vdpReadPort(int port);
void    vdpWritePort(int port, uint8_t v);

void vdpLine(int line);
bool vdpIrq();

int  vdpLineCycle();

VdpState *vdpState();
uint8_t  *vdpVram();

int  vdpDisplayMode();
int  vdpDisplayLines();

#if MSX2_PROFILE
struct VdpProf {
  uint32_t sprCycles;
  uint32_t outCycles;
  uint32_t lines;
  uint32_t modeLines[32];
  uint32_t hsLines, adjLines;
  uint32_t sprLines;
  uint32_t sprVis;
  uint32_t cmdCycles[16];
  uint32_t cmdCount[16];
  uint32_t cmdDots;
  uint32_t cmdData;
  uint32_t vramWrites, vramReads;
  uint32_t cmdMax;
  uint16_t cmdMaxNx, cmdMaxNy;
  uint8_t  cmdMaxCmd;
};
VdpProf *vdpProf();
#endif
