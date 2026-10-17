// ============================================================================
//  MSX2+ マシン本体  (構成は msx2.h を参照)
// ============================================================================
#ifdef ARDUINO
#pragma GCC optimize("O3")
#endif
#include "msx2.h"
#include "v9958.h"
#include "Z80.h"
#include <string.h>
#if MSX2_PROFILE
#include <stdarg.h>
#include <stdio.h>
#endif

// ---------------------------------------------------------------------------
//  Z80
// ---------------------------------------------------------------------------
// MSX2 の Z80 は M1 サイクルごとに1ウェイト入る。MSX1版と同じく、その分を
// 1ラインの実行サイクル数を減らして近似する(228 × 約0.9)。
// msx2SetCpuCycles で速くできる(フロントエンドの F11: 205 → 270 → 335)。
static int cpuCyclesPerLine = 205;
#define CPU_CYCLES_PER_LINE cpuCyclesPerLine

extern "C" {
  Z80 msx2_cpu;
  uint8_t *op_map[8];                  // 命令読み込み用(Z80.c の OpZ80 が直接引く)
  uint8_t *msx2_rd_map[8];             // データ読み出し用(Z80.c の RdZ80 が直接引く。nullptr = RdZ80Slow)
  uint8_t *msx2_wr_map[8];             // データ書き込み用(Z80.c の WrZ80 が直接引く。nullptr = WrZ80Slow)
  byte RdZ80Slow(zword a);
  void WrZ80Slow(zword a, byte v);
  byte RdZ80(zword a);
  void WrZ80(zword a, byte v);
  byte InZ80(zword port);
  void OutZ80(zword port, byte v);
  void PatchZ80(Z80 *R);
}

// ---------------------------------------------------------------------------
//  スロット
// ---------------------------------------------------------------------------
static MSX2_DRAM uint8_t emptyPage[0x2000];        // 何もないところ(0xFF)
static uint8_t *pageRd[4][4][8];                    // [基本][拡張][8KBページ] nullptr=空き
static uint8_t *pageWr[4][4][8];                    // nullptr=書けない(ROM/空き)
static bool expanded[4] = { true, false, false, true };   // msx2SetSlotExpanded で変更可
static uint8_t psr;                                 // A8h 基本スロット
static uint8_t ssr[4];                              // FFFFh 拡張スロット

// データの読み書きの表(Z80.c の RdZ80/WrZ80 が直接引く)。nullptr = readSpecial() / writeSpecial() へ

static uint8_t *diskRom;                            // 3-2
static bool     diskVisible;                        // 6000h-7FFFh にディスクROMが見えている

// ---- メモリマッパー (3-0) ----
static uint8_t *ramSeg[MSX2_MAPPER_SEGMENTS];
static uint8_t  mapReg[4];                          // FCh-FFh

// ---- カートリッジ (スロット1, 2) ----
struct Cart {
  uint8_t   *rom;
  uint32_t   mask8k;                               // 8KBバンク番号のマスク
  Msx2Mapper mapper;
  uint8_t    bank[4];                              // 4000h/6000h/8000h/A000h の8KBバンク
  bool       sccActive;
};
static Cart cart[3];                                 // [1],[2] を使う
// SCC の波形メモリ(読み出し用。音はフロントエンドの msx2SccWrite が鳴らす)。
// SCC モードでは ch4 と ch5 が 9860h〜987Fh の波形を共用する。SD スナッチャーは
// 9000h に 3Fh を書いて 9800h〜 を読み書きし、SCC があるスロットを探す。
static uint8_t sccWave[4][32];
static bool    sccReadable;                          // 今 9800h〜9FFFh で SCC が読める(ページ4のデータ読み出しを readSpecial へ)

// ---- FM-PAC (スロット2。あればスロット2のカートリッジより優先) ----
//   4000h-7FFFh: ROM 16KB × 最大4バンク(7FF7h で選択)
//   7FF6h: bit0=I/Oポート 7Ch/7Dh を有効化, bit4=SRAMの鍵(5FFEh/5FFFh)をリセットして固定
//   7FF4h/7FF5h: OPLL のアドレス/データ(メモリ書き込み。7FF6h に関係なく効く)
//   5FFEh=4Dh かつ 5FFFh=69h で 4000h-5FFDh が 8KB SRAM になる(6000h-7FFFh は FFh)
//   動作は openMSX の MSXFmPac と同じ。
struct FmPac {
  uint8_t *rom;
  uint8_t  bankMask;
  uint8_t *sram;                                     // 8KB
  uint8_t  enable;                                   // 7FF6h
  uint8_t  bank;                                     // 7FF7h
  uint8_t  r1ffe, r1fff;                             // 5FFEh / 5FFFh
  bool     sramOn;
};
static FmPac fmpac;
static bool  fmpacVisible;                           // ページ1にスロット2が見えている
static volatile uint32_t fmpacSramWrites;            // SRAM の中身を変えた回数(フロントエンドが SD に保存する目安)

// ---- 漢字ROM / RTC / その他I/O ----
static uint8_t *kanjiFont;
static uint32_t kanjiSize;
static uint32_t kanjiAddr[2];                        // 第1水準 / 第2水準
static uint8_t  f4reg;
static bool     f4Inverted;                          // F4h(リセット状態)がビット反転型の機種か
static uint8_t  ppiC;
static uint8_t  psgAddr, psgReg[16];
static bool     fmEnabled;

volatile uint8_t msx2_key_matrix[16];
volatile uint8_t msx2_joy[2] = { 0x7F, 0x7F };

