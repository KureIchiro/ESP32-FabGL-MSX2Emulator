/*
 * ============================================================================
 *  MSX2+ Emulator for ESP32 (Work in Progress)
 * ============================================================================
 *
 * [ Credits & Acknowledgments ]
 *  - Graphics/Audio Engine: FabGL by Fabrizio Di Vittorio.
 *  - Z80 CPU Core: Modified based on the work by Marat Fayzullin.
 *  - FM Sound Unit (optional): "emu2413" by Mitsutaka Okazaki (MIT License).
 *  Special thanks to all authors for providing these excellent foundations.
 *
 * [ Development Environment ]
 *  - Board Manager: ESP32 by Espressif Systems (v2.0.5 recommended)
 *  - Library: FabGL v1.0.9
 *
 * [ Recommended Arduino IDE Settings ]
 *  - PSRAM: "Enabled"
 *  - Partition Scheme: "Huge APP" or similar
 *  - ESP32 Dev Module
 *
 * [ Disclaimer ]
 *  IN NO EVENT SHALL FABRIZIO DI VITTORIO, MARAT FAYZULLIN, MITSUTAKA OKAZAKI,
 *  OR THE DEVELOPER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, OR
 *  CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.
 *  USE AT YOUR OWN RISK.
 * ============================================================================
 *
 *  構成
 *   msx2.cpp   : スロット / メモリマッパー / I/O / RTC / 漢字ROM / 1フレーム実行
 *   v9958.cpp  : VDP (ビューポートへ1ラインずつ直接描く)
 *   wd2793.cpp : FDD (ソニー型 7FF8h-7FFFh)
 *   MSX2.ino   : ESP32固有の部分 (画面・SD・キーボード・音)
 *   host/      : PCでコアを動かして確認するためのテスト環境 (Arduinoはビルドしない)
 *
 *  FM音源(MSX-MUSIC)は emu2413.c / emu2413.h をこのフォルダに置くと有効になる。
 *  SDカードのルートに FMPAC.ROM があると FM-PAC をスロット2に挿した構成になり、
 *  内蔵MSX-MUSIC(MSX2PMUS.ROM)は使わない(FM-PAC にしか対応しないソフト向け)。
 *
 *  起動メニュー: Slot1 / Slot2 / Disk BIOS / Disk1 / Disk2 / Mouse / Joy B1〜B3 / Run を ↑↓ で選び Enter。
 *   ポート1のコントローラでも操作できる(↑↓ 選択、←→ 1画面送り、ボタン1 = 決定、ボタン2 = 取り消し)。
 *   Slot2 の既定は FM-PAC(FMPAC.ROM があるとき)。ほかのROMを選ぶと内蔵MSX-MUSICになる。
 *   FM-PAC の SRAM は SD の /FMPAC.PAC に自動で保存する(書き込みが止まって2秒後)。
 *   Slot1/Slot2 の <SCC> は ROM の無い SCC カートリッジ(SD スナッチャーなどの音源。RAM は無し)。
 *   Mouse: NONE / PORT 1 / PORT 2(PS/2 マウスをそのジョイスティックポートにつなぐ)。
 *    マウスをつないだポートのコントローラはキーボードとして働く: 方向 = カーソルキー、
 *    ボタン1〜3 = Joy B1〜B3 で決めたキー(Mouse = NONE のときは働かない)。
 *   Joy B1〜B3: そのボタンで押すキー(Enter で黄色になり、次に押したキーを割り当てる。
 *    F6〜F12 で取り消し、BackSpace で NONE。SD の /JOYKEY.CFG に保存。PC-88 版の Joy B1〜B3 と同じ操作)。
 *   Disk BIOS は、カートリッジだけで起動するときは自動で OFF(Enter で手動切替)。
 *  ROM は SD の /ROM、ディスク(.DSK)は /DISK から選ぶ。中のフォルダにも入れる
 *   (フォルダは「名前/」で表示。Enter で入る、「../」か BackSpace で戻る、PageUp/PageDown で1画面送り)。
 *  ROMのマッパーはファイル名のタグで決める: (A8)/(A) (A16) (K) (KS) (R)
 *   (K) コナミ(SCCあり/なし両用)  (KS) コナミSCC(実機どおりのアドレスのみ。メタルギア2など)
 *  実行中: F12 一時停止 / Shift+F12 リセット / 一時停止中に F10・F11 でドライブ1・2のディスク
 *   入れ替え(Shift で書き戻し) / F8 PSGをSCC波形で鳴らす /
 *   F6 Z80 の速さ Normal(205) → High(285) → Super(365)(1ラインのクロック数。画面下に0.5秒表示)
 *   ※ドライブ2は起動メニューで Disk2 を選んで起動したときだけつながる(実機の HB-F1XDJ と
 *     同じく、選ばなければ1ドライブで、B: は A: を使う仮想ドライブになる)
 */

#pragma GCC optimize("O3")

#include <SPI.h>
#include <SD.h>
#include <fabgl.h>
#include <vector>
#include <algorithm>
#include <WiFi.h>
#include "soc/rtc_wdt.h"
#include "esp_task_wdt.h"
#include "msx2.h"
#include "v9958.h"
#include "wd2793.h"
#include "Z80.h"
#include "KeyboardSystem.hpp"
#include "JoyKeys.hpp"

#if __has_include("emu2413.h")
  #include "emu2413.h"
  #define MSX2_HAS_FM 1
#endif

//Please select one of the following options.
#define KEYBORD_MAP (KeyboardLayout::JAPANESE)
//#define KEYBORD_MAP (KeyboardLayout::US_ENGLISH)
//#define KEYBORD_MAP (KeyboardLayout::PORTUGUESE)
//#define KEYBORD_MAP (KeyboardLayout::SPANISH)
//#define KEYBORD_MAP (KeyboardLayout::ITALIAN)

// 5秒ごとに1フレームの処理時間をシリアルに出す(開発中の確認用)
// msx2_config.h の MSX2_PROFILE が 1 なら、続けてその内訳(PROF 2行)も出す。
// これを消すときは MSX2_PROFILE も 0 にする(数えるだけで出さないことになるため)。
//#define MSX2_PERF_LOG

// TTGO VGA32
#define SD_SCLK 14
#define SD_MISO 2
#define SD_MOSI 12
#define SD_CS 13

// 画面: 液晶モニタはアナログ信号を、自分が思っている解像度(同期の周波数から決める)の
// 画素数で1ラインを標本化する。こちらの1ドットがその整数個に当たらないと、ドットによって
// 拾われる回数が変わり、細い縦線が所々太る(512ドットの画面で目立つ)。
//
// ■ MSX2_FULLHD 1 (推奨・16:9モニタ): 1920x1080@60Hz (CEA: 148.5MHz 2200x1125, 67.5kHz,
//   +H +V) と同じ同期で出す。モニタはパネルと同じ 1920x1080 として受けるので拡大縮小もなく、
//   「引き延ばす/しない」の設定にも左右されない。
//     横: ドットクロック 49.41MHz = 1ドットをモニタの3画素で拾う → 512ドット = 1536画素(80%)
//         合計 732ドット(2200/3=733.3 に一番近い4の倍数)。誤差0.18%は画面全体で約1ドットのずれ。
//         表示512 / フロント92 / 同期16 / バック112 (4の倍数) で左右中央
//     縦: 1ラインを5回くり返す → 212×5 = 1060ライン(98%)。合計 225×5 = 1125
//         表示212 / フロント3 / 同期1 / バック9 (5ライン単位) で上下中央
//     画面の形: 1536×1060 = 1.45:1 (実機のTVでは約1.38:1。4:3は1.33:1)
//   モニタが VGA 入力で 1920x1080 を受け付けないときは 0 にする。
//
// ■ MSX2_FULLHD 0: 標準VGA 640x480@60Hz と同じ同期(31.47kHz / 60Hz)。どのモニタでも映る。
//   モニタは1ラインを640回で拾うので、512ドットをくっきり出すには1ドット=1画素(25.175MHz)
//   しかなく、それだと横が80%で細長くなる。形を優先してドットクロックを変えているため、
//   512ドットの画面では縦線が所々太る。
//   垂直(DoubleScan単位): 表示212 / フロント5+14 / 同期1 / バック16+14 = 262 → 縦の占有率 0.883
//     MSX2_WIDE_MONITOR 1: 16:9 モニタで 640x480 を全画面に引き伸ばす(設定できないモニタ用)
//       30.46MHz: 表示512 / フロント152 / 同期116 / バック188 = 968 (横の占有率0.661 → 4:3)
//     MSX2_WIDE_MONITOR 0: 4:3 モニタ(または比率保持表示)
//       22.78MHz: 表示512 / フロント48 / 同期88 / バック76 = 724 (横の占有率0.883 → 4:3)
#define MSX2_FULLHD 0                 // 1: 1920x1080 の同期で出す(16:9 モニタ推奨。2026-10-07 実機で映らず)  0: 640x480 の同期
#define MSX2_WIDE_MONITOR 0           // MSX2_FULLHD 0 のとき  1: 16:9 モニタ(全画面引き伸ばし)  0: 4:3 モニタ
#if MSX2_WIDE_MONITOR
  #define MSX2_MODELINE_VGA "\"512x212@60Hz\" 30.46 512 664 780 968 212 231 232 262 -HSync -VSync DoubleScan"
#else
  #define MSX2_MODELINE_VGA "\"512x212@60Hz\" 22.78 512 560 648 724 212 231 232 262 -HSync -VSync DoubleScan"
