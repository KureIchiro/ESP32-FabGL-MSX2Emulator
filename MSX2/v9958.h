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
  uint16_t palette[16];        // 0x0RGB の各3bit (RGB333)
  uint32_t vramAddr;           // 17bit (R#14 と下位14bit を合わせた値)
  uint8_t  readAhead;
  uint8_t  latch;              // ポート99h の1バイト目
  bool     latchFull;
  uint8_t  palLatch;           // ポート9Ah の1バイト目
  bool     palLatchFull;
  int      blinkCounter;
  bool     blinkOn;
};

// 初期化。vram は 128KB(PSRAM可)。rows は MSX2_SCREEN_H 行ぶんの出力行ポインタ。
// syncBits は FabGL の createRawPixel(黒) の値(通常 0xC0)。
void vdpInit(uint8_t *vram, uint8_t **rows, uint8_t syncBits);
void vdpReset();
// V9938 (MSX2 の VDP) として動かす: S#1 の ID が 0 になり、R#25〜27 への書き込みを無視する。
// (R#18 の画面位置調整はどちらでも効く)
void vdpSetV9938(bool v9938);
bool vdpIsV9938();
// 1ラインのスプライトの枚数の制限をなくす(true)。Shift+F8 で切り替え(MSX1 版と同じ)
void vdpSetSpriteNoLimit(bool on);
bool vdpSpriteNoLimit();

uint8_t vdpReadPort(int port);          // port = 0..3 (98h〜9Bh)
void    vdpWritePort(int port, uint8_t v);

// 1ラインの処理。line = 0..261。割込みフラグを更新し、表示ラインなら描画を予約する。
// Z80でそのラインを実行する前に呼ぶ。予約したラインは、Z80 がそのラインの表示期間のまんなか
// (vdpLineCycle() >= 128)を過ぎてから VDP を書き換えるとき、または次の vdpLine の頭で描く。
void vdpLine(int line);
bool vdpIrq();                          // INT線の状態(レベル)

// 現在ライン内の経過サイクル数(0〜MSX2_CYCLES_PER_LINE、Z80 3.58MHz 換算)。コア側が実装する。
// S#2 の HR、VDPコマンドの進み、ラインを描く時刻に使う(CPU の速さを変えてもこの範囲で返すこと)。
int  vdpLineCycle();

// ステートセーブ用
VdpState *vdpState();
uint8_t  *vdpVram();

// ---- テスト/デバッグ用 ----
int  vdpDisplayMode();                  // (M5 M4 M3 M2 M1) の5bit値
int  vdpDisplayLines();                 // 192 or 212

#if MSX2_PROFILE
// ---- 処理時間の内訳 (msx2_config.h の MSX2_PROFILE) ----
// VDP側で数える分。コア(msx2.cpp)が1フレームごとに読んで0に戻す。時間は msx2ProfClock() の値。
struct VdpProf {
  uint32_t sprCycles;                   // スプライト処理 (vdpLine の内数)
  uint32_t outCycles;                   // 背景の生成+ビューポートへの書き出し (一体化したので両方。vdpLine の内数)
  uint32_t lines;                       // 表示オン(BL=1)で描いたライン数(1ラインあたりの時間を出す分母)
  uint32_t modeLines[32];               // そのうち画面モード(M5〜M1)ごとのライン数
  uint32_t hsLines, adjLines;           // R#26/R#27 が 0 でないライン数 / R#18 が 0 でないライン数
  uint32_t sprLines;                    // スプライトを重ねたライン数(256ドットの合成ループが回った)
  uint32_t sprVis;                      // 表示したスプライトの延べ数(1ライン1枚で1)
  uint32_t cmdCycles[16];               // コマンド実行 (R#46/R#44 書き込み・S#7 読み出し)。CMD 上位4bit別
  uint32_t cmdCount[16];                // R#46 に書かれたコマンド数。CMD 上位4bit別
  uint32_t cmdDots;                     // LMMV/LMMM/HMMV/HMMM/YMMM/LINE の名目ドット数 (NX×NY 等)
  uint32_t cmdData;                     // HMMC/LMMC の R#44 書き込み + LMCM の S#7 読み出し
  uint32_t vramWrites, vramReads;       // ポート 98h
  uint32_t cmdMax;                      // R#46 書き込み1回(=ブロック系コマンド1つ)の最長時間
  uint16_t cmdMaxNx, cmdMaxNy;          // そのコマンドの NX, NY (R#40〜43)
  uint8_t  cmdMaxCmd;                   // そのコマンドの R#46 の値
};
VdpProf *vdpProf();
#endif