// ---- マウス(openMSX の MSXMouse と同じ) ----
//   ジョイスティック端子の 8 番ピン(PSG R#15 bit4 = ポート1、bit5 = ポート2)を切り替えるたびに
//   X 上位4bit → X 下位4bit → Y 上位4bit → Y 下位4bit の順に R#14 の下位4bit で返す。
//   Y 下位を読んだあとの切り替えで、その間に動いた量を取り込む(左・上が +、-127〜127)。
//   ボタンはトリガ A(bit4)= 左、B(bit5)= 右(0 = 押下)。
volatile int32_t msx2_mouse_x, msx2_mouse_y;          // フロントエンドが積算する(右・上が +)
volatile uint8_t msx2_mouse_buttons;
static int     mousePort;                             // 0 = なし、1 / 2
static int     mousePhase = 3;
static int8_t  mouseRelX, mouseRelY;
static int32_t mouseLastX, mouseLastY;
void msx2SetMousePort(int port) { mousePort = (port == 1 || port == 2) ? port : 0; }

static void mouseStrobe() {
  if (mousePhase == 3) {
    int32_t x = msx2_mouse_x, y = msx2_mouse_y;
    int dx = -(x - mouseLastX), dy = y - mouseLastY;  // MSX は左・上が +
    if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
    if (dy > 127) dy = 127; else if (dy < -127) dy = -127;
    mouseRelX = (int8_t)dx; mouseRelY = (int8_t)dy;
    mouseLastX -= dx;                                 // 取り込んだ分だけ進める(残りは次へ)
    mouseLastY += dy;
  }
  mousePhase = (mousePhase + 1) & 3;
}

static uint8_t mouseRead(uint8_t joy) {
  uint8_t n;
  switch (mousePhase) {
    case 0:  n = (uint8_t)(mouseRelX >> 4); break;
    case 1:  n = (uint8_t)mouseRelX; break;
    case 2:  n = (uint8_t)(mouseRelY >> 4); break;
    default: n = (uint8_t)mouseRelY; break;
  }
  uint8_t b = msx2_mouse_buttons;
  return (uint8_t)((joy & 0xC0) | ((b & 1) ? 0 : 0x10) | ((b & 2) ? 0 : 0x20) | (n & 0x0F));
}

static void updateMemoryMap();

// ---------------------------------------------------------------------------
//  処理時間の内訳 (msx2_config.h の MSX2_PROFILE。集計と出力は末尾の msx2ProfReport)
// ---------------------------------------------------------------------------
#if MSX2_PROFILE
enum { PN_BANK, PN_MAP, PN_IRQ, PN_HALT, PN_COUNT };   // このファイルで数える回数
static uint32_t profN[PN_COUNT];                       // 今のフレームのぶん
static uint32_t profMapCyc, profVioCyc;                // updateMemoryMap / VDPポート(98h〜9Bh)の時間
#define PROF_COUNT(i) (profN[i]++)
#else
#define PROF_COUNT(i) ((void)0)
#endif

// ---------------------------------------------------------------------------
//  RTC (RP5C01)
//    ブロック0: 時刻(BCDの各桁) / 1: アラーム・12/24・うるう年 / 2,3: バックアップRAM
// ---------------------------------------------------------------------------
static uint8_t rtcAddr, rtcMode, rtcRegs[4][13];
static int     rtcFrames;
static const uint8_t kRtcMask0[13] = { 15, 7, 15, 7, 15, 3, 7, 15, 3, 15, 1, 15, 15 };

static void rtcInit() {
  memset(rtcRegs, 0, sizeof(rtcRegs));
  // 2026年1月1日(木) 0:00:00。MSXの年は1980年起点。
  uint8_t *t = rtcRegs[0];
  t[7] = 1;                 // 日
  t[9] = 1;                 // 月
  t[11] = 46 % 10; t[12] = 46 / 10;
  t[6] = 4;                 // 曜日
  rtcRegs[1][10] = 1;       // 24時間制
  rtcMode = 0x08;           // タイマー動作
}

static void rtcTickSecond() {
  static const uint8_t dpm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  uint8_t *t = rtcRegs[0];
  int sec = t[0] + t[1] * 10, min = t[2] + t[3] * 10, hour = t[4] + t[5] * 10;
  int day = t[7] + t[8] * 10, mon = t[9] + t[10] * 10, year = t[11] + t[12] * 10;
  if (++sec >= 60) {
    sec = 0;
    if (++min >= 60) {
      min = 0;
      if (++hour >= 24) {
        hour = 0;
        t[6] = (t[6] + 1) % 7;
        int m = (mon >= 1 && mon <= 12) ? mon - 1 : 0;
        int dim = dpm[m] + ((m == 1 && (year % 4) == 0) ? 1 : 0);
        if (++day > dim) {
          day = 1;
          if (++mon > 12) { mon = 1; year = (year + 1) % 100; }
        }
      }
    }
  }
  t[0] = sec % 10; t[1] = sec / 10; t[2] = min % 10; t[3] = min / 10;
  t[4] = hour % 10; t[5] = hour / 10; t[7] = day % 10; t[8] = day / 10;
  t[9] = mon % 10; t[10] = mon / 10; t[11] = year % 10; t[12] = year / 10;
}

static uint8_t rtcRead() {
  int a = rtcAddr & 15;
  if (a == 13) return 0xF0 | rtcMode;
  if (a >= 13) return 0xFF;
  int blk = rtcMode & 3;
  uint8_t v = rtcRegs[blk][a];
  if (blk == 0) v &= kRtcMask0[a];
  return 0xF0 | (v & 15);
}

static void rtcWrite(uint8_t v) {
  int a = rtcAddr & 15;
  v &= 15;
  if (a == 13) { rtcMode = v; return; }
  if (a >= 13) return;                               // テスト/リセットは無視
  rtcRegs[rtcMode & 3][a] = v;
}