#endif
// 1920x1080 用。縦の5回くり返しは書式に無いので、読み込んだあと scanCount を 5 にする。
#define MSX2_MODELINE_FHD "\"512x212@60Hz\" 49.41 512 604 620 732 212 215 216 225 +HSync +VSync"
#define MSX2_FHD_SCAN 5

// VDP: 0 = BIOS に合わせる(MSX2P.ROM なら V9958、MSX2.ROM なら V9938)  1 = V9938 固定  2 = V9958 固定
// BIOS は /MSX2P.ROM + /MSX2PEXT.ROM (MSX2+) を探し、無ければ /MSX2.ROM + /MSX2EXT.ROM (MSX2)、
// それも無ければ C-BIOS(MSX2+)。V9938 にすると、VDP の種類を見てMSX2+用の処理に切り替える
// ソフト(スペースマンボウなど)も MSX2 用の処理(R#18 での横スクロールなど)で動く。
#define MSX2_VDP 0

// 内部RAMはこの量を空けておく(SDライブラリやタスクのスタック用)
#define INTERNAL_RESERVE (40 * 1024)

// 起動メニューで選んだ内容
static const char *ITEM_NONE  = "<NONE>";          // ファイル選択の「なし」
static const char *ITEM_FMPAC = "<FM-PAC>";        // スロット2に FM-PAC を挿す
static const char *ITEM_SCC   = "<SCC>";           // ROM の無い SCC カートリッジ(SD スナッチャーの音源の代わり)
struct BootConfig {
  String slot[2];                                  // "" = 空き。ITEM_SCC、slot[1] は ITEM_FMPAC もあり
  String disk[2];                                  // "" = 入れない
  bool diskBios;
  bool diskBiosManual;                             // 手で切り替えたら、以後は自動で決めない
  int  mouse;                                      // 0 = なし、1 / 2 = ジョイスティックポート1 / 2 に PS/2 マウス
};
static uint8_t *fmpacSram;                         // FM-PAC の SRAM(SD の /FMPAC.PAC に保存する)
static bool mouseOn = false;                       // PS/2 マウスを読む
static int mousePortSel = 0;                       // マウスをつないだポート(0 = なし)。そのポートのコントローラはキーを押す(JoyKeys.hpp)

fabgl::VGAController DisplayController;
fabgl::SoundGenerator soundGenerator;
fabgl::PS2Controller PS2Controller;

static void printHeap(const char *tag) {
  Serial.printf("HEAP[%-11s] free=%6u largest=%6u psram=%u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// 内部RAMに余裕があれば内部、なければPSRAM
static uint8_t *allocFast(size_t n, bool *internal = nullptr) {
  uint8_t *p = nullptr;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) >= n + INTERNAL_RESERVE)
    p = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (internal) *internal = (p != nullptr);
  if (!p) p = (uint8_t *)ps_malloc(n);
  return p;
}

// ---------------------------------------------------------------------------
//  画面 / UI (ビューポートへ直接描く。4バイト内は px2,px3,px0,px1 なので x^2)
// ---------------------------------------------------------------------------
static uint8_t *vpRow[MSX2_SCREEN_H];      // ビューポートの各行
static uint8_t *vdpRows[MSX2_SCREEN_H];    // VDPが書く先(ステータス表示中はその行を退避先へ)
static uint8_t *scratchRow;                // ステータス表示中にVDPが書く捨て行
static bool vpReady = false;

// 生の画素値の上2ビットは同期信号。負極性(640x480)なら C0h、正極性(1920x1080)なら 00h が
// 「同期なし」なので、画面モードを決めたあと uiSetSync() で合わせる。
static uint8_t UI_BLACK = 0xC0;
static uint8_t UI_WHITE = 0xFF;
static uint8_t UI_BLUE  = 0xF0;
static uint8_t UI_CYAN  = 0xFD;
static uint8_t UI_YELLOW = 0xCF;
static void uiSetSync(uint8_t sync) {
  UI_BLACK = sync;
  UI_WHITE = sync | 0x3F;
  UI_BLUE  = sync | 0x30;
  UI_CYAN  = sync | 0x3D;
  UI_YELLOW = sync | 0x0F;
}
static constexpr int STATUS_Y = MSX2_SCREEN_H - 9;

namespace fabgl { extern const FontInfo FONT_8x8; }

static void uiFillRect(int x1, int y1, int x2, int y2, uint8_t col) {
  if (!vpReady) return;
  for (int y = y1; y <= y2 && y < MSX2_SCREEN_H; y++) {
    if (y < 0) continue;
    uint8_t *row = vpRow[y];
    for (int x = x1; x <= x2 && x < MSX2_SCREEN_W; x++)
      if (x >= 0) row[x ^ 2] = col;
  }
}

static void uiDrawChar(int x, int y, char ch, uint8_t fg, uint8_t bg) {
  if (!vpReady) return;
  const fabgl::FontInfo *f = &fabgl::FONT_8x8;
  uint8_t c = (uint8_t)ch;
  if (c < 32 || c > 127) c = '?';
  const uint8_t *glyph = f->chptr ? f->data + pgm_read_dword(f->chptr + c) : f->data + c * f->height;
  for (int r = 0; r < 8; r++) {
    if (y + r < 0 || y + r >= MSX2_SCREEN_H) continue;
    uint8_t bits = pgm_read_byte(glyph + r);
    uint8_t *dst = vpRow[y + r];
    for (int col = 0; col < f->width; col++) {
      int ax = x + col;
      if (ax >= 0 && ax < MSX2_SCREEN_W) dst[ax ^ 2] = (bits & (0x80 >> col)) ? fg : bg;
    }
  }
}

static void uiText(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
  while (s && *s) { uiDrawChar(x, y, *s++, fg, bg); x += 8; }
}

static void clearStatus() {
  for (int r = STATUS_Y; r < STATUS_Y + 8; r++) vdpRows[r] = vpRow[r];   // VDPに返す
}

static uint32_t statusClearAt;                     // timedStatus を消す時刻(millis。0 = なし)

void flashStatus(const char *msg, int waitMs = 0) {
  if (!vpReady) return;
  statusClearAt = 0;
  for (int r = STATUS_Y; r < STATUS_Y + 8; r++) vdpRows[r] = scratchRow; // VDPに上書きさせない
  uiFillRect(0, STATUS_Y, MSX2_SCREEN_W - 1, STATUS_Y + 7, UI_BLACK);
  uiText(8, STATUS_Y, msg, UI_WHITE, UI_BLACK);
  if (waitMs > 0) { delay(waitMs); clearStatus(); }
}

// 画面の下に ms ミリ秒だけ出す(待たない。loop() で消す)
static void timedStatus(const char *msg, int ms) {
  flashStatus(msg);
  statusClearAt = millis() + ms;
  if (!statusClearAt) statusClearAt = 1;
}

// ---------------------------------------------------------------------------
//  音: PSG / SCC (MSX1版と同じ) / FM
// ---------------------------------------------------------------------------
static const int BUFFER_SIZE = 256;       // 256 / 16384Hz = 約15.6ms (60FPSの約1フレーム)

int8_t scc_waveform[5][32] = { 0 };
uint16_t scc_f[] = { 0, 0, 0, 0, 0, 0 };  // PSGの各chを2つのSCC chに割り当てるため6ch
uint8_t scc_v[] = { 0, 0, 0, 0, 0, 0 };
uint8_t scc_enable;
bool psg2scc = false;
const uint8_t SCC_VOL_TABLE[16] = { 0, 2, 4, 7, 10, 15, 21, 26, 33, 39, 47, 54, 62, 68, 73, 75 };

// PSGあり:オープニングのメインリード用: 偶数倍音のみのノコギリ状(PSG矩形波と合成するとノコギリ波に近づく)
uint8_t SCC_Ys2_Lead[32] = {
  0x00, 0x13, 0x26, 0x39, 0x4A, 0x60, 0x76, 0x5D, 0x00, 0xA3, 0x8A, 0xA0, 0xB6, 0xC7, 0xDA, 0xED,
  0x00, 0x13, 0x26, 0x39, 0x4A, 0x60, 0x76, 0x5D, 0x00, 0xA3, 0x8A, 0xA0, 0xB6, 0xC7, 0xDA, 0xED
};

// PSGあり:ベース・低音パート用: ほぼ2fの正弦波+4fを少しだけ(PSGの低音に芯と輪郭を足す)
uint8_t SCC_Ys2_Bass[32] = {
  0x00, 0x1A, 0x34, 0x50, 0x69, 0x76, 0x6A, 0x40, 0x00, 0xC0, 0x96, 0x8A, 0x97, 0xB0, 0xCC, 0xE6,
  0x00, 0x1A, 0x34, 0x50, 0x69, 0x76, 0x6A, 0x40, 0x00, 0xC0, 0x96, 0x8A, 0x97, 0xB0, 0xCC, 0xE6
};

// バッキング(和音・アルペジオ): デューティ25%のパルス波。4倍音が無く、PSGの矩形波より少し太い
// 倍音比 1 : 0.69 : 0.31 : 0 : 0.16 : 0.17 ...
uint8_t SCC_Ys2_Back[32] = {
  0x29, 0x6D, 0x7C, 0x79, 0x7B, 0x79, 0x7C, 0x6D, 0x29, 0xE5, 0xD5, 0xD8, 0xD7, 0xD8, 0xD7, 0xD7,
  0xD7, 0xD7, 0xD7, 0xD7, 0xD7, 0xD7, 0xD7, 0xD7, 0xD7, 0xD7, 0xD7, 0xD8, 0xD7, 0xD8, 0xD5, 0xE5
};