// ---------------------------------------------------------------------------
//  ROM / RAM 登録
// ---------------------------------------------------------------------------
static void setRom(int prim, int sub, int firstPage, uint8_t *rom, uint32_t size) {
  for (uint32_t off = 0; off + 0x2000 <= size && firstPage + (int)(off >> 13) < 8; off += 0x2000) {
    pageRd[prim][sub][firstPage + (off >> 13)] = rom + off;
  }
}

void msx2SetRom(int prim, int sub, uint16_t addr, uint8_t *rom, uint32_t size) {
  setRom(prim & 3, sub & 3, addr >> 13, rom, size);
}
void msx2SetBios(uint8_t *bios)                 { setRom(0, 0, 0, bios, 0x8000); }
void msx2SetSubRom(uint8_t *sub)                { setRom(3, 1, 0, sub, 0x4000); }
void msx2SetKanjiBasic(uint8_t *rom, uint32_t s){ setRom(3, 1, 2, rom, s > 0x8000 ? 0x8000 : s); }
void msx2SetMusicRom(uint8_t *fm, int prim, int sub) { setRom(prim & 3, sub & 3, 2, fm, 0x4000); fmEnabled = (fm != nullptr); }
void msx2SetSlotExpanded(int prim, bool e)      { expanded[prim & 3] = e; }
void msx2SetF4Inverted(bool inv)                { f4Inverted = inv; }
void msx2SetDiskRom(uint8_t *disk)              { diskRom = disk; setRom(3, 2, 2, disk, 0x4000); }
void msx2SetKanjiFont(uint8_t *font, uint32_t s){ kanjiFont = font; kanjiSize = s; }
void msx2SetRamSegment(int seg, uint8_t *mem)   { if (seg >= 0 && seg < MSX2_MAPPER_SEGMENTS) ramSeg[seg] = mem; }

static void cartUpdatePages(int s) {
  Cart &c = cart[s];
  for (int i = 0; i < 4; i++) {
    pageRd[s][0][2 + i] = c.rom ? c.rom + ((uint32_t)(c.bank[i] & c.mask8k) << 13) : nullptr;
  }
}

void msx2SetCartridge(int s, uint8_t *rom, uint32_t size, Msx2Mapper mapper) {
  if (s != 1 && s != 2) return;
  Cart &c = cart[s];
  memset(&c, 0, sizeof(c));
  c.rom = rom;
  c.mapper = mapper;
  uint32_t banks = size >> 13;                       // size は 2のべき乗に切り上げてあること
  c.mask8k = banks ? banks - 1 : 0;
  switch (mapper) {
    case MSX2_MAP_ASCII8:  c.bank[0] = c.bank[1] = c.bank[2] = c.bank[3] = 0; break;
    case MSX2_MAP_ASCII16: c.bank[0] = 0; c.bank[1] = 1; c.bank[2] = 0; c.bank[3] = 1; break;
    case MSX2_MAP_RTYPE:   c.bank[0] = 46; c.bank[1] = 47; c.bank[2] = 0; c.bank[3] = 1; break;
    default:               c.bank[0] = 0; c.bank[1] = 1; c.bank[2] = 2; c.bank[3] = 3; break;
  }
  cartUpdatePages(s);
}

// ---------------------------------------------------------------------------
//  メモリマップ
// ---------------------------------------------------------------------------
static void mapperUpdatePages() {
  for (int i = 0; i < 4; i++) {
    uint8_t *seg = ramSeg[mapReg[i] & (MSX2_MAPPER_SEGMENTS - 1)];
    for (int k = 0; k < 2; k++) {
      uint8_t *p = seg ? seg + k * 0x2000 : nullptr;
      pageRd[3][0][2 * i + k] = p;
      pageWr[3][0][2 * i + k] = p;
    }
  }
}

static void updateMemoryMap() {
#if MSX2_PROFILE
  uint32_t t0 = msx2ProfClock();
  PROF_COUNT(PN_MAP);
#endif
  for (int p = 0; p < 8; p++) {
    int sh = (p >> 1) * 2;
    int prim = (psr >> sh) & 3;
    int sub = expanded[prim] ? ((ssr[prim] >> sh) & 3) : 0;
    uint8_t *rd = pageRd[prim][sub][p];
    op_map[p] = msx2_rd_map[p] = rd ? rd : emptyPage;
    msx2_wr_map[p] = pageWr[prim][sub][p];
  }
  // FFFFh(拡張スロットレジスタ)を読むため、ページ3が拡張スロットなら E000h-FFFFh の
  // データ読み出しを readSpecial() へ回す。命令の読み込みは op_map なので遅くならない。
  if (expanded[psr >> 6]) msx2_rd_map[7] = nullptr;
  // ソニー型FDC(7FF8h-7FFFh)
  diskVisible = diskRom && ((psr >> 2) & 3) == 3 && ((ssr[3] >> 2) & 3) == 2;
  if (diskVisible) msx2_rd_map[3] = nullptr;
  // FM-PAC の 7FF6h/7FF7h は読むとレジスタ値
  fmpacVisible = fmpac.rom && ((psr >> 2) & 3) == 2;
  if (fmpacVisible) msx2_rd_map[3] = nullptr;
  // SCC の波形メモリ(8000h〜9FFFh のデータ読み出しだけ readSpecial へ。命令の読み込みは op_map)
  int p2 = (psr >> 4) & 3;
  sccReadable = (p2 == 1 || p2 == 2) && cart[p2].sccActive;
  if (sccReadable) msx2_rd_map[4] = nullptr;
#if MSX2_PROFILE
  profMapCyc += msx2ProfClock() - t0;
#endif
}

static uint8_t MSX2_FAST readSpecial(uint16_t a) {
  if (a == 0xFFFF) {
    int prim = psr >> 6;
    if (expanded[prim]) return (uint8_t)~ssr[prim];
  }
  if (diskVisible && a >= 0x7FF8 && a <= 0x7FFF) return msx2FdcRead(a & 7);
  if (fmpacVisible && (a == 0x7FF6 || a == 0x7FF7)) return (a == 0x7FF6) ? fmpac.enable : fmpac.bank;
  if (sccReadable && a >= 0x9800 && a <= 0x9FFF) {
    Cart &c = cart[(psr >> 4) & 3];
    if (c.mapper != MSX2_MAP_KONAMI || a <= 0x98FF) {     // (K) は 9800h〜98FFh だけ、(KS)/SCC は 256 バイトごとのミラー
      uint8_t r = a & 0xFF;
      return r < 0x80 ? sccWave[r >> 5][r & 0x1F] : 0xFF;
    }
  }
  return op_map[a >> 13][a & 0x1FFF];
}

// SCC への書き込み: 波形を覚えてからフロントエンドへ(a は 9800h〜98FFh)
static void sccWrite(uint16_t a, uint8_t v) {
  uint8_t r = a & 0xFF;
  if (r < 0x80) sccWave[r >> 5][r & 0x1F] = v;
  msx2SccWrite(a, v);
}

static inline uint8_t readByte(uint16_t a) {
  const uint8_t *p = msx2_rd_map[a >> 13];
  if (MSX2_LIKELY(p != nullptr)) return p[a & 0x1FFF];
  return readSpecial(a);
}

static void cartWrite(int s, uint16_t a, uint8_t v) {
  Cart &c = cart[s];
  if ((!c.rom && c.mapper != MSX2_MAP_SCC) || a < 0x4000 || a >= 0xC000) return;
  switch (c.mapper) {
    case MSX2_MAP_KONAMI:
      // (K): SCCなし(6000h/8000h/A000h)とSCCあり(5000h/7000h/9000h/B000h)の両方で動くよう、
      // ページ内のどこへの書き込みでもバンクを切り替える
      if (c.sccActive && a >= 0x9800 && a <= 0x98FF) { sccWrite(a, v); return; }
      if (a >= 0x9000 && a <= 0x97FF) c.sccActive = ((v & 0x3F) == 0x3F);
      c.bank[(a - 0x4000) >> 13] = v;
      break;
    case MSX2_MAP_KONAMI_SCC:
      // (KS): 実機のコナミSCCどおり 5000h/7000h/9000h/B000h の各 2KB だけがバンク切替。
      // SCC は 9800h-9FFFh(256バイト単位のミラー)。それ以外への書き込みは無視する
      // (メタルギア2は 8000h などへ書くので、(K) だとバンクが変わって止まる)。
      if (a >= 0x9800 && a <= 0x9FFF) { if (c.sccActive) sccWrite(0x9800 | (a & 0xFF), v); return; }
      if ((a & 0x1800) != 0x1000) return;
      if (a >= 0x9000 && a <= 0x97FF) c.sccActive = ((v & 0x3F) == 0x3F);
      c.bank[(a - 0x4000) >> 13] = v;
      break;
    case MSX2_MAP_SCC:
      // ROM の無い SCC カートリッジ: 9000h〜97FFh に 3Fh で 9800h〜9FFFh に SCC が出る
      if (a >= 0x9800 && a <= 0x9FFF) { if (c.sccActive) sccWrite(0x9800 | (a & 0xFF), v); return; }
      if (a >= 0x9000 && a <= 0x97FF) {
        bool on = ((v & 0x3F) == 0x3F);
        if (on == c.sccActive) return;
        c.sccActive = on;
        updateMemoryMap();
      }
      return;
    case MSX2_MAP_ASCII8:
      if (a >= 0x6000 && a < 0x8000) c.bank[(a >> 11) & 3] = v;
      else return;
      break;
    case MSX2_MAP_ASCII16:
      if (a >= 0x6000 && a < 0x6800)      { c.bank[0] = v * 2; c.bank[1] = v * 2 + 1; }
      else if (a >= 0x7000 && a < 0x7800) { c.bank[2] = v * 2; c.bank[3] = v * 2 + 1; }
      else return;
      break;
    case MSX2_MAP_RTYPE:
      if (a >= 0x7000 && a < 0x8000) { uint8_t b = (v % 24) * 2; c.bank[2] = b; c.bank[3] = b + 1; }
      else return;
      break;
    default:
      return;
  }
  PROF_COUNT(PN_BANK);
  cartUpdatePages(s);
  updateMemoryMap();
}

static void fmpacUpdatePages() {
  if (!fmpac.rom) return;
  uint8_t *b = fmpac.rom + ((uint32_t)(fmpac.bank & fmpac.bankMask) << 14);
  pageRd[2][0][2] = fmpac.sramOn ? fmpac.sram : b;
  pageRd[2][0][3] = fmpac.sramOn ? emptyPage : b + 0x2000;
}

static void fmpacCheckSram() {
  bool on = fmpac.sram && fmpac.r1ffe == 0x4D && fmpac.r1fff == 0x69;
  if (on == fmpac.sramOn) return;
  fmpac.sramOn = on;
  if (on) { fmpac.sram[0x1FFE] = 0x4D; fmpac.sram[0x1FFF] = 0x69; }   // 読むと鍵の値が見える
  PROF_COUNT(PN_BANK);
  fmpacUpdatePages();
  updateMemoryMap();
}