class SCCGenerator : public fabgl::WaveformGenerator {
private:
  int16_t sampleBuffer[BUFFER_SIZE];
  volatile int head = 0, tail = 0;
  int lastSample = 0;
  int8_t waveform[32];
  uint32_t phase = 0, phaseInc = 0;
  uint8_t curVol = 0;

public:
  int getSample() override {
    if (head == tail) return lastSample;
    lastSample = sampleBuffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return lastSample;
  }
  void setFrequency(int value) override {}
  void updateParams(int8_t *newWave, uint32_t freq, uint8_t vol) {
    phaseInc = freq ? (uint32_t)((111860 / (freq + 1)) << 18) : 0;
    curVol = vol;
    memcpy(waveform, newWave, 32);
  }
  void fillBuffer() {
    int nextHead = (head + 1) % BUFFER_SIZE;
    while (nextHead != tail) {
      sampleBuffer[head] = calculateNextSample();
      head = nextHead;
      nextHead = (head + 1) % BUFFER_SIZE;
    }
  }

private:
  inline int16_t calculateNextSample() {
    if (curVol == 0 || phaseInc == 0) return 0;
    int8_t s = waveform[(phase >> 27) & 0x1F];
    phase += phaseInc;
    return (int16_t)((int)s * curVol >> 8);
  }
};
SCCGenerator *scc_ch[5];

void IRAM_ATTR updateSCCSound() {
  for (int i = 0; i < 5; i++) {
    if ((scc_enable & (1 << i)) && scc_f[i] > 10) {
      int waveIdx = (i < 4) ? i : 3;
      scc_ch[i]->updateParams((int8_t *)scc_waveform[waveIdx], scc_f[i], SCC_VOL_TABLE[scc_v[i]]);
    } else {
      scc_ch[i]->updateParams((int8_t *)scc_waveform[0], 0, 0);
    }
  }
}

class AY38910Generator : public fabgl::WaveformGenerator {
private:
  const uint32_t psgClock = 111860;
  uint32_t tickAccum = 0, tickStep = 0;
  volatile uint32_t tonePeriod[3] = { 1, 1, 1 };
  volatile uint32_t noisePeriod = 1, envPeriod = 1;
  volatile uint8_t envShape = 0;
  volatile uint8_t regs[16] = { 0 };
  volatile bool holding = true;
  volatile bool cont = false, alt = false, hold = false, att = false;
  volatile uint32_t envCounter = 0, envStep = 0, envVol = 0;
  uint32_t toneCounter[3] = { 0, 0, 0 };
  uint32_t toneOut = 0, noiseCounter = 0, lfsr = 1, noiseOut = 0;
  int16_t volTable[16] = { 0, 5, 10, 15, 20, 26, 32, 40, 48, 58, 68, 78, 90, 102, 114, 127 };
  int16_t sampleBuffer[BUFFER_SIZE];
  volatile int head = 0, tail = 0;
  int lastSample = 0;

public:
  void writeReg(uint8_t reg, uint8_t val) {
    if (reg > 13) return;
    regs[reg] = val;
    switch (reg) {
      case 0: case 1: { uint32_t p = regs[0] | ((regs[1] & 0x0F) << 8); tonePeriod[0] = p ? p : 1; break; }
      case 2: case 3: { uint32_t p = regs[2] | ((regs[3] & 0x0F) << 8); tonePeriod[1] = p ? p : 1; break; }
      case 4: case 5: { uint32_t p = regs[4] | ((regs[5] & 0x0F) << 8); tonePeriod[2] = p ? p : 1; break; }
      case 6: noisePeriod = (regs[6] & 0x1F) ? (regs[6] & 0x1F) : 1; break;
      case 11: case 12: { uint32_t p = regs[11] | (regs[12] << 8); envPeriod = p ? p : 1; break; }
      case 13:
        envShape = val & 0x0F;
        envStep = 0; envCounter = 0;
        cont = envShape & 0x08; att = envShape & 0x04; alt = envShape & 0x02; hold = envShape & 0x01;
        envVol = att ? 0 : 15;
        holding = false;
        break;
    }
  }
  void fillBuffer() {
    if (tickStep == 0) {
      int rate = sampleRate();
      if (rate > 0) setSampleRate(rate); else return;
    }
    int nextHead = (head + 1) % BUFFER_SIZE;
    while (nextHead != tail) {
      sampleBuffer[head] = calculateNextSample();
      head = nextHead;
      nextHead = (head + 1) % BUFFER_SIZE;
    }
  }
  void setSampleRate(int rate) {
    this->fabgl::WaveformGenerator::setSampleRate(rate);
    tickStep = (uint32_t)(((uint64_t)psgClock << 16) / (rate > 0 ? rate : 16384));
  }

private:
  inline int calculateNextSample() {
    uint32_t r7 = regs[7], r8 = regs[8], r9 = regs[9], r10 = regs[10];
    uint32_t tp0 = tonePeriod[0], tp1 = tonePeriod[1], tp2 = tonePeriod[2], np = noisePeriod;
    tickAccum += tickStep;
    uint32_t ticks = tickAccum >> 16;
    tickAccum &= 0xFFFF;
    while (ticks--) {
      toneCounter[0] += 2; while (toneCounter[0] >= tp0) { toneCounter[0] -= tp0; toneOut ^= 1; }
      toneCounter[1] += 2; while (toneCounter[1] >= tp1) { toneCounter[1] -= tp1; toneOut ^= 2; }
      toneCounter[2] += 2; while (toneCounter[2] >= tp2) { toneCounter[2] -= tp2; toneOut ^= 4; }
      if (++noiseCounter >= np) {
        noiseCounter = 0;
        lfsr = (lfsr >> 1) | (((lfsr & 1) ^ ((lfsr >> 3) & 1)) << 16);
        noiseOut = lfsr & 1;
      }
      if (!holding && ++envCounter >= envPeriod) { envCounter = 0; stepEnvelope(); }
    }
    uint32_t mix = (toneOut | r7) & ((noiseOut ? 0x07 : 0) | (r7 >> 3));
    int volA = volTable[(r8 & 0x10) ? envVol : (r8 & 0x0F)];
    int volB = volTable[(r9 & 0x10) ? envVol : (r9 & 0x0F)];
    int volC = volTable[(r10 & 0x10) ? envVol : (r10 & 0x0F)];
    // ノイズだけのchは控えめにする
    if ((r7 & 0x09) == 0x01) volA >>= 1;
    if ((r7 & 0x12) == 0x02) volB >>= 1;
    if ((r7 & 0x24) == 0x04) volC >>= 1;
    int s = 0;
    if (mix & 1) s += volA;
    if (mix & 2) s += volB;
    if (mix & 4) s += volC;
    return (s >> 1) - 95;
  }
  int getSample() override {
    if (head == tail) return lastSample;
    lastSample = sampleBuffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return lastSample;
  }
  inline void stepEnvelope() {
    if (envStep < 15) {
      envStep++;
      envVol = att ? envStep : (15 - envStep);
    } else if (!cont) {
      holding = true; envVol = 0;
    } else if (hold) {
      holding = true;
      if (alt) att = !att;
      envVol = att ? 15 : 0;
    } else {
      envStep = 0;
      if (alt) att = !att;
      envVol = att ? 0 : 15;
    }
  }
  void setFrequency(int value) override {}
};
AY38910Generator psgGen;

#ifdef MSX2_HAS_FM
// MSX-MUSIC (YM2413)。Mark III版と同じく、FMに一度も書き込まれていない間は合成しない
// (合成は1サンプル約12us、16384Hzでコア1の約20%を使う)。
class YM2413Generator : public fabgl::WaveformGenerator {
private:
  OPLL *opll = nullptr;
  int16_t sampleBuffer[BUFFER_SIZE];
  volatile int head = 0, tail = 0;
  int lastSample = 0;
  static const int LP_SHIFT = 2;           // 1次IIRローパス
  // FabGL のミキサーは各chの和に (PSG100+FM100 なら) 約0.32倍を掛けて、そのまま
  // 8bit で DAC に出す(上限で止めないので ±127 を超えると折り返して爆音のノイズになる)。
  // emu2413 は 1ch 最大 ±4094、全ch+リズムで ±25000 程度。>>6 で FM 1ch と PSG 1ch が
  // ほぼ同じ音量になり、±250 で止めれば PSG 最大と重なっても ±110 に収まる。
  static const int FM_SHIFT = 6;
  static const int FM_CLIP  = 250;
  int32_t lpState = 0;
  struct FMCommand { uint8_t adr, val; };
  static const int CMD_QUEUE_SIZE = 128;
  FMCommand cmdQueue[CMD_QUEUE_SIZE];
  volatile int cmdHead = 0, cmdTail = 0;

public:
  volatile bool touched = false;
  YM2413Generator() { opll = OPLL_new(3579545, 16384); }
  void writePort(uint8_t isData, uint8_t val) {   // コア0(Z80)から呼ばれる
    touched = true;
    int n = (cmdHead + 1) % CMD_QUEUE_SIZE;
    if (n != cmdTail) { cmdQueue[cmdHead] = { isData, val }; cmdHead = n; }
  }
  void fillBuffer() {                              // コア1から呼ばれる
    if (!opll || !touched) return;
    while (cmdTail != cmdHead) {
      FMCommand c = cmdQueue[cmdTail];
      OPLL_writeIO(opll, c.adr, c.val);
      cmdTail = (cmdTail + 1) % CMD_QUEUE_SIZE;
    }
    int nextHead = (head + 1) % BUFFER_SIZE;
    while (nextHead != tail) {
      int32_t raw = OPLL_calc(opll);
      lpState += (raw - lpState) >> LP_SHIFT;     // シフト前にかけて小音量の量子化ノイズを避ける
      int32_t s = lpState >> FM_SHIFT;
      if (s > FM_CLIP) s = FM_CLIP; else if (s < -FM_CLIP) s = -FM_CLIP;
      sampleBuffer[head] = (int16_t)s;
      head = nextHead;
      nextHead = (head + 1) % BUFFER_SIZE;
    }
  }
  int getSample() override {
    if (head == tail) return lastSample;
    lastSample = sampleBuffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return lastSample;
  }
  void setFrequency(int value) override {}
};
YM2413Generator *fmGen = nullptr;
#endif