static void fmpacWrite(uint16_t a, uint8_t v) {
  if (a < 0x4000 || a >= 0x8000) return;
  uint16_t o = a & 0x3FFF;
  switch (o) {
    case 0x1FFE: case 0x1FFF:
      if (!(fmpac.enable & 0x10)) {
        if (o == 0x1FFE) fmpac.r1ffe = v; else fmpac.r1fff = v;
        fmpacCheckSram();
      }
      return;
    case 0x3FF4: case 0x3FF5:
      msx2FmWrite(o & 1, v);
      return;
    case 0x3FF6:
      fmpac.enable = v & 0x11;
      if (fmpac.enable & 0x10) { fmpac.r1ffe = fmpac.r1fff = 0; fmpacCheckSram(); }
      return;
    case 0x3FF7:
      if ((v & 3) != fmpac.bank) {
        fmpac.bank = v & 3;
        PROF_COUNT(PN_BANK);
        fmpacUpdatePages();
        updateMemoryMap();
      }
      return;
    default:
      if (fmpac.sramOn && o < 0x1FFE && fmpac.sram[o] != v) { fmpac.sram[o] = v; fmpacSramWrites++; }
      return;
  }
}
uint32_t msx2FmPacSramWrites() { return fmpacSramWrites; }

void msx2SetFmPac(uint8_t *rom, uint32_t size, uint8_t *sram8k) {
  fmpac.rom = rom;
  fmpac.sram = sram8k;
  uint32_t banks = size >> 14;
  fmpac.bankMask = (uint8_t)(banks > 1 ? banks - 1 : 0);
}

static void MSX2_FAST writeSpecial(uint16_t a, uint8_t v) {
  int page = a >> 13;
  int sh = (page >> 1) * 2;
  int prim = (psr >> sh) & 3;
  if (a == 0xFFFF && expanded[prim]) {
    ssr[prim] = v;
    updateMemoryMap();
    return;
  }
  uint8_t *p = msx2_wr_map[page];
  if (p) { p[a & 0x1FFF] = v; return; }
  if (prim == 2 && fmpac.rom) { fmpacWrite(a, v); return; }
  if (prim == 1 || prim == 2) { cartWrite(prim, a, v); return; }
  if (diskVisible && a >= 0x7FF8 && a <= 0x7FFF) msx2FdcWrite(a & 7, v);
}

static inline void writeByte(uint16_t a, uint8_t v) {
  uint8_t *p = msx2_wr_map[a >> 13];
  if (MSX2_LIKELY(p != nullptr && a != 0xFFFF)) { p[a & 0x1FFF] = v; return; }
  writeSpecial(a, v);
}

// ---------------------------------------------------------------------------
//  I/O
// ---------------------------------------------------------------------------
static uint8_t MSX2_FAST readIO(uint8_t p) {
  switch (p) {
    case 0x98: case 0x99: case 0x9A: case 0x9B: {
#if MSX2_PROFILE
      uint32_t t0 = msx2ProfClock();                 // VDPコマンドの時間(cmd)もこの中に入る
      uint8_t v = vdpReadPort(p & 3);
      profVioCyc += msx2ProfClock() - t0;
#else
      uint8_t v = vdpReadPort(p & 3);
#endif
      // S#0/S#1 を読んでフラグが消えたら INT もすぐ下がる。次のラインまで要求を残すと、
      // 割込み処理の終わりの EI で、もう原因の無い割込みをもう一度受け付けてしまう(ソーサリアンの曲が速くなる)
      if (MSX2_UNLIKELY(msx2_cpu.IRequest == INT_IRQ) && !vdpIrq()) msx2_cpu.IRequest = INT_NONE;
      return v;
    }
    case 0xA2:
      if (psgAddr == 14) {
        int port = (psgReg[15] >> 6) & 1;
        if (mousePort == port + 1) return mouseRead(msx2_joy[port]);
        return msx2_joy[port];
      }
      return psgReg[psgAddr];
    case 0xA8: return psr;
    case 0xA9: return msx2_key_matrix[ppiC & 15];
    case 0xAA: return ppiC;
    case 0xB5: return rtcRead();
    case 0xD9: case 0xDB: {                          // 漢字ROM (D9h=第1水準, DBh=第2水準)
      int n = (p == 0xDB);
      uint32_t a = kanjiAddr[n] + (n ? 0x20000 : 0);
      uint8_t v = (kanjiFont && a < kanjiSize) ? kanjiFont[a] : 0xFF;
      kanjiAddr[n] = (kanjiAddr[n] & ~0x1F) | ((kanjiAddr[n] + 1) & 0x1F);
      return v;
    }
    case 0xF4: return f4reg;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
      return mapReg[p & 3] | (uint8_t)~(MSX2_MAPPER_SEGMENTS - 1);
    default:
      return 0xFF;
  }
}