// ---- コアからのフック ----
void msx2PsgWrite(int reg, uint8_t v) {
  psgGen.writeReg(reg, v);
  if (!psg2scc) return;
  switch (reg) {                                  // PSG の音をSCCの波形で鳴らす(F8)
    case 0: case 1: case 2: case 3: case 4: case 5: {
      int ch = reg / 2;
      if (reg % 2 == 0) scc_f[ch] = (scc_f[ch] & 0x0F00) | v;
      else scc_f[ch] = (scc_f[ch] & 0x00FF) | ((v & 0x0F) << 8);
      scc_f[ch + 3] = (scc_f[ch] >> 1) - 3;
      break;
    }
    case 8: case 9: case 10:
      scc_v[reg - 8] = v & 0x0F;
      scc_v[reg - 8 + 3] = scc_v[reg - 8] >> 1;
      break;
    case 7:
      scc_enable = (~v) & 0x07;
      break;
  }
}

void msx2SccWrite(uint16_t a, uint8_t v) {
  if (a < 0x9880) {
    scc_waveform[(a - 0x9800) >> 5][a & 0x1F] = (int8_t)((int)v - 128);
  } else if (a < 0x98E0) {
    int reg = a & 0x0F;
    if (reg < 0x0A) {
      int ch = reg >> 1;
      if (reg & 1) scc_f[ch] = (scc_f[ch] & 0x00FF) | ((v & 0x0F) << 8);
      else scc_f[ch] = (scc_f[ch] & 0x0F00) | v;
    } else if (reg < 0x0F) {
      scc_v[reg - 0x0A] = v & 0x0F;
    } else {
      scc_enable = v & 0x1F;
    }
  }
}

void msx2FmWrite(int port, uint8_t v) {
#ifdef MSX2_HAS_FM
  if (fmGen) fmGen->writePort(port, v);
#endif
}

// ---------------------------------------------------------------------------
//  ROM / ディスク
// ---------------------------------------------------------------------------
static uint8_t *loadFile(const char *path, uint32_t *size, bool fast) {
  File f = SD.open(path);
  if (!f) return nullptr;
  uint32_t n = f.size();
  bool internal = false;
  uint8_t *p = fast ? allocFast(n, &internal) : (uint8_t *)ps_malloc(n);
  if (p) {
    uint32_t done = 0;
    while (done < n) {
      int r = f.read(p + done, (n - done) > 4096 ? 4096 : (n - done));
      if (r <= 0) break;
      done += r;
    }
    Serial.printf("ROM %-24s %7u bytes (%s)\n", path, (unsigned)n, internal ? "internal" : "PSRAM");
  }
  f.close();
  if (size) *size = n;
  return p;
}

static uint8_t *diskBuf[2];
static char diskPath[2][128];
static uint32_t diskSize[2];
static const uint32_t DISK_MAX = 720 * 1024;

static bool loadDisk(int d, const String &path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint32_t n = f.read(diskBuf[d], DISK_MAX);
  f.close();
  if (n == 0) return false;
  strncpy(diskPath[d], path.c_str(), sizeof(diskPath[d]) - 1);
  diskSize[d] = n;
  wd2793SetDisk(d, diskBuf[d], n);
  return true;
}

static void saveDisk(int d) {
  if (!diskBuf[d] || !diskPath[d][0] || !diskSize[d]) { flashStatus("NO DISK", 1500); return; }
  File f = SD.open(diskPath[d], FILE_WRITE);
  if (f) {
    f.write(diskBuf[d], diskSize[d]);
    f.close();
    flashStatus("SAVE DISK PSRAM->SD OK", 1500);
  } else {
    flashStatus("SAVE DISK PSRAM->SD NG", 1500);
  }
}

static void clearKeyBuffer() {
  auto kb = PS2Controller.keyboard();
  while (kb->scancodeAvailable() > 0) { kb->getNextScancode(0); delay(10); }
}

// dirPath(/ROM や /DISK)の中からファイルを選ぶ。サブフォルダにも入れる。
//   フォルダは「名前/」で一覧の上のほうに出る。Enter で入り、「../」か BackSpace で1つ上へ戻る。
//   返す値は dirPath からの相対パス(例 "Ys2/YsII_Game Disk.dsk")。
// extra: 一番上のフォルダの一覧の先頭に足す固定の項目(起動メニューの「なし」「FM-PAC」など)。選ぶとその文字列を返す。
// current: 最初にカーソルを置く項目(相対パス。フォルダ付きならそのフォルダから始める)。ESC で抜けたときは "" を返す。
static String showFileSelector(const char *title, const char *dirPath, const char *ext,
                               const std::vector<String> &extra = {}, const String &current = "") {
  enum { K_EXTRA, K_UP, K_DIR, K_FILE };
  struct Item { String name; int kind; };
  auto kb = PS2Controller.keyboard();
  auto byName = [](const Item &a, const Item &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
  String lext = String(ext);
  lext.toLowerCase();

  String sub = "", want = current;                 // sub: 今のフォルダ(dirPath からの相対。先頭・末尾の / なし)
  bool isExtra = false;
  for (const String &e : extra) if (e == current) isExtra = true;
  int slash = current.lastIndexOf('/');
  if (!isExtra && slash >= 0) { sub = current.substring(0, slash); want = current.substring(slash + 1); }

  for (;;) {                                       // フォルダを移るたびに一覧を作り直す
    String path = String(dirPath) + (sub == "" ? "" : "/" + sub);
    File dir = SD.open(path.c_str());
    if (!dir && sub != "") { sub = ""; want = ""; continue; }   // 前に選んだフォルダが無くなっていた
    if (!dir) { SD.mkdir(dirPath); dir = SD.open(dirPath); }
    std::vector<Item> dirs, files;
    while (dir) {
      File file = dir.openNextFile();
      if (!file) break;
      String name = file.name();
      int s = name.lastIndexOf('/');                // 版によってはフルパスが返る
      if (s >= 0) name = name.substring(s + 1);
      if (name.length() && name[0] != '.' && name != "System Volume Information") {
        if (file.isDirectory()) dirs.push_back({ name, K_DIR });
        else {
          String lower = name;
          lower.toLowerCase();
          if (lower.endsWith(lext)) files.push_back({ name, K_FILE });
        }
      }
      file.close();
    }
    if (dir) dir.close();
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    std::vector<Item> items;
    if (sub == "") for (const String &e : extra) items.push_back({ e, K_EXTRA });
    else items.push_back({ "..", K_UP });
    items.insert(items.end(), dirs.begin(), dirs.end());
    items.insert(items.end(), files.begin(), files.end());
    if (items.empty()) { uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLACK); return ""; }

    const int maxRows = (MSX2_SCREEN_H - 40) / 9;
    int cursor = 0, top = 0;
    for (int i = 0; i < (int)items.size(); i++)
      if (items[i].name == want) { cursor = i; break; }
    bool isBreak = false, redraw = true, changeDir = false;
    // 画面全体を塗るのはフォルダに入ったときだけ。カーソルを動かしたら前と今の2行、
    // スクロールしたら一覧の行だけ描き直す(画面の行に直接書くので、全部塗り直すとちらつく)
    int shownTop = -1, shownCursor = -1;
    auto drawItem = [&](int idx) {
      int y = 30 + (idx - top) * 9;
      uiFillRect(56, y - 1, MSX2_SCREEN_W - 1, y + 7, UI_BLUE);
      if (idx < 0 || idx >= (int)items.size()) return;
      const Item &it = items[idx];
      String label = (it.kind == K_UP || it.kind == K_DIR) ? it.name + "/" : it.name;
      if (label.length() > 54) label = label.substring(0, 54);
      uiText(64, y, label.c_str(), idx == cursor ? UI_YELLOW : (it.kind == K_FILE || it.kind == K_EXTRA) ? UI_WHITE : UI_CYAN, UI_BLUE);   // 選択中=黄、フォルダ=水色
    };
    while (!changeDir) {
      if (redraw) {
        if (cursor < top) top = cursor;
        if (cursor >= top + maxRows) top = cursor - maxRows + 1;
        if (shownTop < 0) {
          uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLUE);
          uiText(48, 5, title, UI_WHITE, UI_BLUE);
          String shown = path + "/";
          if (shown.length() > 56) shown = "..." + shown.substring(shown.length() - 53);
          uiText(48, 16, shown.c_str(), UI_CYAN, UI_BLUE);
        }
        if (shownTop < 0 || top != shownTop) {
          for (int i = 0; i < maxRows; i++) drawItem(top + i);
        } else if (cursor != shownCursor) {
          drawItem(shownCursor);
          drawItem(cursor);
        }
        shownTop = top;
        shownCursor = cursor;
        redraw = false;
      }
      while (!changeDir) {
        // キーボード、なければポート1のコントローラ(JoyKeys.hpp)
        int sc;
        if (kb && kb->scancodeAvailable() > 0) {
          sc = kb->getNextScancode(0);
          if (sc == 0xF0) { isBreak = true; continue; }
          if (sc == 0xE0) continue;
        } else {
          bool e0;
          sc = joyMenuScancode(joyMenuPoll(), &e0);
          if (!sc) break;
        }
        if (!isBreak) {
          int n = (int)items.size();
          if (sc == 0x75 && cursor > 0) { cursor--; redraw = true; }
          else if (sc == 0x72 && cursor < n - 1) { cursor++; redraw = true; }
          else if (sc == 0x7D) { cursor = cursor - maxRows < 0 ? 0 : cursor - maxRows; redraw = true; }        // PageUp
          else if (sc == 0x7A) { cursor = cursor + maxRows >= n ? n - 1 : cursor + maxRows; redraw = true; }   // PageDown
          else if (sc == 0x76) { uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLACK); clearKeyBuffer(); return ""; }
          else if ((sc == 0x66 && sub != "") || (sc == 0x5A && items[cursor].kind == K_UP)) {    // 1つ上へ
            int p = sub.lastIndexOf('/');
            want = p >= 0 ? sub.substring(p + 1) : sub;      // 出てきたフォルダにカーソルを置く
            sub = p >= 0 ? sub.substring(0, p) : "";
            changeDir = true;
          } else if (sc == 0x5A && items[cursor].kind == K_DIR) {                                // フォルダに入る
            sub = sub == "" ? items[cursor].name : sub + "/" + items[cursor].name;
            want = "";
            changeDir = true;
          } else if (sc == 0x5A) {
            const Item &it = items[cursor];
            String r = (it.kind == K_EXTRA || sub == "") ? it.name : sub + "/" + it.name;
            uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLACK);
            return r;
          }
        }
        isBreak = false;
      }
      if (!changeDir) delay(10);
    }
  }
}

static void handleDiskSelect(int d) {
  if (!diskBuf[d]) { flashStatus("NO DISK.ROM", 1000); return; }
  char title[40];
  sprintf(title, "--- SELECT DISK %d (ESC TO SKIP) ---", d + 1);
  String cur = strncmp(diskPath[d], "/DISK/", 6) == 0 ? String(diskPath[d] + 6) : String("");   // 今のディスクから選び始める
  String sel = showFileSelector(title, "/DISK", ".DSK", {}, cur);
  if (sel != "") flashStatus(loadDisk(d, "/DISK/" + sel) ? "LOAD SUCCESS" : "LOAD ERROR!", 1000);
}

static Msx2Mapper detectMapper(const char *filename, uint32_t size) {
  if (size <= 0x8000) return MSX2_MAP_NONE;
  const char *base = strrchr(filename, '/');       // フォルダ名のタグは見ない
  String up = String(base ? base + 1 : filename);
  up.toUpperCase();
  if (up.indexOf("(A16)") >= 0) return MSX2_MAP_ASCII16;
  if (up.indexOf("(A8)") >= 0 || up.indexOf("(A)") >= 0) return MSX2_MAP_ASCII8;
  if (up.indexOf("(KS)") >= 0) return MSX2_MAP_KONAMI_SCC;
  if (up.indexOf("(K)") >= 0) return MSX2_MAP_KONAMI;
  if (up.indexOf("(R)") >= 0) return MSX2_MAP_RTYPE;
  return MSX2_MAP_NONE;
}

// slot = 1 / 2
static bool loadCartridge(const String &path, int slot) {
  static uint8_t *loaded[3];                       // 起動メニューで読み直したときに前のぶんを返す
  File f = SD.open(path.c_str());
  if (!f) return false;
  uint32_t size = f.size();
  uint32_t rs = 0x2000;
  while (rs < size) rs <<= 1;
  if (loaded[slot]) { free(loaded[slot]); loaded[slot] = nullptr; }
  uint8_t *rom = (uint8_t *)ps_malloc(rs);
  if (!rom) { f.close(); return false; }
  loaded[slot] = rom;
  memset(rom, 0xFF, rs);                           // 2のべき乗に切り上げた残りは空きROM
  f.read(rom, size);
  f.close();
  Msx2Mapper m = detectMapper(path.c_str(), size);
  static const char *names[] = { "NONE", "KONAMI", "ASCII8", "ASCII16", "RTYPE", "KONAMI_SCC" };
  Serial.printf("Cartridge slot%d: %s (%u bytes) mapper=%s\n", slot, path.c_str(), (unsigned)size, names[m]);
#if MSX2_PROFILE
  // PSRAM の上位2MB(0x3FA00000〜)はコア1側のキャッシュを通る(デュアルコア時の LOWHIGH 配置)
  Serial.printf("PROF cart rom=%p-%p\n", rom, rom + rs);
#endif
  msx2SetCartridge(slot, rom, rs, m);
  return true;
}

// ---------------------------------------------------------------------------
//  起動メニュー
//    Slot1 / Slot2 / Disk BIOS / Disk1 / Disk2 / Mouse / Joy B3 / Run を ↑↓ で選び、Enter で変更・起動。
//    選んでいる項目は反転表示。
// ---------------------------------------------------------------------------
// (BootConfig の定義は、Arduino が関数の宣言を自動で入れる位置より前に置くため先頭にある)

// Disk BIOS の既定: カートリッジだけで起動するときは外す(ワークエリアを取られて動かない
// ゲームがあるため)。ディスクを入れたとき、何も挿さないとき(BASIC)は入れる。
static bool isRomItem(const String &s) { return s != "" && s != ITEM_FMPAC && s != ITEM_SCC; }
static void bootAutoDiskBios(BootConfig &c) {
  if (c.diskBiosManual) return;
  bool cart = isRomItem(c.slot[0]) || isRomItem(c.slot[1]);
  c.diskBios = !cart || c.disk[0] != "" || c.disk[1] != "";
}

// ジョイスティックのボタン1〜3のキー(PC-88 版の Joy B1〜B3 と同じ。SD の /JOYKEY.CFG に3バイト。
// 前の版の1バイトのファイルはボタン3のキーとして読む)
static const char *JOYKEY_FILE = "/JOYKEY.CFG";
static void loadJoyKey() {
  File f = SD.open(JOYKEY_FILE, FILE_READ);
  if (!f) return;
  uint8_t b[3];
  int n = f.read(b, 3);
  f.close();
  auto ok = [](uint8_t v) { return v == 0xFF || (v >> 4) <= 10; };
  if (n == 1 && ok(b[0])) joyKey[2] = b[0];
  else if (n == 3) for (int i = 0; i < 3; i++) if (ok(b[i])) joyKey[i] = b[i];
}
static void saveJoyKey() {
  File f = SD.open(JOYKEY_FILE, FILE_WRITE);
  if (!f) return;
  f.write(joyKey, 3);
  f.close();
}

// 起動メニューの各行に出ている内容(選択の状態 + 文字)を覚えておき、変わった行だけ描き直す。
// 画面の行に直接書くので、毎回全部塗り直すとちらつく。0〜9 = 項目、10 = 案内、11 = メッセージ
static char menuShown[12][72];
static bool menuValid = false;
static void bootMenuInvalidate() { menuValid = false; }   // ファイル選択などで画面を上書きしたあと

// capture: カーソルの Joy B1〜B3 の行がキーを待っている(黄色で表示)
static void bootMenuDraw(const BootConfig &c, int cursor, bool hasDiskRom, const char *msg, bool capture) {
  static const int X0 = 40, X1 = MSX2_SCREEN_W - 41, Y0 = 40, DY = 14;
  if (!menuValid) {
    uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLUE);
    uiText(X0, 14, "MSX2+ BOOT MENU", UI_WHITE, UI_BLUE);
    memset(menuShown, 0, sizeof(menuShown));
    menuValid = true;
  }
  for (int i = 0; i < 10; i++) {
    char line[64];
    auto name = [](const String &s) { return s == ITEM_FMPAC ? String("FM-PAC") : s == ITEM_SCC ? String("SCC (NO ROM)") : s; };
    static const char *mouseName[3] = { "NONE", "PORT 1", "PORT 2" };
    switch (i) {
      case 0: snprintf(line, sizeof(line), "Slot1    : %.46s", name(c.slot[0]).c_str()); break;
      case 1: snprintf(line, sizeof(line), "Slot2    : %.46s", name(c.slot[1]).c_str()); break;
      case 2: snprintf(line, sizeof(line), "Disk BIOS: %s", !hasDiskRom ? "OFF (NO DISK.ROM)" : c.diskBios ? "ON" : "OFF"); break;
      case 3: snprintf(line, sizeof(line), "Disk1    : %.46s", c.disk[0].c_str()); break;
      case 4: snprintf(line, sizeof(line), "Disk2    : %.46s", c.disk[1].c_str()); break;
      case 5: snprintf(line, sizeof(line), "Mouse    : %s", mouseName[c.mouse]); break;
      case 6: case 7: case 8:
        snprintf(line, sizeof(line), "Joy B%d   : %s", i - 5, (capture && i == cursor) ? "PRESS A KEY..." : msxKeyName(joyKey[i - 6]));
        break;
      case 9: snprintf(line, sizeof(line), "Run"); break;
    }
    bool sel = (i == cursor);
    char shown[72];                                // 1 文字目 = 状態(N 通常 / S 選択 / C キー待ち)
    snprintf(shown, sizeof(shown), "%c%s", sel ? (capture ? 'C' : 'S') : 'N', line);
    if (strcmp(shown, menuShown[i]) == 0) continue;
    strcpy(menuShown[i], shown);
    int y = Y0 + i * DY;
    uint8_t bg = !sel ? UI_BLUE : capture ? UI_YELLOW : UI_WHITE;
    uiFillRect(X0 - 4, y - 3, X1, y + 10, bg);
    uiText(X0, y, line, sel ? UI_BLUE : UI_WHITE, bg);
  }
  const char *help = capture ? "PRESS THE KEY FOR THIS JOYSTICK BUTTON (F6-F12: CANCEL)"
                   : (cursor >= 6 && cursor <= 8) ? "ENTER: SET KEY   BACKSPACE: NONE   (FOR MOUSE PORT PAD)"
                                                  : "UP/DOWN: SELECT   ENTER: CHANGE / RUN";
  const char *texts[2] = { help, msg ? msg : "" };
  for (int k = 0; k < 2; k++) {
    if (strcmp(texts[k], menuShown[10 + k]) == 0) continue;
    snprintf(menuShown[10 + k], sizeof(menuShown[0]), "%s", texts[k]);
    int y = k ? MSX2_SCREEN_H - 16 : MSX2_SCREEN_H - 30;
    uiFillRect(X0 - 4, y - 1, MSX2_SCREEN_W - 1, y + 9, UI_BLUE);
    uiText(X0, y, texts[k], k ? UI_WHITE : UI_CYAN, UI_BLUE);
  }
}