static void MSX2_FAST writeIO(uint8_t p, uint8_t v) {
  switch (p) {
    case 0x98: case 0x99: case 0x9A: case 0x9B:
#if MSX2_PROFILE
    {
      uint32_t t0 = msx2ProfClock();
      vdpWritePort(p & 3, v);
      profVioCyc += msx2ProfClock() - t0;
    }
#else
      vdpWritePort(p & 3, v);
#endif
      if (MSX2_UNLIKELY(msx2_cpu.IRequest == INT_IRQ) && !vdpIrq()) msx2_cpu.IRequest = INT_NONE;   // IE0/IE1 を切ったとき
      return;
    case 0xA0: psgAddr = v & 15; return;
    case 0xA1:
      if (psgAddr == 15 && mousePort && ((psgReg[15] ^ v) & (mousePort == 1 ? 0x10 : 0x20))) mouseStrobe();   // マウスの 8 番ピン
      psgReg[psgAddr] = v;
      msx2PsgWrite(psgAddr, v);
      return;
    case 0xA8: psr = v; updateMemoryMap(); return;
    case 0xAA: ppiC = v; return;
    case 0xAB:                                       // PPI ポートCのビット単位セット/リセット
      if (!(v & 0x80)) {
        uint8_t bit = 1 << ((v >> 1) & 7);
        ppiC = (v & 1) ? (ppiC | bit) : (ppiC & ~bit);
      }
      return;
    case 0x7C: case 0x7D:                            // 内蔵MSX-MUSIC、または FM-PAC で 7FF6h bit0=1
      if (fmEnabled || (fmpac.rom && (fmpac.enable & 1))) msx2FmWrite(p & 1, v);
      return;
    case 0xB4: rtcAddr = v & 15; return;
    case 0xB5: rtcWrite(v); return;
    case 0xD8: kanjiAddr[0] = (kanjiAddr[0] & 0x1F800) | ((v & 0x3F) << 5); return;
    case 0xD9: kanjiAddr[0] = (kanjiAddr[0] & 0x007E0) | ((uint32_t)(v & 0x3F) << 11); return;
    case 0xDA: kanjiAddr[1] = (kanjiAddr[1] & 0x1F800) | ((v & 0x3F) << 5); return;
    case 0xDB: kanjiAddr[1] = (kanjiAddr[1] & 0x007E0) | ((uint32_t)(v & 0x3F) << 11); return;
    case 0xF4:                                       // openMSX の ResetStatusRegister と同じ動作
      f4reg = f4Inverted ? (v | 0x7F) : ((f4reg & 0x20) | (v & 0xA0));
      return;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
      mapReg[p & 3] = v;
      mapperUpdatePages();
      updateMemoryMap();
      return;
    default:
      return;
  }
}

// ---------------------------------------------------------------------------
//  Z80 接続
// ---------------------------------------------------------------------------
extern "C" {
byte MSX2_FAST RdZ80(zword a)         { return readByte(a); }
void MSX2_FAST WrZ80(zword a, byte v) { writeByte(a, v); }
// Z80.c は普通のメモリを表から直接読み書きし、表が nullptr のところ(FDC・FM-PAC・カートリッジの
// バンク切り替え・拡張スロットレジスタなど)と FFFFh への書き込みだけここを呼ぶ
byte MSX2_FAST RdZ80Slow(zword a)         { return readSpecial(a); }
void MSX2_FAST WrZ80Slow(zword a, byte v) { writeSpecial(a, v); }
byte MSX2_FAST InZ80(zword port)      { return readIO((uint8_t)port); }
void MSX2_FAST OutZ80(zword port, byte v) { writeIO((uint8_t)port, v); }
void PatchZ80(Z80 *R) { (void)R; }
}

int vdpLineCycle() {
  int c = CPU_CYCLES_PER_LINE - msx2_cpu.ICount;
  if (c < 0) c = 0;
  return c * MSX2_CYCLES_PER_LINE / CPU_CYCLES_PER_LINE;
}

void msx2SetCpuCycles(int n) {
  if (n < 100) n = 100;
  cpuCyclesPerLine = n;
  msx2_cpu.IPeriod = n;
}
int msx2GetCpuCycles() { return cpuCyclesPerLine; }

#if MSX2_PROFILE
// ---------------------------------------------------------------------------
//  処理時間の内訳: フレームごとの値を msx2ProfReport まで積算する
// ---------------------------------------------------------------------------
// run=msx2RunFrame 全体。spr/out は vdp の内数、vio/map は z80 の内数、cmd は vio の内数
enum { PT_RUN, PT_VDP, PT_SPR, PT_OUT, PT_Z80, PT_VIO, PT_CMD, PT_MAP, PT_COUNT };
static struct {
  uint32_t frames;
  uint64_t sum[PT_COUNT];
  uint32_t max[PT_COUNT];
  uint32_t worst[PT_COUNT];                            // run が最大だったフレームの内訳
  uint64_t opCycles[16];
  uint32_t opCount[16];
  uint32_t n[PN_COUNT];
  uint32_t vramW, vramR, data, lines, sprLines, sprVis;
  uint32_t modeLines[32];                              // 画面モードごとの描いたライン数
  uint32_t hsLines, adjLines;                          // 横スクロール(R#26/27) / 画面位置調整(R#18) が効いたライン数
  uint64_t dots;
  uint32_t cmdMax;                                     // いちばん長かったコマンド1つ
  uint16_t cmdMaxNx, cmdMaxNy;
  uint8_t  cmdMaxCmd;
} profW;

static void profClear() {
  memset(&profW, 0, sizeof(profW));
  memset(profN, 0, sizeof(profN));
  profMapCyc = profVioCyc = 0;
  memset(vdpProf(), 0, sizeof(VdpProf));
}

static MSX2_PROF_COLD void profEndFrame(uint32_t run, uint32_t vdp, uint32_t z80) {
  VdpProf *v = vdpProf();
  uint32_t cmd = 0;
  for (int i = 0; i < 16; i++) {
    cmd += v->cmdCycles[i];
    profW.opCycles[i] += v->cmdCycles[i];
    profW.opCount[i] += v->cmdCount[i];
  }
  const uint32_t t[PT_COUNT] = { run, vdp, v->sprCycles, v->outCycles, z80, profVioCyc, cmd, profMapCyc };
  for (int i = 0; i < PT_COUNT; i++) {
    profW.sum[i] += t[i];
    if (t[i] > profW.max[i]) profW.max[i] = t[i];
  }
  if (run >= profW.worst[PT_RUN]) memcpy(profW.worst, t, sizeof(t));
  if (v->cmdMax > profW.cmdMax) {
    profW.cmdMax = v->cmdMax;
    profW.cmdMaxCmd = v->cmdMaxCmd;
    profW.cmdMaxNx = v->cmdMaxNx;
    profW.cmdMaxNy = v->cmdMaxNy;
  }
  for (int i = 0; i < PN_COUNT; i++) profW.n[i] += profN[i];
  profW.vramW += v->vramWrites;
  profW.vramR += v->vramReads;
  profW.data += v->cmdData;
  profW.lines += v->lines;
  profW.sprLines += v->sprLines;
  profW.sprVis += v->sprVis;
  for (int i = 0; i < 32; i++) profW.modeLines[i] += v->modeLines[i];
  profW.hsLines += v->hsLines;
  profW.adjLines += v->adjLines;
  profW.dots += v->cmdDots;
  profW.frames++;
  memset(v, 0, sizeof(*v));
  memset(profN, 0, sizeof(profN));
  profMapCyc = profVioCyc = 0;
}