// 選んだ内容で ROM / ディスクを読み込む。失敗したら理由を返す(メニューに戻る)。
static const char *bootMenuApply(const BootConfig &c) {
  for (int s = 0; s < 2; s++) {
    msx2SetCartridge(s + 1, nullptr, 0, c.slot[s] == ITEM_SCC ? MSX2_MAP_SCC : MSX2_MAP_NONE);
    if (isRomItem(c.slot[s]) && !loadCartridge("/ROM/" + c.slot[s], s + 1))
      return s ? "SLOT2 ROM LOAD ERROR" : "SLOT1 ROM LOAD ERROR";
  }
  for (int d = 0; d < 2; d++) {
    if (!diskBuf[d]) continue;
    if (c.disk[d] == "") { wd2793SetDisk(d, nullptr, 0); diskPath[d][0] = 0; diskSize[d] = 0; continue; }
    if (!loadDisk(d, "/DISK/" + c.disk[d])) return d ? "DISK2 LOAD ERROR" : "DISK1 LOAD ERROR";
  }
  return nullptr;
}

static void bootMenu(BootConfig &c, bool hasDiskRom, bool hasFmPac) {
  auto kb = PS2Controller.keyboard();
  // 選べる項目(ディスク関係は DISK.ROM があるときだけ)
  std::vector<int> items = { 0, 1 };
  if (hasDiskRom) { items.push_back(2); items.push_back(3); items.push_back(4); }
  items.push_back(5);
  items.push_back(6);
  items.push_back(7);
  items.push_back(8);
  items.push_back(9);
  int pos = 0;                                     // 最初は Slot1(↑ で一番下の Run へ回り込む)
  const char *msg = nullptr;
  bool isBreak = false, isE0 = false, redraw = true, capture = false;
  clearKeyBuffer();
  bootMenuInvalidate();
  for (;;) {
    if (redraw) { bootAutoDiskBios(c); bootMenuDraw(c, items[pos], hasDiskRom, msg, capture); redraw = false; }
    for (;;) {
      // キーボード、なければポート1のコントローラ(キーを割り当て中はキーボードだけ)
      int sc;
      bool e0;
      if (kb && kb->scancodeAvailable() > 0) {
        sc = kb->getNextScancode(0);
        if (sc == 0xF0) { isBreak = true; continue; }
        if (sc == 0xE0) { isE0 = true; continue; }
        e0 = isE0;
        isE0 = false;
        if (isBreak) { isBreak = false; continue; }
      } else if (!capture && (sc = joyMenuScancode(joyMenuPoll(), &e0)) != 0) {
      } else break;
      if (capture) {                               // ジョイスティックのボタンに割り当てるキー
        uint8_t rb = e0 ? key_map_e0[sc & 0xFF] : key_map[sc & 0xFF];
        if (rb != 0xFF) { joyKey[items[pos] - 6] = rb; saveJoyKey(); }   // MSX に無いキー(F6〜F12 など)なら取り消し
        capture = false;
        redraw = true;
        continue;
      }
      if (sc == 0x75)      { pos = (pos + (int)items.size() - 1) % (int)items.size(); redraw = true; }
      else if (sc == 0x72) { pos = (pos + 1) % (int)items.size(); redraw = true; }
      else if (sc == 0x66 && items[pos] >= 6 && items[pos] <= 8) { joyKey[items[pos] - 6] = 0xFF; saveJoyKey(); redraw = true; }   // BackSpace: なし
      else if (sc == 0x5A) {
        int it = items[pos];
        msg = nullptr;
        if (it == 0 || it == 1) {
          std::vector<String> extra = { ITEM_NONE, ITEM_SCC };
          if (it == 1 && hasFmPac) extra.push_back(ITEM_FMPAC);
          bootMenuInvalidate();                    // ファイル選択の画面で上書きされる
          String sel = showFileSelector(it ? "-- SELECT ROM FOR SLOT 2 --" : "-- SELECT ROM FOR SLOT 1 --",
                                        "/ROM", ".ROM", extra, c.slot[it] == "" ? String(ITEM_NONE) : c.slot[it]);
          if (sel != "") c.slot[it] = (sel == ITEM_NONE) ? "" : sel;
        } else if (it == 2) {
          c.diskBios = !c.diskBios;
          c.diskBiosManual = true;
        } else if (it == 5) {
          c.mouse = (c.mouse + 1) % 3;
        } else if (it == 3 || it == 4) {
          int d = it - 3;
          bootMenuInvalidate();
          String sel = showFileSelector(d ? "-- SELECT DISK FOR DRIVE 2 --" : "-- SELECT DISK FOR DRIVE 1 --",
                                        "/DISK", ".DSK", { ITEM_NONE }, c.disk[d] == "" ? String(ITEM_NONE) : c.disk[d]);
          if (sel != "") c.disk[d] = (sel == ITEM_NONE) ? "" : sel;
        } else if (it >= 6 && it <= 8) {
          capture = true;                          // 次に押したキーをそのボタンに割り当てる
        } else if (it == 9) {
          bootAutoDiskBios(c);
          bootMenuDraw(c, it, hasDiskRom, "LOADING...", false);
          msg = bootMenuApply(c);
          if (!msg) return;
        }
        clearKeyBuffer();
        redraw = true;
      }
    }
    delay(10);
  }
}

// ---------------------------------------------------------------------------
//  キーボード / ジョイスティック
// ---------------------------------------------------------------------------
static volatile bool isPaused = false;
static volatile bool frameReady = false;

// F6 で Z80 の速さを切り替える(1ラインに実行するクロック数。MSX1 版でも F6 は空いている)
static const int CPU_SPEEDS[3] = { 205, 285, 365 };
static const char *const CPU_SPEED_NAMES[3] = { "CPU: Normal", "CPU: High", "CPU: Super" };
static int cpuSpeed = 0;

static void updateKeyboard() {
  static bool isBreak = false, isE0 = false, isShift = false;
  static bool f6_held = false, f8_held = false, f10_held = false, f11_held = false, f12_held = false;
  auto kb = PS2Controller.keyboard();
  if (!kb) return;
  int count = kb->scancodeAvailable();
  while (count-- > 0) {
    int sc = kb->getNextScancode(0);
    if (sc == -1) break;
    if (sc == 0xF0) { isBreak = true; continue; }
    if (sc == 0xE0) { isE0 = true; continue; }
    if (sc == 0x12 || sc == 0x59) isShift = !isBreak;

    if (sc == 0x0A) {                                // F8: PSGをSCC波形で鳴らす
      if (!isBreak && !f8_held) {
        f8_held = true;
        psg2scc = !psg2scc;
        if (!psg2scc) scc_enable = 0;
        flashStatus(psg2scc ? "SCC ON" : "SCC OFF", 1000);
      } else if (isBreak) f8_held = false;
    } else if ((sc == 0x09 || sc == 0x78) && !isE0) { // F10/F11: (一時停止中) ドライブ1/2のディスク選択 / Shift:書き戻し
      int d = (sc == 0x78) ? 1 : 0;
      bool &held = d ? f11_held : f10_held;
      if (!isBreak && !held) {
        held = true;
        if (isPaused) {
          if (!isShift) {
            handleDiskSelect(d);
            flashStatus("PAUSED!", 0);
            // 選択画面がキーを離した信号を読んでしまうので、押したままの扱いを消す
            // (残すと次の F10/F11 が1回目だけ無視される)
            held = isShift = false;
            for (int r = 0; r < 12; r++) msx2_key_matrix[r] = 0xFF;
          }
          else saveDisk(d);
        }
      } else if (isBreak) held = false;
    } else if (sc == 0x0B && !isE0) {                // F6: Z80 の速さ Normal → High → Super → Normal
      if (!isBreak && !f6_held) {
        f6_held = true;
        cpuSpeed = (cpuSpeed + 1) % 3;
        msx2SetCpuCycles(CPU_SPEEDS[cpuSpeed]);
        timedStatus(CPU_SPEED_NAMES[cpuSpeed], 500);
      } else if (isBreak) f6_held = false;
    } else if (sc == 0x07) {                         // F12: 一時停止 / Shift:リセット
      if (!isBreak && !f12_held) {
        f12_held = true;
        if (isShift) ESP.restart();
        isPaused = !isPaused;
        if (isPaused) flashStatus("PAUSED!", 0); else clearStatus();
      } else if (isBreak) f12_held = false;
    }

    // KeyboardSystem.hpp の表で MSX のマトリクスへ(E0 付き: カーソル・Home・Ins・Del・End・
    // PageUp/Down・テンキーの / と Enter・右 Alt など)
    int row = -1, bit = -1;
    uint8_t res = isE0 ? key_map_e0[sc & 0xFF] : key_map[sc & 0xFF];
    if (res != 0xFF) { row = res >> 4; bit = res & 0x0F; }
    // 一時停止中は押下を伝えない(離したのは伝える。押しっぱなしで残らないように)
    if (row >= 0 && row < 12 && (!isPaused || isBreak)) {
      if (isBreak) msx2_key_matrix[row] |= (1 << bit);
      else msx2_key_matrix[row] &= ~(1 << bit);
    }
    isBreak = false;
    isE0 = false;
  }
}

// 外部の変換器からシリアル(IO39)で届くアタリ仕様ジョイスティック(MSX1版と同じ形式)
// 1バイト: bit7 = 2P、bit0〜5 = ↑↓←→ トリガA/B、bit6 = ボタン3(0 = 押下)
// マウスをつないだポートのコントローラは、ゲームには渡さずキーを押す(JoyKeys.hpp)
static void updateController() {
  uint8_t j1 = 0x7F, j2 = 0x7F;
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    if (b & 0x80) j2 = b & 0x7F; else j1 = b & 0x7F;
  }
  msx2_joy[0] = 0x40 | ((mousePortSel == 1 ? 0x7F : j1) & 0x3F);   // bit6=1: JIS配列
  msx2_joy[1] = 0x40 | ((mousePortSel == 2 ? 0x7F : j2) & 0x3F);
  //joyKeysApply(mousePortSel == 2 ? j2 : j1, mousePortSel != 0, msx2_key_matrix);
}

// ---------------------------------------------------------------------------
//  Z80 / VDP (コア0)
// ---------------------------------------------------------------------------
void IRAM_ATTR cpuTask(void *) {
  const int64_t frameUs = 16683;                     // NTSC 59.94Hz
  int64_t next = esp_timer_get_time();
#ifdef MSX2_PERF_LOG
  uint32_t sum = 0, mx = 0, over = 0, n = 0;
#if MSX2_PROFILE
  char prof[1024];                                   // msx2ProfReport の2行+1行。次のフレームから少しずつ送る
  int profLen = 0, profPos = 0;
#endif
#endif
  for (;;) {
    if (isPaused) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      next = esp_timer_get_time();
      continue;
    }
    next += frameUs;
    int64_t t0 = esp_timer_get_time();
    msx2RunFrame();
    int64_t now = esp_timer_get_time();
    frameReady = true;
#ifdef MSX2_PERF_LOG
    uint32_t us = (uint32_t)(now - t0);
    sum += us; if (us > mx) mx = us; if (us > frameUs) over++;
    if (++n == 300) {
      Serial.printf("FRAME us: avg=%u max=%u over=%u/300 (budget=%d)\n", sum / n, mx, over, (int)frameUs);
#if MSX2_PROFILE
      profLen = msx2ProfReport(prof, sizeof(prof), getCpuFrequencyMhz());   // 同じ300フレームの内訳
      {                                              // cpuTask のスタックの残り(最小値)と、この時点のVDPの設定
        const uint8_t *r = vdpState()->reg;
        int k = snprintf(prof + profLen, sizeof(prof) - profLen,
                         "PROF stack free %u B | vdp mode %02X lines %d R0-11 %02X %02X %02X %02X %02X %02X %02X %02X"
                         " %02X %02X %02X %02X R18 %02X R23 %02X R25 %02X R26 %02X R27 %02X\n",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL), vdpDisplayMode(), vdpDisplayLines(),
                         r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[18], r[23], r[25],
                         r[26], r[27]);
        if (k > 0) profLen = (profLen + k < (int)sizeof(prof) - 1) ? profLen + k : (int)sizeof(prof) - 1;
      }
      profPos = 0;
#endif
      sum = mx = over = n = 0;
    }
#if MSX2_PROFILE
    else if (profPos < profLen) {
      // Serial は送信バッファが無く、UART の FIFO(128バイト)に入りきらないぶんは送り終わるまで
      // 待たされる。1フレームに64バイト(115200bpsで約5.6ms)ずつなら、遅れを取り戻すために
      // フレームを続けて回しているときでも待たずに済む。
      int k = profLen - profPos;
      if (k > 64) k = 64;
      Serial.write((const uint8_t *)prof + profPos, k);
      profPos += k;
    }
#endif
#endif
    int64_t wait = next - now;
    if (wait > 0) {
      if (wait >= 1000) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
      while (esp_timer_get_time() < next) asm volatile("nop");
    } else if (wait < -frameUs * 4) {
      next = now;                                    // 大きく遅れたら追いかけない
    }
  }
}

// ---------------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------------
static void haltWithMessage(const char *msg) {
  Serial.println(msg);
  uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLUE);
  uiText(16, 90, msg, UI_WHITE, UI_BLUE);
  while (1) delay(1000);
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF);
  setCpuFrequencyMhz(240);
  disableCore0WDT();
  disableCore1WDT();
  rtc_wdt_protect_off();
  rtc_wdt_disable();
  esp_task_wdt_delete(NULL);
  delay(100);
  printHeap("boot");

  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1, fabgl::KbdMode::NoVirtualKeys);
  PS2Controller.keyboard()->begin(GPIO_NUM_33, GPIO_NUM_32, false, false);
  initKeyMap(KEYBORD_MAP);

  // ---- 画面: 内部RAMの大物より先に確保する ----
  // FabGLはビューポートを確保するとき内部RAMの最大連続空きを約40KB残そうとし、
  // それを割り込むと確保を打ち切る(X1で実測)。だからRAMより先に取る。
  printHeap("before VGA");
  DisplayController.begin();
#if MSX2_FULLHD
  {
    fabgl::VGATimings t;
    fabgl::VGABaseController::convertModelineToTimings(MSX2_MODELINE_FHD, &t);
    t.scanCount = MSX2_FHD_SCAN;
    DisplayController.setResolution(t, -1, -1, false);
  }
  // DMAの記述子が増えるぶん内部RAMを使う。ビューポートが取り切れなかったら 640x480 に戻す
  if (DisplayController.getViewPortHeight() < MSX2_SCREEN_H) {
    Serial.printf("1920x1080 mode: viewport %d rows only, falling back to 640x480\n", DisplayController.getViewPortHeight());
    DisplayController.setResolution(MSX2_MODELINE_VGA, -1, -1, false);
  }
#else
  DisplayController.setResolution(MSX2_MODELINE_VGA, -1, -1, false);
#endif
  printHeap("after VGA");
  for (int y = 0; y < MSX2_SCREEN_H; y++) {
    vpRow[y] = DisplayController.getScanline(y);
    if (!vpRow[y] || ((uintptr_t)vpRow[y] & 3)) {
      Serial.printf("VIEWPORT row %d is NULL or unaligned (%p)\n", y, vpRow[y]);
      while (1) delay(1000);
    }
    vdpRows[y] = vpRow[y];
  }
  vpReady = true;
  uint8_t sync = DisplayController.createRawPixel(fabgl::RGB222(0, 0, 0));
  uiSetSync(sync);
  Serial.printf("VIEWPORT: %dx%d row0=%p stride=%d rawBlack=%02X (C0: 640x480, 00: 1920x1080)\n",
                DisplayController.getViewPortWidth(), DisplayController.getViewPortHeight(),
                vpRow[0], (int)(vpRow[1] - vpRow[0]), sync);
  uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLACK);
  scratchRow = (uint8_t *)heap_caps_malloc(MSX2_SCREEN_W, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);

  // ---- RAM (メモリマッパー) ----
  // セグメント0(ページ3: スタック/ワークエリア)から順に内部RAMへ。
  int internalSegs = 0;
  for (int i = 0; i < MSX2_MAPPER_SEGMENTS; i++) {
    bool internal = false;
    uint8_t *p = (i < 4) ? allocFast(0x4000, &internal) : (uint8_t *)ps_malloc(0x4000);
    if (!p) haltWithMessage("RAM ALLOCATION FAILED");
    memset(p, 0, 0x4000);
    if (internal) internalSegs++;
    msx2SetRamSegment(i, p);
  }
  Serial.printf("RAM: %d KB mapper, %d segments in internal RAM\n", MSX2_MAPPER_SEGMENTS * 16, internalSegs);

  uint8_t *vram = (uint8_t *)ps_malloc(VDP_VRAM_SIZE);
  if (!vram) haltWithMessage("VRAM ALLOCATION FAILED");
  memset(vram, 0, VDP_VRAM_SIZE);
#if MSX2_PROFILE
  Serial.printf("PROF vram=%p\n", vram);
#endif
  vdpInit(vram, vdpRows, sync);
  printHeap("after RAM");

  // ---- 音 ----
  for (int i = 0; i < 32; i++) {
    scc_waveform[0][i] = SCC_Ys2_Lead[i];
    scc_waveform[1][i] = SCC_Ys2_Back[i];
    scc_waveform[2][i] = SCC_Ys2_Bass[i];
  }
  soundGenerator.attach(&psgGen);
  psgGen.enable(true);
  psgGen.setVolume(100);
  for (int i = 0; i < 5; i++) {
    scc_ch[i] = new SCCGenerator();
    soundGenerator.attach(scc_ch[i]);
    scc_ch[i]->enable(true);
    scc_ch[i]->setVolume(0);
  }