static void profAppend(char *buf, int size, int *pos, const char *fmt, ...) {
  if (*pos >= size - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf + *pos, size - *pos, fmt, ap);
  va_end(ap);
  if (r > 0) *pos = (*pos + r < size - 1) ? *pos + r : size - 1;
}

// 1フレームあたりの回数 n/frames を有効数字3桁くらいで(10未満は小数2桁、100未満は小数1桁)
static const char *profRate(char *s, uint64_t n, uint64_t frames) {
  uint64_t c = n * 100 / frames;
  if (c < 1000)       snprintf(s, 24, "%u.%02u", (unsigned)(c / 100), (unsigned)(c % 100));
  else if (c < 10000) snprintf(s, 24, "%u.%u", (unsigned)(c / 100), (unsigned)(c / 10 % 10));
  else                snprintf(s, 24, "%u", (unsigned)(c / 100));
  return s;
}

int msx2ProfReport(char *buf, int size, uint32_t clockPerUs) {
  if (!buf || size <= 0) return 0;
  buf[0] = 0;
  uint64_t cpu = clockPerUs ? clockPerUs : 1;
  uint64_t f = profW.frames ? profW.frames : 1;
  unsigned avg[PT_COUNT], mx[PT_COUNT], worst[PT_COUNT];
  for (int i = 0; i < PT_COUNT; i++) {
    avg[i] = (unsigned)(profW.sum[i] / (f * cpu));
    mx[i] = (unsigned)(profW.max[i] / cpu);
    worst[i] = (unsigned)(profW.worst[i] / cpu);
  }
  static const char *const opName[16] = { "STOP", "op1", "op2", "op3", "POINT", "PSET", "SRCH", "LINE",
                                          "LMMV", "LMMM", "LMCM", "LMMC", "HMMV", "HMMM", "YMMM", "HMMC" };
  int pos = 0;
  profAppend(buf, size, &pos,
             "PROF us avg/max (%uf): run %u/%u vdp %u/%u (spr %u/%u out %u/%u) z80 %u/%u"
             " (vio %u/%u cmd %u/%u map %u/%u) | worst: vdp %u spr %u z80 %u vio %u cmd %u | longest cmd: ",
             (unsigned)profW.frames, avg[PT_RUN], mx[PT_RUN], avg[PT_VDP], mx[PT_VDP], avg[PT_SPR], mx[PT_SPR],
             avg[PT_OUT], mx[PT_OUT], avg[PT_Z80], mx[PT_Z80], avg[PT_VIO], mx[PT_VIO], avg[PT_CMD], mx[PT_CMD],
             avg[PT_MAP], mx[PT_MAP],
             worst[PT_VDP], worst[PT_SPR], worst[PT_Z80], worst[PT_VIO], worst[PT_CMD]);
  if (profW.cmdMax)                                    // R#46 の値/LOP NX×NY 時間
    profAppend(buf, size, &pos, "%s/%X %ux%u %uus\n", opName[profW.cmdMaxCmd >> 4],
               (unsigned)(profW.cmdMaxCmd & 15), (unsigned)profW.cmdMaxNx, (unsigned)profW.cmdMaxNy,
               (unsigned)(profW.cmdMax / cpu));
  else
    profAppend(buf, size, &pos, "-\n");
  char a[24], b[24], c[24], d[24], e[24], g[24], h[24], k2[24], l[24];
  // halt = Z80 が HALT のまま始まったライン数(262 から引くと動いていたライン数), ln = 表示オンで描いたライン数
  // spl = スプライトを重ねたライン数, spv = 表示したスプライトの延べ数(1ライン1枚で1)
  profAppend(buf, size, &pos, "PROF n/frame: 98h wr %s rd %s int %s halt %s bank %s map %s ln %s spl %s spv %s | cmd n(us):",
             profRate(a, profW.vramW, f), profRate(b, profW.vramR, f), profRate(c, profW.n[PN_IRQ], f),
             profRate(g, profW.n[PN_HALT], f), profRate(d, profW.n[PN_BANK], f), profRate(e, profW.n[PN_MAP], f),
             profRate(l, profW.lines, f), profRate(h, profW.sprLines, f), profRate(k2, profW.sprVis, f));
  // コマンドの区分 (CMD 上位4bit)。other = STOP/POINT/PSET/SRCH/LMCM
  static const char *const names[8] = { "HMMM", "LMMM", "HMMV", "LMMV", "YMMM", "LINE", "HMMC/LMMC", "other" };
  static const uint16_t ops[8] = { 1 << 0xD, 1 << 0x9, 1 << 0xC, 1 << 0x8, 1 << 0xE, 1 << 0x7,
                                   (1 << 0xF) | (1 << 0xB), 0x007F | (1 << 0xA) };
  for (int k = 0; k < 8; k++) {
    uint64_t n = 0, cyc = 0;
    for (int op = 0; op < 16; op++)
      if (ops[k] & (1 << op)) { n += profW.opCount[op]; cyc += profW.opCycles[op]; }
    profAppend(buf, size, &pos, " %s %s(%u)", names[k], profRate(a, n, f), (unsigned)(cyc / (f * cpu)));
  }
  profAppend(buf, size, &pos, " | dots %u data %s\n", (unsigned)(profW.dots / f), profRate(a, profW.data, f));
  // 画面モードごとの1フレームあたりのライン数(ライン割込みで途中から切り替えるソフトの確認用)
  profAppend(buf, size, &pos, "PROF lines/frame by mode:");
  for (int m = 0; m < 32; m++) {
    if (!profW.modeLines[m]) continue;
    const char *nm = nullptr;
    switch (m) {
      case 0x00: nm = "G1(SC1)"; break;  case 0x01: nm = "T1(SC0w40)"; break; case 0x02: nm = "MC(SC3)"; break;
      case 0x04: nm = "G2(SC2)"; break;  case 0x08: nm = "G3(SC4)"; break;    case 0x09: nm = "T2(SC0w80)"; break;
      case 0x0C: nm = "G4(SC5)"; break;  case 0x10: nm = "G5(SC6)"; break;    case 0x14: nm = "G6(SC7)"; break;
      case 0x1C: nm = "G7(SC8)"; break;  default: break;
    }
    if (nm) profAppend(buf, size, &pos, " %s %s", nm, profRate(a, profW.modeLines[m], f));
    else    profAppend(buf, size, &pos, " m%02X %s", m, profRate(a, profW.modeLines[m], f));
  }
  profAppend(buf, size, &pos, " | hscroll(R26/27) %s adjust(R18) %s\n", profRate(a, profW.hsLines, f),
             profRate(b, profW.adjLines, f));
  memset(&profW, 0, sizeof(profW));
  return pos;
}
#endif