#ifdef MSX2_HAS_FM
  fmGen = new YM2413Generator();
  soundGenerator.attach(fmGen);
  fmGen->enable(true);
#endif
  soundGenerator.setVolume(64);
  soundGenerator.play(true);

  // ---- ROM ----
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI)) haltWithMessage("SD CARD NOT FOUND");

  uint32_t n;
  bool cbios = false;
  bool msx2Bios = false;                           // MSX2(+ でない)の BIOS
  uint8_t *bios = loadFile("/MSX2P.ROM", &n, true);
  if (!bios) {
    bios = loadFile("/MSX2.ROM", &n, true);
    msx2Bios = (bios != nullptr);
  }
  if (!bios) {
    // 実機のROMが無ければ C-BIOS (カートリッジだけ起動できるフリーのBIOS)
    bios = loadFile("/cbios_main_msx2+_jp.rom", &n, true);
    cbios = (bios != nullptr);
  }
  if (!bios) haltWithMessage("/MSX2P.ROM NOT FOUND");
  msx2SetBios(bios);
  // VDP: BIOS の機種番号(002Dh: 1=MSX2, 2=MSX2+)に合わせる。MSX2 の BIOS なら V9938 として動かす
#if MSX2_VDP == 1
  bool v9938 = true;
#elif MSX2_VDP == 2
  bool v9938 = false;
#else
  bool v9938 = (n > 0x2D && bios[0x2D] < 2);
#endif
  vdpSetV9938(v9938);
  Serial.printf("BIOS version byte %02X -> VDP %s\n", n > 0x2D ? bios[0x2D] : 0xFF, v9938 ? "V9938 (MSX2)" : "V9958 (MSX2+)");
  if (cbios) {
    // C-BIOS はスロット0が拡張なし・F4hが反転型の前提
    msx2SetSlotExpanded(0, false);
    msx2SetF4Inverted(true);
    if (uint8_t *p = loadFile("/cbios_logo_msx2+.rom", &n, false)) msx2SetRom(0, 0, 0x8000, p, n);
    if (uint8_t *p = loadFile("/cbios_sub.rom", &n, false)) msx2SetSubRom(p);
  } else {
    if (uint8_t *p = loadFile(msx2Bios ? "/MSX2EXT.ROM" : "/MSX2PEXT.ROM", &n, false)) msx2SetSubRom(p);
    else Serial.println(msx2Bios ? "Warning: /MSX2EXT.ROM (sub ROM) not found" : "Warning: /MSX2PEXT.ROM (sub ROM) not found");
    // 漢字BASIC は MSX2+ の機能なので、MSX2 の BIOS のときは載せない
    if (!msx2Bios)
      if (uint8_t *p = loadFile("/KNJBASIC.ROM", &n, false)) msx2SetKanjiBasic(p, n);
  }
  if (uint8_t *p = loadFile("/KANJI.ROM", &n, false)) msx2SetKanjiFont(p, n);

  // ---- 起動メニュー (スロット1/2・Disk BIOS・ドライブ1/2) ----
  uint32_t fmpacSize = 0;
  uint8_t *fmpacRom = loadFile("/FMPAC.ROM", &fmpacSize, false);
  uint8_t *diskRom = cbios ? nullptr : loadFile("/DISK.ROM", &n, false);
  if (diskRom) {
    diskBuf[0] = (uint8_t *)ps_malloc(DISK_MAX);
    diskBuf[1] = (uint8_t *)ps_malloc(DISK_MAX);
  }
  BootConfig boot;
  boot.slot[1] = fmpacRom ? ITEM_FMPAC : "";
  boot.diskBiosManual = false;
  boot.mouse = 0;
  loadJoyKey();
  Serial1.begin(115200, SERIAL_8N1, 39, -1);         // 起動メニューもポート1のコントローラで操作できる
  bootMenu(boot, diskRom && diskBuf[0] && diskBuf[1], fmpacRom != nullptr);

  // FM音源: スロット2が FM-PAC なら内蔵MSX-MUSICは載せない。
  // (FM-PAC の印 "PAC2OPLL" やカートリッジスロットしか探さない古いソフトでもFMが鳴る。
  //  内蔵MSX-MUSIC対応のソフトは全スロットから "OPLL" を探すので FM-PAC でも鳴る)
  // スロット2にほかのROMを挿したときは、FMが鳴るように内蔵MSX-MUSICを載せる。
  uint8_t *sram = (fmpacRom && boot.slot[1] == ITEM_FMPAC) ? (uint8_t *)ps_malloc(0x2000) : nullptr;
  if (sram) {
    memset(sram, 0xFF, 0x2000);
    loadFmPacSram(sram);
    fmpacSram = sram;
    msx2SetFmPac(fmpacRom, fmpacSize, sram);
    Serial.println("FM-PAC in slot 2 (internal MSX-MUSIC disabled)");
  } else {
    if (fmpacRom) free(fmpacRom);
    if (cbios) {
      if (uint8_t *p = loadFile("/cbios_music.rom", &n, false)) msx2SetMusicRom(p, 3, 3);
    } else {
      if (uint8_t *p = loadFile("/MSX2PMUS.ROM", &n, false)) msx2SetMusicRom(p);
    }
  }
  if (diskRom && boot.diskBios) msx2SetDiskRom(diskRom);
  mouseOn = boot.mouse != 0;
  mousePortSel = boot.mouse;
  msx2SetMousePort(boot.mouse);
  printHeap("end setup");

  wd2793Reset();
  msx2Reset();
  uiFillRect(0, 0, MSX2_SCREEN_W - 1, MSX2_SCREEN_H - 1, UI_BLACK);
  xTaskCreatePinnedToCore(cpuTask, "cpuTask", 8192, NULL, 24, NULL, 0);
  Serial.println(cbios ? "MSX2+ Booting (C-BIOS)..." : "MSX2+ Booting...");
#ifndef MSX2_PERF_LOG
  Serial.end();
#endif
}

// ---------------------------------------------------------------------------
//  FM-PAC の SRAM(8KB)を SD の /FMPAC.PAC に保存する(openMSX などと同じ形:
//  "PAC2 BACKUP DATA" の 16 バイトのあとに 1FFEh バイト)。書き込みが止まって 2 秒たったら保存。
// ---------------------------------------------------------------------------
static const char *FMPAC_SRAM_FILE = "/FMPAC.PAC";
static const char FMPAC_SRAM_HEADER[17] = "PAC2 BACKUP DATA";

static void loadFmPacSram(uint8_t *sram) {
  File f = SD.open(FMPAC_SRAM_FILE, FILE_READ);
  if (!f) return;
  char h[16];
  if (f.read((uint8_t *)h, 16) == 16 && memcmp(h, FMPAC_SRAM_HEADER, 16) == 0) {
    f.read(sram, 0x1FFE);
    Serial.println("FM-PAC SRAM loaded from " + String(FMPAC_SRAM_FILE));
  }
  f.close();
}

static void saveFmPacSramIfChanged() {
  static uint32_t lastWrites = 0, changedAt = 0;
  static bool pending = false;
  if (!fmpacSram) return;
  uint32_t w = msx2FmPacSramWrites();
  if (w != lastWrites) { lastWrites = w; changedAt = millis(); pending = true; return; }
  if (!pending || millis() - changedAt < 2000) return;
  pending = false;
  File f = SD.open(FMPAC_SRAM_FILE, FILE_WRITE);
  if (!f) { timedStatus("FM-PAC SRAM SAVE ERROR", 1500); return; }
  f.write((const uint8_t *)FMPAC_SRAM_HEADER, 16);
  f.write(fmpacSram, 0x1FFE);
  f.close();
  Serial.println("FM-PAC SRAM saved");
}

// ---------------------------------------------------------------------------
//  PS/2 マウス(起動メニューで選んだジョイスティックポートにつなぐ)
// ---------------------------------------------------------------------------
static void updateMouse() {
  if (!mouseOn) return;
  auto m = PS2Controller.mouse();
  if (!m) return;
  while (m->deltaAvailable()) {
    MouseDelta d;
    if (!m->getNextDelta(&d, 0)) break;
    msx2_mouse_x += d.deltaX;                        // 右が +
    msx2_mouse_y += d.deltaY;                        // 上が +
    msx2_mouse_buttons = (d.buttons.left ? 1 : 0) | (d.buttons.right ? 2 : 0);
  }
}

static void updateAllSound() {
  if (isPaused) {
    psgGen.setVolume(0);
    for (int i = 0; i < 5; i++) scc_ch[i]->setVolume(0);
    return;
  }
  psgGen.setVolume(100);
  updateSCCSound();
  psgGen.fillBuffer();
  for (int i = 0; i < 5; i++) scc_ch[i]->fillBuffer();
#ifdef MSX2_HAS_FM
  fmGen->fillBuffer();
#endif
}

void loop() {
  if (frameReady) {
    frameReady = false;
    updateController();
    updateKeyboard();
    updateMouse();
    saveFmPacSramIfChanged();
    if (statusClearAt && (int32_t)(millis() - statusClearAt) >= 0) { statusClearAt = 0; clearStatus(); }
  }
  updateAllSound();
  if (isPaused) {
    vTaskDelay(pdMS_TO_TICKS(5));
    updateKeyboard();
  } else {
    // リングは BUFFER_SIZE=256 / 16384Hz = 約15.6ms ぶんあるので1tick眠っても途切れない
    vTaskDelay(1);
  }
}