// ---------------------------------------------------------------------------
//  リセット / 実行
// ---------------------------------------------------------------------------
void msx2Reset() {
  memset(emptyPage, 0xFF, sizeof(emptyPage));
  for (int i = 0; i < 16; i++) msx2_key_matrix[i] = 0xFF;
  psr = 0;
  memset(ssr, 0, sizeof(ssr));
  // BIOSと同じ初期値(ページ0〜3にセグメント3,2,1,0)
  mapReg[0] = 3; mapReg[1] = 2; mapReg[2] = 1; mapReg[3] = 0;
  mapperUpdatePages();
  for (int s = 1; s <= 2; s++) { cart[s].sccActive = false; if (cart[s].rom) cartUpdatePages(s); }
  memset(sccWave, 0, sizeof(sccWave));
  mousePhase = 3;
  mouseLastX = msx2_mouse_x; mouseLastY = msx2_mouse_y;
  fmpac.enable = 0; fmpac.bank = 0; fmpac.r1ffe = fmpac.r1fff = 0; fmpac.sramOn = false;
  fmpacUpdatePages();
  kanjiAddr[0] = kanjiAddr[1] = 0;
  f4reg = f4Inverted ? 0xFF : 0x00;
  ppiC = 0;
  psgAddr = 0;
  memset(psgReg, 0, sizeof(psgReg));
  rtcInit();
  rtcFrames = 0;
  updateMemoryMap();

  vdpReset();
  memset(&msx2_cpu, 0, sizeof(msx2_cpu));
  ResetZ80(&msx2_cpu);
  msx2_cpu.IPeriod = CPU_CYCLES_PER_LINE;
  msx2_cpu.IAutoReset = 1;                           // 受け付けたら IRequest を下げる
#if MSX2_PROFILE
  profClear();
#endif
}

static uint32_t lineClock;
uint32_t msx2LineClock() { return lineClock; }

void MSX2_FAST msx2RunFrame() {
#if MSX2_PROFILE
  // 1ラインにつきカウンタを2回読む: vdp = vdpLine、z80 = 割込み判定 + ExecZ80 (I/O処理と VDPコマンドを含む)
  uint32_t tFrame = msx2ProfClock(), t0 = tFrame, vdpCycles = 0, z80Cycles = 0;
#endif
  for (int line = 0; line < MSX2_LINES_PER_FRAME; line++) {
    lineClock++;
    vdpLine(line);
#if MSX2_PROFILE
    uint32_t t1 = msx2ProfClock();
    vdpCycles += t1 - t0;
#endif
    // INT はレベル信号。VDPのフラグ(S#0/S#1を読むと消える)が立っている間は要求し続ける。
    if (vdpIrq()) {
      msx2_cpu.IRequest = INT_IRQ;
      if (msx2_cpu.IFF & IFF_1) {
        IntZ80(&msx2_cpu, INT_IRQ);
        PROF_COUNT(PN_IRQ);
      }
    } else {
      msx2_cpu.IRequest = INT_NONE;
    }
#if MSX2_PROFILE
    bool pending = (msx2_cpu.IRequest == INT_IRQ);  // DI 中の要求。EI のあと ExecZ80 の中で受け付けると下がる
    if (msx2_cpu.IFF & IFF_HALT) PROF_COUNT(PN_HALT);   // このラインは HALT で待つだけ
#endif
    ExecZ80(&msx2_cpu, CPU_CYCLES_PER_LINE);
#if MSX2_PROFILE
    if (pending && msx2_cpu.IRequest != INT_IRQ) PROF_COUNT(PN_IRQ);
    t0 = msx2ProfClock();
    z80Cycles += t0 - t1;
#endif
  }
  if (++rtcFrames >= 60) { rtcFrames = 0; rtcTickSecond(); }
#if MSX2_PROFILE
  profEndFrame(msx2ProfClock() - tFrame, vdpCycles, z80Cycles);
#endif
}

uint8_t msx2PrimarySlots() { return psr; }
uint8_t msx2SubSlots(int prim) { return ssr[prim & 3]; }
