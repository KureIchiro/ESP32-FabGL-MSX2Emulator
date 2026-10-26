/*
 * ============================================================================
 *  MSX Emulator for ESP32 (Work in Progress)
 * ============================================================================
 * 
 * [ Credits & Acknowledgments ]
 *  - Graphics/Audio Engine: FabGL by Fabrizio Di Vittorio.
 *  - Z80 CPU Core: Modified based on the work by Marat Fayzullin.
 *  - VDP Section: Modified based on the TMS9918A core originally by David Latham 
 *                 and maintained by Marat Fayzullin.
 *  Special thanks to all authors for providing these excellent foundations.
 * 
 * [ Development Environment ]
 *  - Board Manager: ESP32 by Espressif Systems (v2.0.5 recommended)
 *  - Library: FabGL v1.0.9
 * 
 * [ Recommended Arduino IDE Settings ]
 *  - PSRAM: "Enabled"
 *  - Partition Scheme: "Default 4MB with SPIFFS" or similar
 *  - ESP32 Dev Module
 * 
 * [ Disclaimer ]
 *  IN NO EVENT SHALL FABRIZIO DI VITTORIO, MARAT FAYZULLIN, DAVID LATHAM, 
 *  OR THE DEVELOPER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, OR 
 *  CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE. 
 *  USE AT YOUR OWN RISK.
 * 
 * [ Important Notes ]
 *  - This project is under development and may contain bugs or redundant code.
 *  - Localization (e.g., keyboard mappings) should be customized by the user.
 *  - Subject to the license policies of FabGL and Marat Fayzullin (fMSX License).
 * ============================================================================
 */

/* Japanese
 * ============================================================================
 *  MSX Emulator for ESP32 (Work in Progress)
 * ============================================================================
 * 
 * 【クレジットと謝辞】
 *  - グラフィック/オーディオエンジン: Fabrizio Di Vittorio氏による FabGL を使用しています。
 *  - Z80 CPUコア: Marat Fayzullin氏によるコードをベースに改変して使用しています。
 *  - VDPセクション: David Latham氏によるTMS9918Aエミュレーションコアをベースに、
 *                  Marat Fayzullin氏が調整を加えた実装を改変して使用しています。
 *  素晴らしい基盤を公開されている諸氏に深く感謝の意を表します。
 * 
 * 【動作確認済み環境 / Required Libraries】
 *  - ボードマネージャ: ESP32 by Espressif Systems (v2.0.5 推奨)
 *  - 使用ライブラリ: FabGL v1.0.9
 * 
 * 【Arduino IDE 推奨設定】
 *  - PSRAM: "Enabled" 
 *  - Partition Scheme: "Default 4MB with SPIFFS" 等
 *  - ESP32 Dev Module
 * 
 * 【免責事項】
 *  本ソフトウェアの使用によって生じたいかなる損害（直接的・間接的を問わず）
 *  についても、Fabrizio Di Vittorio氏、Marat Fayzullin氏、David Latham氏、
 *  および開発者本人は一切の責任を負いません。利用者の責任において使用してください。
 * 
 * 【注意事項】
 *  - 本プロジェクトは開発中であり、不具合や最適化不足なコードが含まれる場合があります。
 *  - キーボード配列等のローカライズに関しては、各自の環境に合わせて修正してください。
 *  - 本ソフトウェア内の各コアのライセンスは、FabGL、および Marat Fayzullin氏 
 *    (fMSX License) の各ポリシーに従います。
 * ============================================================================
 */


#pragma GCC optimize("O3")

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <SPI.h>
#include <SD.h>
#include <fabgl.h>
#include <vector>
#include <algorithm>
#include <WiFi.h>
#include "soc/rtc_wdt.h"
#include "esp_task_wdt.h"
#include "tms9918a.h"
#include "Z80.h"
#include "KeyboardSystem.hpp"
#include "JoyKeys.hpp"

// SD カードの中の MSX1 のフォルダ(BIOS・/ROM・/DISK・設定ファイルはすべてこの下。MSX2 は /MSX2、マーク3 は /SMS)
#define SD_ROOT "/MSX1"
#include "wd2793.h"
#include "emu2413.h"

// (Arduino が関数の宣言を先頭に足すので、引数に使う型はここで定義しておく)
enum MapperType { MAPPER_NONE,
                  MAPPER_KONAMI,
                  MAPPER_ASCII8,
                  MAPPER_ASCII16,
                  MAPPER_RTYPE,
                  MAPPER_SCC,        // ROM の無い SCC カートリッジ(SD スナッチャーなどの音源の代わり。RAM は無し)
                  MAPPER_FMPAC };    // FM-PAC(ROM 16KB×バンク + SRAM 8KB + OPLL)

// カートリッジ(スロット1 / 2)。pages は 4000h/6000h/8000h/A000h の 8KB(nullptr = 空き)
struct Cart {
  uint8_t* rom;
  uint32_t mask8k;
  MapperType mapper;
  int banks[4];
  uint8_t* pages[4];
  bool sccActive;                    // 9000h〜97FFh に 3Fh: 9800h〜 に SCC
};
Cart carts[3];                       // [1],[2] を使う

//Please select one of the following options.
#define KEYBORD_MAP (KeyboardLayout::JAPANESE)
//#define KEYBORD_MAP (KeyboardLayout::US_ENGLISH)
//#define KEYBORD_MAP (KeyboardLayout::PORTUGUESE)
//#define KEYBORD_MAP (KeyboardLayout::SPANISH)
//#define KEYBORD_MAP (KeyboardLayout::ITALIAN)

#define USE_FDC

//Disk interface type of your DISK.ROM (effective only with USE_FDC).
//  defined     : Sony/Philips type, memory-mapped FDC registers at 7FF8h-7FFFh
//  not defined : I/O port type (Microsol etc.), FDC registers at D0h-D4h
#define DISK_IF_MEMORY_MAPPED

// TTGO VGA32
#define SD_SCLK 14
#define SD_MISO 2
#define SD_MOSI 12
#define SD_CS 13

// // ORANGE ESPer
// #define SD_SCLK 18
// #define SD_MISO 19
// #define SD_MOSI 23 // or 12
// #define SD_CS 5

#define VDP_SAVE_SIZE offsetof(struct tms9918a, rasterbuffer0)
#define VRAM_SIZE 16384

extern "C" {
  int readByte(void* self, int addr);
  void writeByte(void* self, int addr, int val);
  int readIO(void* context, int port);
  void writeIO(void* context, int port, int val);

  extern Z80 cpu;

  byte RdZ80Slow(zword Addr);
  void WrZ80Slow(zword Addr, byte Value);
  byte InZ80(zword Port);
  void OutZ80(zword Port, byte Value);
  void PatchZ80(Z80* R);
}

Z80 cpu;

// Z80.c は普通のメモリを表で直接読み書きし、表が nullptr のページだけここを呼ぶ
byte IRAM_ATTR RdZ80Slow(zword Addr) {
  return (byte)readByte(NULL, Addr);
}
void IRAM_ATTR WrZ80Slow(zword Addr, byte Value) {
  writeByte(NULL, Addr, Value);
}
byte IRAM_ATTR InZ80(zword Port) {
  return (byte)readIO(NULL, Port);
}
void IRAM_ATTR OutZ80(zword Port, byte Value) {
  writeIO(NULL, Port, Value);
}
void PatchZ80(Z80* R) {}

// --- Prototypes ---
void listROMs();
void updateKeyboard();
void updateFabGLSound();
void handleDiskSelect(int drive);
String showFileSelector(const char* title, const char* dirPath, const char* ext,
                        const std::vector<String>& extra = {}, const String& current = "");
void flashStatus(const char* message, int ms);
bool loadSdToPsram(int drive, String path);
bool savePsramToSd(int drive, String path);
void clearCanvasDouble();


// FM-PAC(どちらかのスロット。動作は MSX2 版と同じ openMSX の MSXFmPac)
//   7FF7h バンク / 7FF6h bit0 = I/O 7Ch・7Dh 有効、bit4 = SRAM の鍵を固定 / 7FF4h・7FF5h OPLL
//   5FFEh=4Dh かつ 5FFFh=69h で 4000h〜5FFDh が SRAM
struct FmPac {
  uint8_t* sram;
  uint8_t bankMask, enable, bank, r1ffe, r1fff;
  bool sramOn;
};
FmPac fmpac;
int fmpacSlot = 0;                   // 0 = なし、1 / 2
volatile uint32_t fmpacSramWrites;   // SRAM を書き換えた回数(SD への保存の目安)

// Mark III と同じ VGAController (ビューポートをDMAが直接読む方式)。
// VGADirectController は1ラインごとの割込みで絵を作るため、モニターによって
// 表示位置がずれた(1920x1200で確認)。
fabgl::VGAController DisplayController;
fabgl::SoundGenerator soundGenerator;
fabgl::NoiseWaveformGenerator noiseGen;
fabgl::Mouse Mouse;
fabgl::PS2Controller PS2Controller;

// state save
struct StateSaveData {
  uint16_t AF, BC, DE, HL, IX, IY, PC, SPtr;
  uint16_t AF1, BC1, DE1, HL1;
  uint8_t I, R, IFF;
  uint16_t IRequest;
  uint8_t primary_slot;
  uint8_t rom_banks[4];
};
uint8_t* psram_slots[5] = { NULL, NULL, NULL, NULL, NULL };
const size_t SAVE_BUFFER_SIZE = 100 * 1024;
StateSaveData tempState;
bool isPaused = false;
enum class SaveMode { NONE,
                      GAME_TO_PSRAM,
                      PSRAM_TO_GAME,
                      PSRAM_TO_SD,
                      SD_TO_PSRAM };
volatile SaveMode currentSaveMode = SaveMode::NONE;

//FDC (WD2793 本体は wd2793.cpp。ここはディスクイメージの置き場所だけ)
struct Drive {
  uint8_t* buffer = nullptr;
  size_t size = 0;
};
Drive drives[2];
static uint32_t fdcLines;                 // 起動からのライン数(FDC のインデックスパルス用)
uint32_t fdcLineClock() { return fdcLines; }
char drive_filepath[2][256];
size_t drive_imagesize[2];
uint8_t* disk_ram_a = nullptr;
uint8_t* disk_ram_b = nullptr;

// MemoryMapper/ROM
std::vector<String> romFiles;
String selectedRom = "";                // スロット1の ROM(ステートセーブのファイル名に使う)
uint8_t* slot_mem[4] = { nullptr, nullptr, nullptr, nullptr };  // [0]=BIOS [2]=DISK.ROM(4000h〜) [1][3]は未使用
uint8_t* slot3_page[8] = { nullptr };  // スロット3 RAM 64KB を8KB×8ページで(内部RAM優先)

// 内部RAMの残量ログ(ビューポートとスロットRAMの確保確認用)
static void printHeap(const char* tag) {
  Serial.printf("HEAP[%-11s] free=%6u largest=%6u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}
bool isRomListLoaded = false;
// msx1_rd_map : データ読み出し用。メモリマップド型では nullptr のページを readMemSpecial() へ回す
//               (ソニー型ディスクROMの 7FF8h〜7FFFh のFDCレジスタを捕まえるため)
// msx1_wr_map : 書き込み用。RAM(スロット3)のページだけ実メモリ、ほかは nullptr(writeByte で処理)
// op_map      : 命令読み込み用。常に実メモリを指す。
// 3つとも Z80.c が直接引く(nullptr のページだけ RdZ80Slow / WrZ80Slow を呼ぶ)
extern "C" { uint8_t* msx1_rd_map[8]; uint8_t* msx1_wr_map[8]; uint8_t* op_map[8]; }
// DISK.ROM の場所: 0 = 無し、2 = スロット2、3 = スロット3-1(スロット2にカートリッジを挿したとき。
// このときだけスロット3を拡張スロットにする: 3-0 = RAM、3-1 = DISK.ROM)
static int diskPrim = 0;
static bool slot3Expanded = false;
static uint8_t ssr3 = 0;                 // スロット3の拡張スロットレジスタ(FFFFh)
static bool diskVisible = false;         // 6000h〜7FFFh にディスクROMが見えている(7FF8h〜 が FDC)
static bool fmpacVisible = false;        // 4000h〜7FFFh に FM-PAC が見えている(7FF6h/7FF7h を読む)
static bool sccReadable = false;         // 8000h〜9FFFh に SCC が見えている(9800h〜 の波形を読む)
uint8_t dummy_page[8192];

//psg scc
volatile uint8_t psg_regs[16];
uint8_t old_psg_regs[16] = { 0xFF };
uint8_t psg_address = 0;
int8_t scc_waveform[5][32] = { 0 };
uint16_t scc_f[] = { 0, 0, 0, 0, 0, 0 };  //6ch for converting each PSG ch to 2 SCC chs.
uint8_t scc_v[] = { 0, 0, 0, 0, 0, 0 };
uint8_t scc_enable;
bool psg2scc = false;
static const int BUFFER_SIZE = 256;
const uint8_t SCC_VOL_TABLE[16] = {
  0, 2, 4, 7, 10, 15, 21, 26, 33, 39, 47, 54, 62, 68, 73, 75
};

uint8_t Dirty_Bass[32]{
  0x80, 0x88, 0x70, 0x98, 0x80, 0x90, 0x78, 0xA0, 0x90, 0xB0, 0xA0, 0xD0, 0xC0, 0xF0, 0xEF, 0xA0,
  0x78, 0x60, 0x70, 0x50, 0x60, 0x40, 0x50, 0x30, 0x40, 0x20, 0x30, 0x10, 0x20, 0x00, 0x10, 0x20
};

uint8_t Pseudo_Vocal[32] = {
  0x80, 0x85, 0x90, 0xA0, 0xC0, 0xFF, 0xC0, 0xA0, 0x90, 0x85, 0x80, 0x7B, 0x70, 0x60, 0x40, 0x00,
  0x40, 0x60, 0x70, 0x7B, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xFF, 0xD0, 0xC0, 0xB0, 0xA0, 0x90
};

uint8_t Warm_Body_PhaseShift[32] = {
  0x80, 0x68, 0x50, 0x38, 0x20, 0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10, 0x20, 0x38, 0x50, 0x68,
  0x80, 0x98, 0xB0, 0xC8, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFC, 0xF8, 0xF0, 0xE0, 0xC8, 0xB0, 0x98
};

uint8_t Metallic_Lead[32] = {
  0x80, 0xC0, 0xFF, 0xC0, 0x80, 0x40, 0x00, 0x40, 0x80, 0xFF, 0x80, 0xFF, 0x80, 0x40, 0x00, 0x40,
  0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0xFF, 0xE0, 0xC0, 0xA0, 0x80, 0x60, 0x40, 0x20
};


//keyboard
volatile uint8_t msx_key_matrix[12];
static bool key_map_inited = false;
uint8_t joy_bits;
volatile uint8_t joy_bits1;
volatile uint8_t joy_bits2;
uint8_t selected_row = 0;
uint8_t slot_reg = 0;

//VDP
struct tms9918a* vdp;
volatile bool frameReady = false;
bool displayEnabled = true;
//V9938 color (RGB888, used to compute RGB222 colourmap)
static const fabgl::RGB888 msx_palette[16] = {
  { 0x00, 0x00, 0x00 },  // 0: Transparent (Black)
  { 0x00, 0x00, 0x00 },  // 1: Black
  { 0x2D, 0xDD, 0x22 },  // 2: Medium Green
  { 0x66, 0xFF, 0x66 },  // 3: Light Green
  { 0x22, 0x22, 0xFF },  // 4: Dark Blue
  { 0x44, 0x66, 0xFF },  // 5: Light Blue
  { 0xAA, 0x22, 0x22 },  // 6: Dark Red
  { 0x44, 0xDD, 0xFF },  // 7: Cyan
  { 0xFF, 0x22, 0x22 },  // 8: Medium Red
  { 0xFF, 0x66, 0x66 },  // 9: Light Red
  { 0xDD, 0xDD, 0x22 },  // 10: Dark Yellow
  { 0xDD, 0xDD, 0x88 },  // 11: Light Yellow
  { 0x22, 0x88, 0x22 },  // 12: Dark Green
  { 0xDD, 0x44, 0xAA },  // 13: Magenta
  { 0xAA, 0xAA, 0xAA },  // 14: Gray
  { 0xFF, 0xFF, 0xFF }   // 15: White
};

// --- UI / 画面(VGAController) ---
// 以前は PSRAM の uiBuffer(320x200) に描き、走査線割込みでゲーム画面と合成していた。
// 今は UI も FabGL のビューポート(内部RAM)へ直接描き、ゲーム画面は loop() で
// 1フレームごとにビューポートへ写す。画面の配置は以前と同じ:
//   0行目=黒 / 1〜192行目=MSX画面(x=32〜287) / 193〜199行目=ステータス表示
static uint8_t* vpRow[200];     // ビューポートの各行(FabGLのネイティブ形式, 320バイト)
static bool vpReady = false;
enum class DisplayMode : uint8_t { UI, GAME };
static volatile DisplayMode displayMode = DisplayMode::UI;

// VGA native pixel constants: bit7=VSync, bit6=HSync, bit5-0=BBGGRR
// Active video bytes must have bit7=1 and bit6=1 (sync inactive = HIGH)
// so the base is 0xC0; color bits (00BBGGRR) are OR'd in.
static constexpr uint8_t UI_BLACK = 0xC0;  // 0xC0 | 0x00
static constexpr uint8_t UI_WHITE = 0xFF;  // 0xC0 | 0x3F
static constexpr uint8_t UI_BLUE = 0xF0;   // 0xC0 | 0x30  dark blue background
static constexpr uint8_t UI_CYAN = 0xFD;   // 0xC0 | 0x3D  folder
static constexpr uint8_t UI_YELLOW = 0xCF; // 0xC0 | 0x0F  selected item
static constexpr uint8_t UI_GRAY = 0xEA;   // 0xC0 | 0x2A

namespace fabgl { extern const FontInfo FONT_8x8; }

static void vpClearRows(int y0, int y1) {
  if (!vpReady) return;
  for (int y = y0; y <= y1 && y < 200; y++) memset(vpRow[y], UI_BLACK, 320);
}

static void uiFillRect(int x1, int y1, int x2, int y2, uint8_t col) {
  if (!vpReady) return;
  for (int y = y1; y <= y2 && y < 200; y++) {
    if (y < 0) continue;
    uint8_t* row = vpRow[y];
    for (int x = x1; x <= x2 && x < 320; x++) {
      if (x >= 0) row[x ^ 2] = col;
    }
  }
}

static void uiDrawChar(int x, int y, char ch, uint8_t fg, uint8_t bg) {
  if (!vpReady) return;
  const fabgl::FontInfo* f = &fabgl::FONT_8x8;
  uint8_t c = (uint8_t)ch;
  if (c < 32 || c > 127) c = '?';
  uint8_t charIdx = c;
  const uint8_t* glyph;
  if (f->chptr) {
    glyph = f->data + pgm_read_dword(f->chptr + charIdx);
  } else {
    glyph = f->data + charIdx * f->height;
  }

  // 元の呼び出しが y=192 の場合、表示エリア（193〜199）に収めるため自動で1ライン下げます
  int drawY = (y == 192) ? 193 : y;

  // ループを f->height (8) から 「7」 に変更し、上から7行分だけを描画
  for (int row = 0; row < 7; row++) {
    if (drawY + row < 0 || drawY + row >= 200) continue;
    uint8_t bits = pgm_read_byte(glyph + row);

    // 行の先頭アドレスを取得（drawY を使用）
    uint8_t* dst_row = vpRow[drawY + row];

    for (int col = 0; col < f->width; col++) {
      int abs_x = x + col;  // 画面上の絶対X座標を計算
      if (abs_x < 0 || abs_x >= 320) continue;

      // 絶対X座標に対して ^ 2 を適用して書き込む
      dst_row[abs_x ^ 2] = (bits & (0x80 >> col)) ? fg : bg;
    }
  }
}

static void uiText(int x, int y, const char* s, uint8_t fg, uint8_t bg) {
  while (s && *s) {
    uiDrawChar(x, y, *s++, fg, bg);
    x += 8;
  }
}

// ゲーム画面(192行)をビューポートの1〜192行目、x=32〜287へ写す。
// ラスタは tms9918a.c が既にFabGLのネイティブ形式(ピクセル並べ替え済み・
// 同期ビット0xC0入り)で作っているので、行単位のmemcpyで済む。
// 左右の黒帯(0〜31, 288〜319)と0行目は vpClearRows() で黒にしたまま触らない。
static void presentGameFrame() {
  if (!vpReady || !vdp) return;
  const uint8_t* src = vdp->visible_raster;
  for (int y = 0; y < 192; y++) {
    memcpy(vpRow[y + 1] + 32, src + y * 256, 256);
  }
}

class SCCGenerator : public fabgl::WaveformGenerator {
private:
  int16_t sampleBuffer[BUFFER_SIZE];
  volatile int head = 0;
  volatile int tail = 0;
  int lastSample = 0;

  int8_t waveform[32];
  uint32_t phase = 0;
  uint32_t phaseInc = 0;
  uint8_t curVol = 0;

public:
  int getSample() override {
    if (head == tail) return lastSample;
    lastSample = sampleBuffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return lastSample;
  }

  void setFrequency(int value) override {}

  void updateParams(int8_t* newWave, uint32_t freq, uint8_t vol) {
    if (freq == 0) {
      phaseInc = 0;
    } else {
      uint32_t freqHz = 111860 / (freq + 1);
      phaseInc = (uint32_t)(freqHz << 18);
    }
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

    int8_t sample = waveform[(phase >> 27) & 0x1F];
    phase += phaseInc;

    return (int16_t)((int)sample * curVol >> 8);
  }
};

SCCGenerator* scc_ch[5];

void IRAM_ATTR updateSCCSound() {
  for (int i = 0; i < 5; i++) {
    auto* ch = scc_ch[i];
    if ((scc_enable & (1 << i)) && scc_f[i] > 10) {
      int waveIdx = (i < 4) ? i : 3;
      ch->updateParams((int8_t*)scc_waveform[waveIdx], scc_f[i], SCC_VOL_TABLE[scc_v[i]]);
    } else {
      ch->updateParams((int8_t*)scc_waveform[0], 0, 0);
    }
  }
}

class AY38910Generator : public fabgl::WaveformGenerator {
private:
  const uint32_t psgClock = 111860;
  uint32_t tickAccum = 0;
  uint32_t tickStep = 0;

  volatile uint32_t tonePeriod[3] = { 1, 1, 1 };
  volatile uint32_t noisePeriod = 1;
  volatile uint32_t envPeriod = 1;
  volatile uint8_t envShape = 0;
  volatile uint8_t regs[16] = { 0 };
  volatile bool holding = true;
  volatile bool cont = false, alt = false, hold = false, att = false;
  volatile uint32_t envCounter = 0;
  volatile uint32_t envStep = 0;
  volatile uint32_t envVol = 0;

  uint32_t toneCounter[3] = { 0, 0, 0 };
  uint32_t toneOut = 0;
  uint32_t noiseCounter = 0;
  uint32_t lfsr = 1;
  uint32_t noiseOut = 0;

  int16_t volTable[16] = { 0, 5, 10, 15, 20, 26, 32, 40, 48, 58, 68, 78, 90, 102, 114, 127 };

  int16_t sampleBuffer[BUFFER_SIZE];
  volatile int head = 0;
  volatile int tail = 0;
  int lastSample = 0;

public:
  void writeReg(uint8_t reg, uint8_t val) {
    if (reg > 13) return;
    regs[reg] = val;

    switch (reg) {
      case 0:
      case 1:
        tonePeriod[0] = (regs[0] | ((regs[1] & 0x0F) << 8)) ? (regs[0] | ((regs[1] & 0x0F) << 8)) : 1;
        break;
      case 2:
      case 3:
        tonePeriod[1] = (regs[2] | ((regs[3] & 0x0F) << 8)) ? (regs[2] | ((regs[3] & 0x0F) << 8)) : 1;
        break;
      case 4:
      case 5:
        tonePeriod[2] = (regs[4] | ((regs[5] & 0x0F) << 8)) ? (regs[4] | ((regs[5] & 0x0F) << 8)) : 1;
        break;
      case 6:
        noisePeriod = (regs[6] & 0x1F) ? (regs[6] & 0x1F) : 1;
        break;
      case 11:
      case 12:
        envPeriod = (regs[11] | (regs[12] << 8)) ? (regs[11] | (regs[12] << 8)) : 1;
        break;
      case 13:
        envShape = val & 0x0F;
        envStep = 0;
        envCounter = 0;
        cont = envShape & 0x08;
        att = envShape & 0x04;
        alt = envShape & 0x02;
        hold = envShape & 0x01;
        envVol = att ? 0 : 15;
        holding = false;
        break;
    }
  }

  void fillBuffer() {
    if (tickStep == 0) {
      int rate = sampleRate();
      if (rate > 0) setSampleRate(rate);
      else return;
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

    if (rate > 0) {
      tickStep = (uint32_t)(((uint64_t)psgClock << 16) / rate);
    } else {
      tickStep = (uint32_t)(((uint64_t)psgClock << 16) / 16384);
    }
  }

private:
  inline int calculateNextSample() {
    uint32_t curR7 = regs[7];
    uint32_t curR8 = regs[8];
    uint32_t curR9 = regs[9];
    uint32_t curR10 = regs[10];
    uint32_t curTP0 = tonePeriod[0];
    uint32_t curTP1 = tonePeriod[1];
    uint32_t curTP2 = tonePeriod[2];
    uint32_t curNP = noisePeriod;

    tickAccum += tickStep;
    uint32_t ticks = tickAccum >> 16;
    tickAccum &= 0xFFFF;

    while (ticks--) {
      toneCounter[0] += 2;
      while (toneCounter[0] >= curTP0) {
        toneCounter[0] -= curTP0;
        toneOut ^= 1;
      }
      toneCounter[1] += 2;
      while (toneCounter[1] >= curTP1) {
        toneCounter[1] -= curTP1;
        toneOut ^= 2;
      }
      toneCounter[2] += 2;
      while (toneCounter[2] >= curTP2) {
        toneCounter[2] -= curTP2;
        toneOut ^= 4;
      }

      if (++noiseCounter >= curNP) {
        noiseCounter = 0;
        lfsr = (lfsr >> 1) | (((lfsr & 1) ^ ((lfsr >> 3) & 1)) << 16);
        noiseOut = lfsr & 1;
      }

      if (!holding && ++envCounter >= envPeriod) {
        envCounter = 0;
        stepEnvelope();
      }
    }

    uint32_t tMask = toneOut | curR7;
    uint32_t nMask = (noiseOut ? 0x07 : 0) | (curR7 >> 3);
    uint32_t mixMask = tMask & nMask;

    int volA = volTable[(curR8 & 0x10) ? envVol : (curR8 & 0x0F)];
    int volB = volTable[(curR9 & 0x10) ? envVol : (curR9 & 0x0F)];
    int volC = volTable[(curR10 & 0x10) ? envVol : (curR10 & 0x0F)];

    // ノイズ控えめ処琁E
    if ((curR7 & 0x09) == 0x01) volA >>= 1;
    if ((curR7 & 0x12) == 0x02) volB >>= 1;
    if ((curR7 & 0x24) == 0x04) volC >>= 1;

    int mixedSample = 0;
    if (mixMask & 1) mixedSample += volA;
    if (mixMask & 2) mixedSample += volB;
    if (mixMask & 4) mixedSample += volC;

    return (mixedSample >> 1) - 95;
  }

  int getSample() override {
    if (head == tail) {
      return lastSample;
    }

    lastSample = sampleBuffer[tail];
    tail = (tail + 1) % BUFFER_SIZE;
    return lastSample;
  }

  inline void stepEnvelope() {
    if (envStep < 15) {
      envStep++;
      envVol = att ? envStep : (15 - envStep);
    } else {
      if (!cont) {
        holding = true;
        envVol = 0;
      } else {
        if (hold) {
          holding = true;
          if (alt) att = !att;
          envVol = att ? 15 : 0;
        } else {
          envStep = 0;
          if (alt) att = !att;
          envVol = att ? 0 : 15;
        }
      }
    }
  }

  void setFrequency(int value) override {}
};
AY38910Generator psgGen;

// ---------------------------------------------------------------------------
//  メモリマップ
//   スロット0 = BIOS、1 / 2 = カートリッジ(ROM / SCC / FM-PAC)か DISK.ROM、3 = RAM 64KB。
//   スロット2 にカートリッジを挿して Disk BIOS も使うときは、スロット3を拡張して 3-1 に DISK.ROM。
// ---------------------------------------------------------------------------
static inline uint8_t* diskRomPage(int page) {   // page 2 / 3 = 4000h / 6000h
  return &slot_mem[2][page * 0x2000];
}

void updateMemoryMap() {
  for (int page = 0; page < 8; page++) {
    int sh = (page >> 1) * 2;
    int slot = (slot_reg >> sh) & 0x03;
    uint8_t* rdp = nullptr;
    uint8_t* wrp = nullptr;
    switch (slot) {
      case 0:
        if (page < 4) rdp = &slot_mem[0][page * 0x2000];
        break;
      case 1:
      case 2:
        if (slot == diskPrim) { if (page == 2 || page == 3) rdp = diskRomPage(page); }
        else if (page >= 2 && page <= 5) rdp = carts[slot].pages[page - 2];
        break;
      case 3: {
        int sub = slot3Expanded ? (ssr3 >> sh) & 3 : 0;
        if (sub == 0) rdp = wrp = slot3_page[page];
        else if (sub == 1 && diskPrim == 3 && (page == 2 || page == 3)) rdp = diskRomPage(page);
        break;
      }
    }
    msx1_rd_map[page] = op_map[page] = rdp ? rdp : dummy_page;
    msx1_wr_map[page] = wrp;
  }
  // データ読み出しだけ readMemSpecial() へ回すページ(命令の読み込みは op_map で直接読むので遅くならない)
  int p1 = (slot_reg >> 2) & 3, p2 = (slot_reg >> 4) & 3, p3 = (slot_reg >> 6) & 3;
  int sub1 = (p1 == 3 && slot3Expanded) ? (ssr3 >> 2) & 3 : 0;
#ifdef DISK_IF_MEMORY_MAPPED
  // ソニー型ディスクROM(6000h〜7FFFh。7FF8h〜7FFFh がFDCレジスタ)
  diskVisible = diskPrim && ((diskPrim == p1 && p1 != 3) || (diskPrim == 3 && p1 == 3 && sub1 == 1));
#else
  diskVisible = false;
#endif
  fmpacVisible = fmpacSlot && p1 == fmpacSlot;             // FM-PAC の 7FF6h / 7FF7h
  if (diskVisible || fmpacVisible) msx1_rd_map[3] = nullptr;
  sccReadable = (p2 == 1 || p2 == 2) && p2 != diskPrim && carts[p2].sccActive;   // SCC の波形メモリ
  if (sccReadable) msx1_rd_map[4] = nullptr;
  // 拡張したスロット3の FFFFh は拡張スロットレジスタ(読むと反転値)。3-0 の RAM のときは
  // RAM の FFFFh に反転値を置いておく(書き込みは Z80.c が FFFFh だけ WrZ80Slow へ回す)
  if (slot3Expanded && p3 == 3) {
    if (((ssr3 >> 6) & 3) == 0) slot3_page[7][0x1FFF] = (uint8_t)~ssr3;
    else msx1_rd_map[7] = nullptr;
  }
}

// カートリッジのバンク(8KB 単位)
static void cartSetBank(int s, int page_idx, int v) {
  Cart& c = carts[s];
  c.banks[page_idx] = v;
  c.pages[page_idx] = c.rom ? &c.rom[((uint32_t)v & c.mask8k) << 13] : nullptr;
}

static void initMegaROM(int s) {
  Cart& c = carts[s];
  c.sccActive = false;
  switch (c.mapper) {
    case MAPPER_ASCII8:
      for (int i = 0; i < 4; i++) cartSetBank(s, i, 0);
      break;
    case MAPPER_ASCII16:
      cartSetBank(s, 0, 0); cartSetBank(s, 1, 1); cartSetBank(s, 2, 0); cartSetBank(s, 3, 1);
      break;
    case MAPPER_RTYPE:
      cartSetBank(s, 0, 46); cartSetBank(s, 1, 47); cartSetBank(s, 2, 0); cartSetBank(s, 3, 1);
      break;
    case MAPPER_SCC:
      for (int i = 0; i < 4; i++) c.pages[i] = nullptr;
      break;
    case MAPPER_FMPAC:
      fmpac.enable = fmpac.bank = fmpac.r1ffe = fmpac.r1fff = 0;
      fmpac.sramOn = false;
      cartSetBank(s, 0, 0); cartSetBank(s, 1, 1);
      c.pages[2] = c.pages[3] = nullptr;
      break;
    default:
      for (int i = 0; i < 4; i++) cartSetBank(s, i, i);
      break;
  }
}

// 起動時: スロット s にカートリッジを入れる(rom は 2 のべき乗の大きさに切り上げてあること)
static void cartInsert(int s, uint8_t* rom, uint32_t size, MapperType m) {
  Cart& c = carts[s];
  memset(&c, 0, sizeof(c));
  c.rom = rom;
  c.mapper = m;
  uint32_t banks = size >> 13;
  c.mask8k = banks ? banks - 1 : 0;
  if (m == MAPPER_FMPAC) {
    fmpacSlot = s;
    uint32_t b16 = size >> 14;
    fmpac.bankMask = (uint8_t)(b16 > 1 ? b16 - 1 : 0);
  }
  initMegaROM(s);
}

// ---- FM-PAC ----
static void fmpacUpdatePages() {
  Cart& c = carts[fmpacSlot];
  if (fmpac.sramOn) { c.pages[0] = fmpac.sram; c.pages[1] = nullptr; }
  else {
    int b = (fmpac.bank & fmpac.bankMask) * 2;
    cartSetBank(fmpacSlot, 0, b);
    cartSetBank(fmpacSlot, 1, b + 1);
  }
  updateMemoryMap();
}
static void fmpacCheckSram() {
  bool on = fmpac.sram && fmpac.r1ffe == 0x4D && fmpac.r1fff == 0x69;
  if (on == fmpac.sramOn) return;
  fmpac.sramOn = on;
  if (on) { fmpac.sram[0x1FFE] = 0x4D; fmpac.sram[0x1FFF] = 0x69; }   // 読むと鍵の値が見える
  fmpacUpdatePages();
}
void fmWrite(int port, uint8_t v);
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
    case 0x3FF4: case 0x3FF5: fmWrite(o & 1, v); return;
    case 0x3FF6:
      fmpac.enable = v & 0x11;
      if (fmpac.enable & 0x10) { fmpac.r1ffe = fmpac.r1fff = 0; fmpacCheckSram(); }
      return;
    case 0x3FF7:
      if ((v & 3) != fmpac.bank) { fmpac.bank = v & 3; fmpacUpdatePages(); }
      return;
    default:
      if (fmpac.sramOn && o < 0x1FFE && fmpac.sram[o] != v) { fmpac.sram[o] = v; fmpacSramWrites++; }
      return;
  }
}

void initDiskPSRAM() {
  disk_ram_a = (uint8_t*)ps_malloc(720 * 1024);
  disk_ram_b = (uint8_t*)ps_malloc(720 * 1024);
  drives[0].buffer = disk_ram_a;
  drives[0].size = 720 * 1024;
  drives[1].buffer = disk_ram_b;
  drives[1].size = 720 * 1024;
  memset(disk_ram_a, 0, 720 * 1024);
  memset(disk_ram_b, 0, 720 * 1024);
}

bool loadSdToPsram(int drive, String path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size_t readSize = f.read(drives[drive].buffer, 720 * 1024);
  f.close();
  if (readSize == 0) return false;

  strncpy(drive_filepath[drive], path.c_str(), sizeof(drive_filepath[drive]) - 1);
  drive_filepath[drive][sizeof(drive_filepath[drive]) - 1] = '\0';  // 終端文字を保証
  drive_imagesize[drive] = readSize;
  drives[drive].size = readSize;
  // 形式(片面/両面、8/9セクタ)の判定と、入れ替えの知らせ(DSKCHG)は wd2793.cpp が行う
  wd2793SetDisk(drive, drives[drive].buffer, readSize);
  return true;
}

bool savePsramToSd(int drive) {
  if (drive_filepath[drive][0] == '\0' || drive_imagesize[drive] == 0) {
    // do nothing
  } else {
    File f = SD.open(drive_filepath[drive], FILE_WRITE);
    if (f) {
      size_t written = f.write(drives[drive].buffer, drive_imagesize[drive]);
      f.close();
      flashStatus("SAVE DISK PSRAM->SD OK", 2000);
      return true;
    }
  }
  flashStatus("SAVE DISK PSRAM->SD NG", 2000);
  return false;
}

void loadDiskROM() {
  File file = SD.open(SD_ROOT "/disk.rom");
  if (file) {
    file.read(&slot_mem[2][0x4000], 16384);
    file.close();
  }
}

// ---------------------------------------------------------------------------
// ソニー(フィリップス)型 メモリマップドFDC  7FF8h〜7FFFh
//   7FF8 R:ステータス W:コマンド / 7FF9 トラック / 7FFA セクタ / 7FFB データ
//   7FFC bit0:サイド / 7FFD bit0-1:ドライブ(0,2=A 1=B 3=なし) bit7:モーター
//       (読むと bit2 で入れ替えを知らせる) / 7FFF R: bit6=/INTRQ, bit7=/DRQ(アクティブLOW)
//   中身は wd2793.cpp (MSX2版と同じ)。
// ---------------------------------------------------------------------------
static uint8_t fdcMemRead(int r) { return wd2793Read(r); }
static void fdcMemWrite(int r, uint8_t v) { wd2793Write(r, v); }

// SCC の波形メモリの読み出し(9800h〜987Fh。そのほかは FFh)
static uint8_t sccRead(uint16_t a) {
  uint8_t r = a & 0xFF;
  return r < 0x80 ? (uint8_t)(scc_waveform[r >> 5][r & 0x1F] + 128) : 0xFF;
}

// msx1_rd_map が nullptr のページの読み出し(FDC / FM-PAC のレジスタ / SCC / 拡張スロットレジスタ)
static int IRAM_ATTR readMemSpecial(int addr) {
  if (diskVisible && addr >= 0x7FF8 && addr <= 0x7FFF) return fdcMemRead(addr & 7);
  if (fmpacVisible && (addr == 0x7FF6 || addr == 0x7FF7)) return addr == 0x7FF6 ? fmpac.enable : fmpac.bank;
  if (sccReadable && addr >= 0x9800 && addr <= 0x9FFF) {
    const Cart& c = carts[(slot_reg >> 4) & 3];
    if (c.mapper != MAPPER_KONAMI || addr <= 0x98FF) return sccRead(addr);   // (K) は 9800h〜98FFh、SCC は 256 バイトごとのミラー
  }
  if (addr == 0xFFFF && slot3Expanded && (slot_reg >> 6) == 3) return (uint8_t)~ssr3;
  return op_map[addr >> 13][addr & 0x1FFF];
}

// 通常は表を1回引くだけ。nullptr(=捕まえるページ)のときだけ分岐先へ。
inline int IRAM_ATTR readByte(void* self, int addr) {
  const uint8_t* p = msx1_rd_map[addr >> 13];
  if (__builtin_expect(p != nullptr, 1)) return p[addr & 0x1FFF];
  return readMemSpecial(addr);
}

// SCC への書き込み(a は 9800h〜98FFh)。波形は読み出し用にも使う
static void sccWrite(uint16_t a, uint8_t val) {
  if (a < 0x9880) {
    int ch_idx = (a - 0x9800) >> 5;
    scc_waveform[ch_idx][a & 0x1F] = (int8_t)((int)val - 128);
  } else if (a < 0x98E0) {
    int reg = a & 0x0F;
    if (reg < 0x0A) {
      int ch_idx = reg >> 1;
      if (reg & 1) scc_f[ch_idx] = (scc_f[ch_idx] & 0x00FF) | ((val & 0x0F) << 8);
      else scc_f[ch_idx] = (scc_f[ch_idx] & 0x0F00) | (val & 0xFF);
    } else if (reg < 0x0F) {
      scc_v[reg - 0x0A] = val & 0x0F;
    } else {
      scc_enable = val & 0x1F;
    }
  }
}

// カートリッジ(スロット s)への書き込み: バンク切り替え・SCC・FM-PAC
static void cartWrite(int s, uint16_t a, uint8_t val) {
  Cart& c = carts[s];
  if ((uint16_t)(a - 0x4000) >= 0x8000) return;
  switch (c.mapper) {
    case MAPPER_KONAMI:
      if (c.sccActive && a >= 0x9800 && a <= 0x98FF) { sccWrite(a, val); return; }
      if (a >= 0x9000 && a <= 0x97FF) c.sccActive = ((val & 0x3F) == 0x3F);
      cartSetBank(s, (a - 0x4000) >> 13, val);
      break;
    case MAPPER_SCC:                         // ROM の無い SCC: 9000h〜97FFh に 3Fh で 9800h〜9FFFh に SCC
      if (a >= 0x9800 && a <= 0x9FFF) { if (c.sccActive) sccWrite(0x9800 | (a & 0xFF), val); return; }
      if (a >= 0x9000 && a <= 0x97FF) c.sccActive = ((val & 0x3F) == 0x3F);
      else return;
      break;
    case MAPPER_ASCII8:
      if (a >= 0x6000 && a <= 0x7FFF) cartSetBank(s, (a >> 11) & 3, val);
      else return;
      break;
    case MAPPER_ASCII16:
      if (a >= 0x6000 && a <= 0x67FF) { cartSetBank(s, 0, val << 1); cartSetBank(s, 1, (val << 1) + 1); }
      else if (a >= 0x7000 && a <= 0x77FF) { cartSetBank(s, 2, val << 1); cartSetBank(s, 3, (val << 1) + 1); }
      else return;
      break;
    case MAPPER_RTYPE:
      if (a >= 0x7000 && a <= 0x7FFF) { uint8_t b = (val % 24) * 2; cartSetBank(s, 2, b); cartSetBank(s, 3, b + 1); }
      else return;
      break;
    case MAPPER_FMPAC:
      fmpacWrite(a, val);
      return;
    default:
      return;
  }
  updateMemoryMap();
}

inline void IRAM_ATTR writeByte(void* self, int addr, int val) {
  uint16_t a = (uint16_t)addr;
  uint8_t* ptr = msx1_wr_map[a >> 13];
  int slot = (slot_reg >> ((a >> 13) & 0x06)) & 0x03;

  if (slot == 3) {
    if (a == 0xFFFF && slot3Expanded) {      // 拡張スロットレジスタ
      ssr3 = (uint8_t)val;
      updateMemoryMap();
      return;
    }
    if (ptr) ptr[a & 0x1FFF] = (uint8_t)val;
    else if (diskPrim == 3 && diskVisible && a >= 0x7FF8 && a <= 0x7FFF) fdcMemWrite(a & 7, (uint8_t)val);
    return;
  }
  if (slot == 1 || slot == 2) {
    if (slot == diskPrim) {
#ifdef DISK_IF_MEMORY_MAPPED
      if (a >= 0x7FF8 && a <= 0x7FFF) fdcMemWrite(a & 7, (uint8_t)val);   // ソニー型FDC
#endif
      return;
    }
    cartWrite(slot, a, (uint8_t)val);
  }
}

void writeWord(void* ctx, int addr, int val) {
  writeByte(ctx, addr, val & 0xFF);
  writeByte(ctx, addr + 1, val >> 8);
}

// ---------------------------------------------------------------------------
//  マウス(MSX2 版と同じ。openMSX の MSXMouse)
//   ジョイスティック端子の 8 番ピン(PSG R#15 bit4 = ポート1、bit5 = ポート2)を切り替えるたびに
//   X 上位4bit → X 下位4bit → Y 上位4bit → Y 下位4bit を R#14 の下位4bit で返す(左・上が +)。
//   ボタンはトリガ A = 左、B = 右。
// ---------------------------------------------------------------------------
volatile int32_t mouse_x, mouse_y;           // loop() が PS/2 マウスの動きを積算する(右・上が +)
volatile uint8_t mouse_buttons;              // bit0 = 左、bit1 = 右(1 = 押下)
static int mousePort = 0;                    // 0 = なし、1 / 2
static int mousePhase = 3;
static int8_t mouseRelX, mouseRelY;
static int32_t mouseLastX, mouseLastY;

// 最後に 8 番ピンを切り替えた時刻。しばらく切り替えが無い(マウスとして読んでいない)ときは、方向のビットを
// 「押していない」にする(MSX2 版と同じ。止まっているマウスの移動量 0 はジョイスティックとして読むと
// 全方向が押された形になり、ポート1も読むゲームでカーソルキーが効かなくなっていた)
static uint32_t mouseStrobeMs;
static bool mouseStrobed = false;

static void mouseStrobe() {
  mouseStrobeMs = millis();
  mouseStrobed = true;
  if (mousePhase == 3) {
    int32_t x = mouse_x, y = mouse_y;
    int dx = -(x - mouseLastX), dy = y - mouseLastY;
    if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
    if (dy > 127) dy = 127; else if (dy < -127) dy = -127;
    mouseRelX = (int8_t)dx; mouseRelY = (int8_t)dy;
    mouseLastX -= dx;
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
  if (!mouseStrobed || millis() - mouseStrobeMs > 70) n = 0x0F;   // 約 4 フレーム読まれていない
  uint8_t b = mouse_buttons;
  return (uint8_t)((joy & 0xC0) | ((b & 1) ? 0 : 0x10) | ((b & 2) ? 0 : 0x20) | (n & 0x0F));
}

inline int IRAM_ATTR readIO(void* context, int port) {
  uint8_t p = (uint8_t)port;

  if ((p & 0xFE) == 0x98) {
    return (int)tms9918a_read(vdp, p & 0x01);
  }
  if (p == 0xA2) {
    if (psg_address == 14) {
      int port = (psg_regs[15] & 0x40) ? 1 : 0;
      uint8_t joy = port ? joy_bits2 : joy_bits1;
      if (mousePort == port + 1) return (int)mouseRead(joy);
      return (int)joy;
    }

    if (psg_address == 15) {
      return (int)psg_regs[15];
    }

    return (int)psg_regs[psg_address];
  }
  if (p == 0xA8) return slot_reg;
  if (p == 0xA9) {
    return (selected_row < 12) ? (int)msx_key_matrix[selected_row] : 0xFF;
  }
  if (p == 0xAA) return selected_row;

#ifndef DISK_IF_MEMORY_MAPPED
  // I/Oポート型FDC (Microsol等 D0h〜D3h、7Ch〜7Fh にも同じレジスタ)
  if ((p >= 0x7C && p <= 0x7F) || (p >= 0xD0 && p <= 0xD3)) {
    p = (p >= 0xD0) ? (p - 0x54) : p;
    if (p >= 0x7C) return wd2793Read(p - 0x7C);   // ステータス / トラック / セクタ / データ
  }
#endif

  return 0xFF;
}

inline void IRAM_ATTR writeIO(void* context, int port, int val) {
  uint8_t p = (uint8_t)port;
  uint8_t v = (uint8_t)val;

  if ((p & 0xFE) == 0x98) {
    tms9918a_write(vdp, p & 0x01, v);
    return;
  }
  if (p == 0xA0) {
    psg_address = v & 0x0F;
    return;
  } else if (p == 0xA1) {
    if (psg_address == 15 && mousePort && ((psg_regs[15] ^ v) & (mousePort == 1 ? 0x10 : 0x20))) mouseStrobe();   // マウスの 8 番ピン
    psg_regs[psg_address] = v;
    psgGen.writeReg(psg_address, v);

    if (psg2scc) {
      int reg = psg_address;
      switch (reg) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
          {
            int ch = reg / 2;
            if (reg % 2 == 0) {
              scc_f[ch] = (scc_f[ch] & 0x0F00) | v;
              scc_f[ch + 3] = (scc_f[ch] >> 1) - 3;
            } else {
              scc_f[ch] = (scc_f[ch] & 0x00FF) | ((v & 0x0F) << 8);
              scc_f[ch + 3] = (scc_f[ch] >> 1) - 3;
            }
          }
          break;

        case 8:
        case 9:
        case 10:
          {
            scc_v[reg - 8] = (v & 0x0F);
            scc_v[reg - 8 + 3] = scc_v[reg - 8] >> 1;
          }
          break;

        case 7:
          {
            scc_enable = (~v) & 0x07;
          }
          break;
      }
    }
    return;
  }
  if (p == 0xA8) {
    slot_reg = v;
    updateMemoryMap();
    return;
  }
  if (p == 0xAA) {
    selected_row = v & 0x0F;
    return;
  }
  if ((p == 0x7C || p == 0x7D) && fmpacSlot && (fmpac.enable & 1)) {   // FM-PAC の OPLL(7FF6h bit0 = 1 のとき)
    fmWrite(p & 1, v);
    return;
  }
#ifndef DISK_IF_MEMORY_MAPPED
  // I/Oポート型FDC (Microsol等 D0h〜D4h、7Bh〜7Fh にも同じレジスタ)
  if ((p >= 0x7B && p <= 0x7F) || (p >= 0xD0 && p <= 0xD4)) {
    p = (p >= 0xD0) ? (p - 0x54) : p;
    switch (p) {
      case 0x7C: case 0x7D: case 0x7E: case 0x7F:   // コマンド / トラック / セクタ / データ
        wd2793Write(p - 0x7C, (uint8_t)val); return;
      case 0x80:
      case 0x7B:  // Control Register
        if (val & 0x01) wd2793SelectDrive(0);
        else if (val & 0x02) wd2793SelectDrive(1);
        wd2793Write(4, (val & 0x10) ? 1 : 0);        // サイド
        break;
      default: return;
    }
  }
#endif
}

void clearStatus() {
  uiFillRect(48, 192, 303, 199, UI_BLACK);
}

void flashStatus(const char* msg, int waitMs = 0) {
  clearStatus();
  uiText(48, 192, msg, UI_WHITE, UI_BLACK);
  if (waitMs > 0) {
    delay(waitMs);
    clearStatus();
  }
}

void initSaveSystem() {
  uint8_t* base_ptr = (uint8_t*)ps_malloc(SAVE_BUFFER_SIZE * 5);
  if (base_ptr) {
    for (int i = 0; i < 5; i++) {
      psram_slots[i] = base_ptr + (SAVE_BUFFER_SIZE * i);
    }
  }
}

String getSavePath(int slotIdx) {
  String path = String(selectedRom);
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash != -1) {
    path = path.substring(lastSlash + 1);
  }

  int dotIndex = path.lastIndexOf('.');
  String base = (dotIndex != -1) ? path.substring(0, dotIndex) : path;

  return SD_ROOT "/ROM/SAVE/" + base + String(slotIdx + 1) + ".sav";
}

void syncGametoPSRAM(int idx) {
  if (psram_slots[idx] == NULL) return;
  uint8_t* p = psram_slots[idx];

  // 1. RAM (64KB)
  for (int i = 0; i < 8; i++) memcpy(p + i * 0x2000, slot3_page[i], 0x2000);
  p += 65536;

  // 2. CPU / System State
  tempState.AF = cpu.AF.W;
  tempState.BC = cpu.BC.W;
  tempState.DE = cpu.DE.W;
  tempState.HL = cpu.HL.W;
  tempState.IX = cpu.IX.W;
  tempState.IY = cpu.IY.W;
  tempState.PC = cpu.PC.W;
  tempState.SPtr = cpu.SPtr.W;
  tempState.AF1 = cpu.AF1.W;
  tempState.BC1 = cpu.BC1.W;
  tempState.DE1 = cpu.DE1.W;
  tempState.HL1 = cpu.HL1.W;
  tempState.I = cpu.I;
  tempState.R = cpu.R;
  tempState.IFF = cpu.IFF;
  tempState.IRequest = cpu.IRequest;
  tempState.primary_slot = slot_reg;
  for (int i = 0; i < 4; i++) tempState.rom_banks[i] = (uint8_t)carts[1].banks[i];

  memcpy(p, &tempState, sizeof(StateSaveData));
  p += sizeof(StateSaveData);

  // 3. VRAM(16KB)
  memcpy(p, vdp->framebuffer, VRAM_SIZE);
  p += VRAM_SIZE;

  // 4. VDP struct
  memcpy(p, vdp, VDP_SAVE_SIZE);
  p += VDP_SAVE_SIZE;

  // 5. PSG
  memcpy(p, (void*)psg_regs, 16);
  p += 16;
  memcpy(p, &psg_address, 1);
  p += 1;

  // 6. SCC
  memcpy(p, scc_waveform, 5 * 32);
  p += (5 * 32);  // 160 bytes
  memcpy(p, scc_f, sizeof(scc_f));
  p += sizeof(scc_f);  // 10 bytes
  memcpy(p, scc_v, sizeof(scc_v));
  p += sizeof(scc_v);  // 5 bytes
  memcpy(p, &scc_enable, 1);
  p += 1;
  memcpy(p, &carts[1].sccActive, 1);
  p += 1;
  memcpy(p, &psg2scc, 1);
  p += 1;

  // 7. スロット2・拡張スロット・FM-PAC(古いセーブデータには無いので印を付ける)
  memcpy(p, "S2v1", 4);
  p += 4;
  for (int i = 0; i < 4; i++) *p++ = (uint8_t)carts[2].banks[i];
  *p++ = carts[2].sccActive;
  *p++ = ssr3;
  memcpy(p, &fmpac, sizeof(fmpac));
  p += sizeof(fmpac);
}

void syncPSRAMtoGame(int idx) {
  if (psram_slots[idx] == NULL) return;
  uint8_t* p = psram_slots[idx];

  for (int i = 0; i < 8; i++) memcpy(slot3_page[i], p + i * 0x2000, 0x2000);
  p += 65536;

  StateSaveData state;
  memcpy(&state, p, sizeof(StateSaveData));
  p += sizeof(StateSaveData);

  cpu.AF.W = state.AF;
  cpu.BC.W = state.BC;
  cpu.DE.W = state.DE;
  cpu.HL.W = state.HL;
  cpu.IX.W = state.IX;
  cpu.IY.W = state.IY;
  cpu.PC.W = state.PC;
  cpu.SPtr.W = state.SPtr;
  cpu.AF1.W = state.AF1;
  cpu.BC1.W = state.BC1;
  cpu.DE1.W = state.DE1;
  cpu.HL1.W = state.HL1;
  cpu.I = state.I;
  cpu.R = state.R;
  cpu.IFF = state.IFF;
  cpu.IRequest = state.IRequest;
  if (cpu.IPeriod <= 0) cpu.IPeriod = 597;
  cpu.ICount = cpu.IPeriod;

  memcpy(vdp->framebuffer, p, VRAM_SIZE);
  p += VRAM_SIZE;

  uint8_t* tmp_framebuffer = vdp->framebuffer;
  uint8_t* tmp_active_raster = vdp->active_raster;
  uint8_t* tmp_visible_raster = vdp->visible_raster;

  memcpy(vdp, p, VDP_SAVE_SIZE);
  p += VDP_SAVE_SIZE;

  vdp->framebuffer = tmp_framebuffer;
  vdp->active_raster = tmp_active_raster;
  vdp->visible_raster = tmp_visible_raster;
  // Re-initialize colourmap from msx_palette (not saved as pointer any more)
  for (int i = 0; i < 16; i++) {
    vdp->colourmap[i] = 0xC0 | 
                        ((msx_palette[i].B >> 6) << 4) |
                        ((msx_palette[i].G >> 6) << 2) |
                         (msx_palette[i].R >> 6);
  }

  memcpy((void*)psg_regs, p, 16);
  p += 16;
  memcpy(&psg_address, p, 1);
  p += 1;
  memcpy(scc_waveform, p, 5 * 32);
  p += (5 * 32);
  memcpy(scc_f, p, sizeof(scc_f));
  p += sizeof(scc_f);
  memcpy(scc_v, p, sizeof(scc_v));
  p += sizeof(scc_v);
  memcpy(&scc_enable, p, 1);
  p += 1;
  memcpy(&carts[1].sccActive, p, 1);
  p += 1;
  memcpy(&psg2scc, p, 1);
  p += 1;

  slot_reg = state.primary_slot;
  for (int i = 0; i < 4; i++) {
    if (carts[1].mapper == MAPPER_RTYPE && i < 2) continue;
    if (carts[1].mapper == MAPPER_SCC || carts[1].mapper == MAPPER_FMPAC) break;
    cartSetBank(1, i, state.rom_banks[i]);
  }
  if (memcmp(p, "S2v1", 4) == 0) {             // スロット2・拡張スロット・FM-PAC
    p += 4;
    uint8_t b2[4];
    memcpy(b2, p, 4);
    p += 4;
    carts[2].sccActive = *p++;
    ssr3 = *p++;
    uint8_t* sram = fmpac.sram;
    memcpy(&fmpac, p, sizeof(fmpac));
    p += sizeof(fmpac);
    fmpac.sram = sram;                         // ポインタは今のものを使う
    for (int i = 0; i < 4; i++) {
      if (carts[2].mapper == MAPPER_RTYPE && i < 2) continue;
      if (carts[2].mapper == MAPPER_SCC || carts[2].mapper == MAPPER_FMPAC) break;
      cartSetBank(2, i, b2[i]);
    }
    if (fmpacSlot) fmpacUpdatePages();
  }
  updateMemoryMap();
}

void stateSavePSRAMtoSD(int idx) {
  String path = getSavePath(idx);
  if (!SD.exists(SD_ROOT)) SD.mkdir(SD_ROOT);
  if (!SD.exists(SD_ROOT "/ROM")) SD.mkdir(SD_ROOT "/ROM");
  if (!SD.exists(SD_ROOT "/ROM/SAVE")) SD.mkdir(SD_ROOT "/ROM/SAVE");

  File file = SD.open(path.c_str(), FILE_WRITE);
  if (file) {
    file.write(psram_slots[idx], SAVE_BUFFER_SIZE);
    file.close();
  }
}

void stateLoadSDtoPSRAM(int idx) {
  String path = getSavePath(idx);
  if (!SD.exists(path)) {
    flashStatus("NO SD FILE", 2000);
    return;
  }
  File file = SD.open(path.c_str(), FILE_READ);
  if (file) {
    file.read(psram_slots[idx], SAVE_BUFFER_SIZE);
    file.close();
  }
}

void clearKeyBuffer() {
  auto kb = PS2Controller.keyboard();
  while (kb->scancodeAvailable() > 0) {
    kb->getNextScancode(0);
    delay(10);
  }
}

void clearCanvasDouble() {
  vpClearRows(0, 199);
  displayMode = DisplayMode::GAME;
}

// dirPath(/ROM や /DISK)の中からファイルを選ぶ。サブフォルダにも入れる。
//   フォルダは「名前/」で一覧の上のほうに水色で出る(選んでいる項目は黄色)。Enter で入り、「../」か BackSpace で1つ上へ戻る。
//   PageUp/PageDown で1画面送り。返す値は dirPath からの相対パス(例 "Konami/Gradius(K).rom")。ESC は ""。
// extra: いちばん上のフォルダの一覧の先頭に足す項目(起動メニューの「なし」)。選ぶとその文字列を返す。
// current: 最初にカーソルを置く項目(相対パス。フォルダ付きならそのフォルダから始める)。
String showFileSelector(const char* title, const char* dirPath, const char* ext,
                        const std::vector<String>& extra, const String& current) {
  enum { K_UP, K_DIR, K_FILE, K_EXTRA };
  struct Item { String name; int kind; };
  auto kb = PS2Controller.keyboard();
  auto byName = [](const Item &a, const Item &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
  String lowerExt = String(ext);
  lowerExt.toLowerCase();
  String sub = "", want = current;                 // sub: 今のフォルダ(dirPath からの相対。先頭・末尾の / なし)
  bool isExtra = false;
  for (const String& e : extra) if (e == current) isExtra = true;
  int slash = current.lastIndexOf('/');
  if (!isExtra && slash >= 0) { sub = current.substring(0, slash); want = current.substring(slash + 1); }

  displayMode = DisplayMode::UI;

  for (;;) {                                       // フォルダを移るたびに一覧を作り直す
    String path = String(dirPath) + (sub == "" ? "" : "/" + sub);
    File dir = SD.open(path.c_str());
    if (!dir && sub != "") { sub = ""; want = ""; continue; }   // 前に選んだフォルダが無くなっていた
    if (!dir) {
      SD.mkdir(SD_ROOT);
      SD.mkdir(dirPath);
      dir = SD.open(dirPath);
    }
    std::vector<Item> dirs, files;
    while (dir) {
      File file = dir.openNextFile();
      if (!file) break;
      String name = file.name();
      int s = name.lastIndexOf('/');                // 版によってはフルパスが返る
      if (s >= 0) name = name.substring(s + 1);
      if (name.length() && name[0] != '.' && name != "System Volume Information") {
        if (file.isDirectory()) {
          if (!(sub == "" && name == "SAVE" && strcmp(dirPath, SD_ROOT "/ROM") == 0))   // ステートセーブ置き場は出さない
            dirs.push_back({ name, K_DIR });
        } else {
          String lowerName = name;
          lowerName.toLowerCase();
          if (lowerName.endsWith(lowerExt) && !(sub == "" && (name == "MSX.ROM" || name == "DISK.ROM")))
            files.push_back({ name, K_FILE });
        }
      }
      file.close();
    }
    if (dir) dir.close();
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    std::vector<Item> items;
    if (sub == "") for (const String& e : extra) items.push_back({ e, K_EXTRA });
    else items.push_back({ "..", K_UP });
    items.insert(items.end(), dirs.begin(), dirs.end());
    items.insert(items.end(), files.begin(), files.end());
    if (items.empty()) return "";

    int cursor = 0, topIndex = 0;
    const int maxRows = 17;
    for (int i = 0; i < (int)items.size(); i++)
      if (items[i].name == want) { cursor = i; break; }
    bool isBreak = false, redrawn = true, changeDir = false;
    // 画面全体を塗るのはフォルダに入ったときだけ。カーソルを動かしたら前と今の2行、
    // スクロールしたら一覧の行だけ描き直す(画面の行に直接書くので、全部塗り直すとちらつく)
    int shownTop = -1, shownCursor = -1;
    auto drawItem = [&](int idx) {
      int y = 27 + (idx - topIndex) * 9;
      uiFillRect(56, y - 1, 319, y + 7, UI_BLUE);
      if (idx < 0 || idx >= (int)items.size()) return;
      const Item &it = items[idx];
      bool plain = it.kind == K_FILE || it.kind == K_EXTRA;
      String label = plain ? it.name : it.name + "/";
      if (label.length() > 32) label = label.substring(0, 32);
      uint8_t fg = (idx == cursor) ? UI_YELLOW : (plain ? UI_WHITE : UI_CYAN);   // 選択中=黄、フォルダ=水色
      uiText(64, y, label.c_str(), fg, UI_BLUE);
    };

    while (!changeDir) {
      if (redrawn) {
        if (cursor < topIndex) topIndex = cursor;
        if (cursor >= topIndex + maxRows) topIndex = cursor - maxRows + 1;
        if (shownTop < 0) {
          uiFillRect(0, 0, 319, 199, UI_BLUE);
          uiText(48, 5, title, UI_WHITE, UI_BLUE);
          String shown = path + "/";
          if (shown.length() > 34) shown = "..." + shown.substring(shown.length() - 31);
          uiText(48, 15, shown.c_str(), UI_CYAN, UI_BLUE);
        }
        if (shownTop < 0 || topIndex != shownTop) {
          for (int i = 0; i < maxRows; ++i) drawItem(topIndex + i);
        } else if (cursor != shownCursor) {
          drawItem(shownCursor);
          drawItem(cursor);
        }
        shownTop = topIndex;
        shownCursor = cursor;
        redrawn = false;
      }

      {
        while (!changeDir) {
          // キーボード、なければポート1のコントローラ(JoyKeys.hpp)
          int sc;
          if (kb && kb->scancodeAvailable() > 0) {
            sc = kb->getNextScancode(0);
            if (sc == 0xF0) {
              isBreak = true;
              continue;
            }
            if (sc == 0xE0) continue;
          } else {
            bool e0;
            sc = joyMenuScancode(joyMenuPoll(), &e0);
            if (!sc) break;
          }

          if (!isBreak) {
            int n = (int)items.size();
            if (sc == 0x75 && cursor > 0) {
              cursor--;
              redrawn = true;
            } else if (sc == 0x72 && cursor < n - 1) {
              cursor++;
              redrawn = true;
            } else if (sc == 0x7D) {  // PageUp
              cursor = cursor - maxRows < 0 ? 0 : cursor - maxRows;
              redrawn = true;
            } else if (sc == 0x7A) {  // PageDown
              cursor = cursor + maxRows >= n ? n - 1 : cursor + maxRows;
              redrawn = true;
            } else if (sc == 0x76) {  // ESC
              clearCanvasDouble();
              clearKeyBuffer();
              return "";
            } else if ((sc == 0x66 && sub != "") || (sc == 0x5A && items[cursor].kind == K_UP)) {  // 1つ上へ
              int p = sub.lastIndexOf('/');
              want = p >= 0 ? sub.substring(p + 1) : sub;  // 出てきたフォルダにカーソルを置く
              sub = p >= 0 ? sub.substring(0, p) : "";
              changeDir = true;
            } else if (sc == 0x5A && items[cursor].kind == K_DIR) {  // フォルダに入る
              sub = sub == "" ? items[cursor].name : sub + "/" + items[cursor].name;
              want = "";
              changeDir = true;
            } else if (sc == 0x5A) {  // ENTER
              String res = (items[cursor].kind == K_EXTRA || sub == "") ? items[cursor].name : sub + "/" + items[cursor].name;
              clearCanvasDouble();
              return res;
            }
          }
          isBreak = false;
        }
      }
      if (!changeDir) delay(10);
    }
  }
}

// 実行中の F9: ディスクの入れ替え(今入っているディスクのフォルダから選び始める)
void handleDiskSelect(int drive) {
  char title[32];
  sprintf(title, "--- SELECT %c (ESC TO EXIT) ---", (drive == 0) ? 'A' : 'B');
  String cur = strncmp(drive_filepath[drive], SD_ROOT "/DISK/", sizeof(SD_ROOT "/DISK/") - 1) == 0 ? String(drive_filepath[drive] + sizeof(SD_ROOT "/DISK/") - 1) : String("");
  String selected = showFileSelector(title, SD_ROOT "/DISK", ".DSK", {}, cur);
  if (selected != "") {
    if (loadSdToPsram(drive, SD_ROOT "/DISK/" + selected)) {
      flashStatus("LOAD SUCCESS", 1000);
    } else {
      flashStatus("LOAD ERROR!", 2000);
    }
  }
}

// ---------------------------------------------------------------------------
//  起動メニュー(MSX2版と同じ形)
//    Slot1 / Slot2 / Disk BIOS / DiskA / DiskB / Mouse / Joy B1〜B3 / Run を ↑↓ で選び、Enter で変更・起動。
//    ポート1のコントローラでも操作できる(↑↓ 選択、←→ 1画面送り、ボタン1 = 決定、ボタン2 = 取り消し)。
//    マウスをつないだポートのコントローラはキーボードとして働く: 方向 = カーソルキー、
//    ボタン1〜3 = Joy B1〜B3 で決めたキー(Mouse = NONE のときは働かない。JoyKeys.hpp)。
//    Joy B1〜B3: Enter で黄色になり、次に押したキーを割り当てる。F6〜F12 で取り消し、BackSpace で NONE。
//     SD の /MSX1/JOYKEY.CFG に保存(PC-88 版と同じ操作)。
//    選んでいる項目は反転表示。
//    (Arduino が関数の宣言を先頭に自動で入れるので、構造体は使わずに変数で持つ)
// ---------------------------------------------------------------------------
static const char* ITEM_NONE = "<NONE>";
static const char* ITEM_SCC = "<SCC>";       // ROM の無い SCC カートリッジ
static const char* ITEM_FMPAC = "<FM-PAC>";  // FM-PAC(/FMPAC.ROM があるとき。スロット2のみ)
static String bootSlot[2] = { "", "" };       // /ROM からの相対パス、ITEM_SCC / ITEM_FMPAC("" = なし)
static String bootDisk[2] = { "", "" };       // /DISK からの相対パス
static bool bootDiskBios = true, bootDiskBiosManual = false;
static int bootMouse = 0;                     // 0 = なし、1 / 2 = ジョイスティックポート

// ジョイスティックのボタン1〜3のキー(PC-88 版の Joy B1〜B3 と同じ。SD の /MSX1/JOYKEY.CFG に3バイト。
// 前の版の1バイトのファイルはボタン3のキーとして読む)
static const char* JOYKEY_FILE = SD_ROOT "/JOYKEY.CFG";
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

static bool isRomItem(const String& s) { return s != "" && s != ITEM_SCC && s != ITEM_FMPAC; }

// Disk BIOS の既定: カートリッジ(ROM)だけで起動するときは外す(ワークエリアを取られて動かないゲームが
// あるため)。ディスクを入れたとき、ROM を挿さないとき(BASIC)は入れる。Enter で切り替えたらそのまま。
static void bootAutoDiskBios() {
  if (bootDiskBiosManual) return;
  bootDiskBios = !(isRomItem(bootSlot[0]) || isRomItem(bootSlot[1])) || bootDisk[0] != "" || bootDisk[1] != "";
}

static String slotLabel(const String& s) {
  if (s == ITEM_SCC) return "SCC (NO ROM)";
  if (s == ITEM_FMPAC) return "FM-PAC";
  return s;
}

// 起動メニューの各行に出ている内容(選択の状態 + 文字)を覚えておき、変わった行だけ描き直す。
// 画面の行に直接書くので、毎回全部塗り直すとちらつく。0〜9 = 項目、10 = 案内、11 = メッセージ
static char menuShown[12][56];
static bool menuValid = false;
static void bootMenuInvalidate() { menuValid = false; }   // ファイル選択などで画面を上書きしたあと

// capture: カーソルの Joy B1〜B3 の行がキーを待っている(黄色で表示)
static void bootMenuDraw(int cursor, bool hasDiskRom, const char* msg, bool capture) {
  static const int X0 = 16, X1 = 303, Y0 = 28, DY = 13;
  static const char* mouseName[3] = { "NONE", "PORT 1", "PORT 2" };
  if (!menuValid) {
    uiFillRect(0, 0, 319, 199, UI_BLUE);
    uiText(X0, 12, "MSX BOOT MENU", UI_WHITE, UI_BLUE);
    memset(menuShown, 0, sizeof(menuShown));
    menuValid = true;
  }
  for (int i = 0; i < 10; i++) {
    char line[48];
    switch (i) {
      case 0: snprintf(line, sizeof(line), "Slot1    : %.24s", slotLabel(bootSlot[0]).c_str()); break;
      case 1: snprintf(line, sizeof(line), "Slot2    : %.24s", slotLabel(bootSlot[1]).c_str()); break;
      case 2: snprintf(line, sizeof(line), "Disk BIOS: %s", !hasDiskRom ? "OFF (NO DISK.ROM)" : bootDiskBios ? "ON" : "OFF"); break;
      case 3: snprintf(line, sizeof(line), "DiskA    : %.24s", bootDisk[0].c_str()); break;
      case 4: snprintf(line, sizeof(line), "DiskB    : %.24s", bootDisk[1].c_str()); break;
      case 5: snprintf(line, sizeof(line), "Mouse    : %s", mouseName[bootMouse]); break;
      case 6: case 7: case 8:
        snprintf(line, sizeof(line), "Joy B%d   : %s", i - 5, (capture && i == cursor) ? "PRESS A KEY..." : msxKeyName(joyKey[i - 6]));
        break;
      case 9: snprintf(line, sizeof(line), "Run"); break;
    }
    bool sel = (i == cursor);
    char shown[56];                                // 1 文字目 = 状態(N 通常 / S 選択 / C キー待ち)
    snprintf(shown, sizeof(shown), "%c%s", sel ? (capture ? 'C' : 'S') : 'N', line);
    if (strcmp(shown, menuShown[i]) == 0) continue;
    strcpy(menuShown[i], shown);
    int y = Y0 + i * DY;
    uint8_t bg = !sel ? UI_BLUE : capture ? UI_YELLOW : UI_WHITE;
    uiFillRect(X0 - 4, y - 3, X1, y + 9, bg);
    uiText(X0, y, line, sel ? UI_BLUE : UI_WHITE, bg);
  }
  const char* help = capture ? "PRESS A KEY (F6-F12:CANCEL)"
                   : (cursor >= 6 && cursor <= 8) ? "ENTER:SET KEY BS:NONE (MOUSE PAD)"
                                                  : "UP/DOWN:SELECT ENTER:CHANGE/RUN";
  const char* texts[2] = { help, msg ? msg : "" };
  for (int k = 0; k < 2; k++) {
    if (strcmp(texts[k], menuShown[10 + k]) == 0) continue;
    snprintf(menuShown[10 + k], sizeof(menuShown[0]), "%s", texts[k]);
    int y = k ? 200 - 20 : 200 - 34;
    uiFillRect(X0 - 4, y - 1, 319, y + 8, UI_BLUE);
    uiText(X0, y, texts[k], k ? UI_WHITE : UI_CYAN, UI_BLUE);
  }
}

// 起動する内容を選ぶ(Run で戻る)。ディスクは読み込みまで行う(失敗したらメニューに戻る)
static void bootMenu(bool hasDiskRom, bool hasFmPac) {
  auto kb = PS2Controller.keyboard();
  std::vector<int> items = { 0, 1 };
#ifdef USE_FDC
  if (hasDiskRom) { items.push_back(2); items.push_back(3); items.push_back(4); }
#endif
  items.push_back(5);
  items.push_back(6);
  items.push_back(7);
  items.push_back(8);
  items.push_back(9);
  int pos = 0;
  const char* msg = nullptr;
  bool isBreak = false, isE0 = false, redraw = true, capture = false;
  displayMode = DisplayMode::UI;
  clearKeyBuffer();
  bootMenuInvalidate();
  for (;;) {
    if (redraw) { bootAutoDiskBios(); bootMenuDraw(items[pos], hasDiskRom, msg, capture); redraw = false; }
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
      if (capture) {                           // ジョイスティックのボタンに割り当てるキー
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
          bootMenuInvalidate();                  // ファイル選択の画面で上書きされる
          String sel = showFileSelector(it ? "-- SELECT ROM FOR SLOT 2 --" : "-- SELECT ROM FOR SLOT 1 --", SD_ROOT "/ROM", ".ROM", extra,
                                        bootSlot[it] == "" ? String(ITEM_NONE) : bootSlot[it]);
          if (sel != "") bootSlot[it] = (sel == ITEM_NONE) ? "" : sel;
        } else if (it == 2) {
          bootDiskBios = !bootDiskBios;
          bootDiskBiosManual = true;
        } else if (it == 3 || it == 4) {
          int d = it - 3;
          bootMenuInvalidate();
          String sel = showFileSelector(d ? "-- SELECT DISK FOR DRIVE B --" : "-- SELECT DISK FOR DRIVE A --",
                                        SD_ROOT "/DISK", ".DSK", { ITEM_NONE }, bootDisk[d] == "" ? String(ITEM_NONE) : bootDisk[d]);
          if (sel != "") bootDisk[d] = (sel == ITEM_NONE) ? "" : sel;
        } else if (it == 5) {
          bootMouse = (bootMouse + 1) % 3;
        } else if (it >= 6 && it <= 8) {
          capture = true;                      // 次に押したキーをそのボタンに割り当てる
        } else if (it == 9) {
          bootAutoDiskBios();
          bootMenuDraw(it, hasDiskRom, "LOADING...", false);
          msg = nullptr;
#ifdef USE_FDC
          if (hasDiskRom && bootDiskBios) {
            for (int d = 0; d < 2 && !msg; d++)
              if (bootDisk[d] != "" && !loadSdToPsram(d, SD_ROOT "/DISK/" + bootDisk[d])) msg = d ? "DISKB LOAD ERROR" : "DISKA LOAD ERROR";
          }
#endif
          if (!msg) {
            // ステートセーブのファイル名に使う ROM(スロット1、無ければスロット2)
            selectedRom = isRomItem(bootSlot[0]) ? SD_ROOT "/ROM/" + bootSlot[0] : isRomItem(bootSlot[1]) ? SD_ROOT "/ROM/" + bootSlot[1] : "";
            return;
          }
        }
        displayMode = DisplayMode::UI;           // ファイル選択から戻るとゲーム表示に切り替わっているので戻す
        clearKeyBuffer();
        redraw = true;
      }
    }
    delay(10);
  }
}

void updateKeyboard() {
  static bool isBreak = false;
  static bool isE0 = false;
  static bool isShift = false;
  static bool f6_held = false;
  static bool f7_held = false;
  static bool f8_held = false;
  static bool f9_held = false;
  static bool f10_held = false;
  static bool f11_held = false;
  static bool f12_held = false;

  auto kb = PS2Controller.keyboard();
  if (!kb) return;
  int count = kb->scancodeAvailable();
  if (count <= 0) return;

  while (count--) {
    int sc = kb->getNextScancode(0);
    if (sc == -1) break;
    if (sc == 0xF0) {
      isBreak = true;
      continue;
    }
    if (sc == 0xE0) {
      isE0 = true;
      continue;
    }
    if (sc == 0x12 || sc == 0x59) {
      isShift = !isBreak;
    }

    if (isPaused && !isBreak && !isE0 && currentSaveMode != SaveMode::NONE) {
      int slot = -1;
      if (sc == 0x16) slot = 0;       // 1
      else if (sc == 0x1E) slot = 1;  // 2
      else if (sc == 0x26) slot = 2;  // 3
      else if (sc == 0x25) slot = 3;  // 4
      else if (sc == 0x2E) slot = 4;  // 5

      if (slot != -1) {
        char msg[32];
        switch (currentSaveMode) {
          case SaveMode::GAME_TO_PSRAM:
            syncGametoPSRAM(slot);
            sprintf(msg, "SLOT %d: QUICK SAVED", slot + 1);
            break;
          case SaveMode::PSRAM_TO_GAME:
            syncPSRAMtoGame(slot);
            sprintf(msg, "SLOT %d: RESTORED", slot + 1);
            break;
          case SaveMode::PSRAM_TO_SD:
            stateSavePSRAMtoSD(slot);
            sprintf(msg, "SLOT %d: PSRAM -> SD", slot + 1);
            break;
          case SaveMode::SD_TO_PSRAM:
            stateLoadSDtoPSRAM(slot);
            sprintf(msg, "SLOT %d: SD -> PSRAM", slot + 1);
            break;
        }
        flashStatus(msg, 1000);
        flashStatus("PAUSED!", 0);
        currentSaveMode = SaveMode::NONE;
        isBreak = false;
        isE0 = false;
        continue;
      }
    }

    if (sc == 0x0B) {  // F6
      if (!isBreak && !f6_held) {
        f6_held = true;
        if (isShift) {
          // todo anything
        } else {
          // todo anything
        }
      } else if (isBreak) f6_held = false;
    } else if (sc == 0x83) {  // F7
      if (!isBreak && !f7_held) {
        f7_held = true;
        if (isShift) {
          // todo anything
        } else {
        }
      } else if (isBreak) f7_held = false;
    } else if (sc == 0x0A) {  // F8
      if (!isBreak && !f8_held) {
        f8_held = true;
        if (!isShift) {
          if (!psg2scc && !scc_enable) {
            psg2scc = true;
            flashStatus("SCC ON", 1000);
          } else {
            psg2scc = false;
            scc_enable = false;
            for (int i = 0; i < 5; i++) scc_ch[i]->setVolume(0);
            flashStatus("SCC OFF", 1000);
          }
        } else {
          if (vdp->limit_sprites == 0) {
            vdp->limit_sprites = 1;
            flashStatus("LIMIT SPRITES", 1000);
          } else {
            vdp->limit_sprites = 0;
            flashStatus("NO LIMIT SPRITES", 1000);
          }
        }
      } else if (isBreak) f8_held = false;
    } else if (sc == 0x01) {  // F9
#ifdef USE_FDC
      if (!isBreak && !f9_held) {
        f9_held = true;
        if (isPaused) {
          if (!isShift) {
            handleDiskSelect(0);
            // 選択画面がキーを離した信号を読んでしまうので、押したままの扱いを消す
            // (残すと次の F9 が1回目だけ無視される)
            f9_held = isShift = false;
            for (int i = 0; i < 12; i++) msx_key_matrix[i] = 0xFF;
          } else {
            savePsramToSd(0);
          }
        }
      } else if (isBreak) {
        f9_held = false;
      }
#endif
    } else if (sc == 0x09) {  // F10
      if (!isBreak && !f10_held) {
        f10_held = true;
        if (!isShift) {
          if (isPaused) {
            currentSaveMode = SaveMode::SD_TO_PSRAM;
            flashStatus("LOAD SD->PSRAM: SELECT 1-5", 0);
          }
        } else {
          if (isPaused) {
            currentSaveMode = SaveMode::PSRAM_TO_SD;
            flashStatus("SAVE PSRAM->SD: SELECT 1-5", 0);
          }
        }

      } else if (isBreak) f10_held = false;
    } else if (sc == 0x78) {  // F11
      if (!isBreak && !f11_held) {
        f11_held = true;
        if (!isShift) {
          if (isPaused) {
            currentSaveMode = SaveMode::PSRAM_TO_GAME;
            flashStatus("RESTORE PSRAM: SELECT 1-5", 0);
          }
        } else {
          if (isPaused) {
            currentSaveMode = SaveMode::GAME_TO_PSRAM;
            flashStatus("QUICK SAVE: SELECT 1-5", 0);
          }
        }
      } else if (isBreak) f11_held = false;
    } else if (sc == 0x07) {  // F12
      if (!isBreak && !f12_held) {
        f12_held = true;

        if (isShift) {
          ESP.restart();
        } else {
          isPaused = !isPaused;
          currentSaveMode = SaveMode::NONE;
          if (!isPaused) {
            clearStatus();
            currentSaveMode = SaveMode::NONE;
          } else {
            flashStatus("PAUSED!", 0);
          }
        }
      } else if (isBreak) f12_held = false;
    }

    // KeyboardSystem.hpp の表で MSX のマトリクスへ(E0 付き: カーソル・Home・Ins・Del・End・
    // PageUp/Down・テンキーの / と Enter・右 Alt など)
    int row = -1, bit = -1;
    uint8_t res = isE0 ? key_map_e0[sc & 0xFF] : key_map[sc & 0xFF];
    if (res != 0xFF) {
      row = res >> 4;
      bit = res & 0x0F;
    }

    if (row >= 0 && row < 12) {
      if (isBreak) msx_key_matrix[row] |= (1 << bit);
      else msx_key_matrix[row] &= ~(1 << bit);
    }

    isBreak = false;
    isE0 = false;
  }
}

// 1バイト: bit7 = 2P、bit0〜5 = ↑↓←→ トリガA/B、bit6 = ボタン3(0 = 押下)
// マウスをつないだポートのコントローラは、ゲームには渡さずキーを押す(JoyKeys.hpp)
void IRAM_ATTR updateController() {
  uint8_t j1 = 0x7F, j2 = 0x7F;
  while (Serial1.available() > 0) {
    joy_bits = Serial1.read();
    if (joy_bits & 0x80) j2 = joy_bits & 0x7F;
    else j1 = joy_bits & 0x7F;
  }
  joy_bits1 = (mousePort == 1 ? 0x7F : j1) & 0x3F;
  joy_bits2 = (mousePort == 2 ? 0x7F : j2) & 0x3F;
  joyKeysApply(mousePort == 2 ? j2 : j1, mousePort != 0, msx_key_matrix);
}

// マッパーはファイル名のタグで決める(自動判別はしない)
MapperType detectMapper(const char* filename, uint32_t size) {
  if (size <= 0x8000) return MAPPER_NONE;
  const char* base = strrchr(filename, '/');  // フォルダ名のタグは見ない
  String up = String(base ? base + 1 : filename);
  up.toUpperCase();
  if (up.indexOf("(A16)") >= 0) return MAPPER_ASCII16;
  if (up.indexOf("(A8)") >= 0 || up.indexOf("(A)") >= 0) return MAPPER_ASCII8;
  if (up.indexOf("(K)") >= 0) return MAPPER_KONAMI;
  if (up.indexOf("(R)") >= 0) return MAPPER_RTYPE;
  return MAPPER_NONE;
}

// ---------------------------------------------------------------------------
//  FM 音源(FM-PAC の YM2413)。MSX2 版と同じ。FM に一度も書き込まれていない間は合成しない
//  (合成は1サンプル約12us、16384Hz でコア1の約20%を使う)。
// ---------------------------------------------------------------------------
class YM2413Generator : public fabgl::WaveformGenerator {
private:
  OPLL* opll = nullptr;
  int16_t sampleBuffer[BUFFER_SIZE];
  volatile int head = 0, tail = 0;
  int lastSample = 0;
  static const int LP_SHIFT = 2;           // 1次IIRローパス
  // FabGL のミキサーは上限で止めないので、emu2413 の出力(全ch で ±25000 程度)を >>6 して ±250 で止める
  static const int FM_SHIFT = 6;
  static const int FM_CLIP = 250;
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
      lpState += (raw - lpState) >> LP_SHIFT;
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
YM2413Generator* fmGen = nullptr;

void fmWrite(int port, uint8_t v) {
  if (fmGen) fmGen->writePort(port, v);
}

// ---------------------------------------------------------------------------
//  カートリッジの読み込み(スロット s)
// ---------------------------------------------------------------------------
static bool loadCartridge(int s, const String& path) {
  File romFile = SD.open(path.c_str());
  if (!romFile) return false;
  uint32_t size = romFile.size();
  Serial.printf("Loading Game ROM (slot %d): %s (%d bytes)\n", s, path.c_str(), size);
  // 'AB' ヘッダの確認。先頭(バンク0)に加え、R-TYPEのように
  // バンク23の先頭にヘッダを持つROMもあるため両方を見る。
  uint8_t header0[2] = { 0, 0 }, header23[2] = { 0, 0 };
  romFile.seek(0);
  romFile.read(header0, 2);
  romFile.seek(384 * 1024 - 16 * 1024);
  romFile.read(header23, 2);
  if (!((header0[0] == 'A' && header0[1] == 'B') || (header23[0] == 'A' && header23[1] == 'B'))) {
    Serial.printf("Warning: No 'AB' header in [%s]! This might be a raw data or corrupted.\n", path.c_str());
    romFile.close();
    return false;
  }
  uint32_t rs = 0x2000;
  while (rs < size) rs <<= 1;
  uint8_t* rom = (uint8_t*)ps_malloc(rs);
  if (!rom) { romFile.close(); return false; }
  memset(rom, 0xFF, rs);                     // 2のべき乗に切り上げた残りは空きROM
  romFile.seek(0);
  romFile.read(rom, size);
  romFile.close();
  MapperType m = detectMapper(path.c_str(), size);
  const char* mapperNames[] = { "NONE", "KONAMI", "ASCII8", "ASCII16", "RTYPE", "SCC", "FMPAC" };
  Serial.printf("Mapper: %s\n", mapperNames[m]);
  cartInsert(s, rom, rs, m);
  return true;
}

// ---------------------------------------------------------------------------
//  FM-PAC: /MSX1/FMPAC.ROM をスロット s に。SRAM(8KB)は SD の /MSX1/FMPAC.PAC に保存する
//  (openMSX などと同じ形: "PAC2 BACKUP DATA" の 16 バイトのあとに 1FFEh バイト)。
// ---------------------------------------------------------------------------
static const char* FMPAC_SRAM_FILE = SD_ROOT "/FMPAC.PAC";
static const char FMPAC_SRAM_HEADER[17] = "PAC2 BACKUP DATA";

static bool loadFmPac(int s) {
  File f = SD.open(SD_ROOT "/FMPAC.ROM");
  if (!f) return false;
  uint32_t size = f.size();
  uint32_t rs = 0x4000;
  while (rs < size) rs <<= 1;
  uint8_t* rom = (uint8_t*)ps_malloc(rs);
  fmpac.sram = (uint8_t*)ps_malloc(0x2000);
  if (!rom || !fmpac.sram) { f.close(); return false; }
  memset(rom, 0xFF, rs);
  f.read(rom, size);
  f.close();
  memset(fmpac.sram, 0xFF, 0x2000);
  File pf = SD.open(FMPAC_SRAM_FILE, FILE_READ);
  if (pf) {
    char h[16];
    if (pf.read((uint8_t*)h, 16) == 16 && memcmp(h, FMPAC_SRAM_HEADER, 16) == 0) {
      pf.read(fmpac.sram, 0x1FFE);
      Serial.println("FM-PAC SRAM loaded");
    }
    pf.close();
  }
  cartInsert(s, rom, rs, MAPPER_FMPAC);
  if (!fmGen) {
    fmGen = new YM2413Generator();
    soundGenerator.attach(fmGen);
    fmGen->enable(true);
  }
  Serial.printf("Slot %d: FM-PAC (%u bytes)\n", s, (unsigned)size);
  return true;
}

// 書き込みが止まって 2 秒たったら SD に保存する(loop から呼ぶ)
static void saveFmPacSramIfChanged() {
  static uint32_t lastWrites = 0, changedAt = 0;
  static bool pending = false;
  if (!fmpacSlot || !fmpac.sram) return;
  uint32_t w = fmpacSramWrites;
  if (w != lastWrites) { lastWrites = w; changedAt = millis(); pending = true; return; }
  if (!pending || millis() - changedAt < 2000) return;
  pending = false;
  File f = SD.open(FMPAC_SRAM_FILE, FILE_WRITE);
  if (!f) return;
  f.write((const uint8_t*)FMPAC_SRAM_HEADER, 16);
  f.write(fmpac.sram, 0x1FFE);
  f.close();
}

// ---------------------------------------------------------------------------
//  PS/2 マウス(起動メニューで選んだジョイスティックポートにつなぐ)
// ---------------------------------------------------------------------------
static bool mouseOn = false;
static void updateMouse() {
  if (!mouseOn) return;
  auto m = PS2Controller.mouse();
  if (!m) return;
  while (m->deltaAvailable()) {
    MouseDelta d;
    if (!m->getNextDelta(&d, 0)) break;
    mouse_x += d.deltaX;                     // 右が +
    mouse_y += d.deltaY;                     // 上が +
    mouse_buttons = (d.buttons.left ? 1 : 0) | (d.buttons.right ? 2 : 0);
  }
}

//M1WAIT is closer to the original hardware behavior and results in a lower CPU load.
#define CPU_CYCLE_M1WAIT 205 // -10%
//#define CPU_CYCLE_M1WAIT 210 // -8%
//#define CPU_CYCLE_M1WAIT 215 // -6%
#define CPU_CYCLE_NOWAIT 228

void IRAM_ATTR cpuTask(void* pvParameters) {
  const int64_t frameTimeUs = 16684;  // NTSC 59.94FPS
  int64_t nextFrameTime = esp_timer_get_time();
  int64_t now;
  int64_t waitTime;

  while (1) {
    if (isPaused) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      nextFrameTime = esp_timer_get_time();
      continue;
    }

    nextFrameTime += frameTimeUs;

    for (int i = 0; i < 262; ++i) {
      fdcLines++;
      ExecZ80(&cpu, CPU_CYCLE_M1WAIT);
      if (i == 192) {
        frameReady = true;
      }
      if (tms9918a_irq_pending(vdp) && (cpu.IFF & IFF_1)) {
        IntZ80(&cpu, 0x38);
      }
    }
    now = esp_timer_get_time();
    waitTime = nextFrameTime - now;

    if (waitTime > 0) {
      if (waitTime >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(waitTime / 1000));
      }
      while (esp_timer_get_time() < nextFrameTime) {
        asm volatile("nop");
      }
    }
  }
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

  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1, fabgl::KbdMode::NoVirtualKeys);
  PS2Controller.keyboard()->begin(GPIO_NUM_33, GPIO_NUM_32, false, false);

  auto kb = PS2Controller.keyboard();
  for (int i = 0; i < 12; i++) {
    msx_key_matrix[i] = 0xFF;
  }
  initKeyMap(KEYBORD_MAP);

  initSaveSystem();

  // ---- 画面: 内部RAMの大物より先に確保する ----
  // FabGLはビューポート(320x200=64,000B)を確保するとき、内部RAMの最大連続空きを
  // 約40KB残そうとし、それを割り込む時点で確保を打ち切る(X1で155行止まりを実測)。
  // だからスロットRAMより先に取る。
  printHeap("before VGA");
  DisplayController.begin();
  DisplayController.setResolution(VGA_320x200_70Hz, -1, -1, false);
  printHeap("after VGA");
  {
    bool ok = true;
    for (int y = 0; y < 200; y++) {
      vpRow[y] = DisplayController.getScanline(y);
      if (!vpRow[y] || ((uintptr_t)vpRow[y] & 3)) {
        Serial.printf("VIEWPORT row %d is NULL or unaligned (%p)\n", y, vpRow[y]);
        ok = false;
        break;
      }
    }
    if (!ok) {
      Serial.println("Viewport allocation FAILED!");
      while (1) delay(1000);
    }
    vpReady = true;
    vpClearRows(0, 199);
    // UIの色定数とVDPのカラーマップは同期ビット=0xC0決め打ちなので確認しておく
    uint8_t rawBlack = DisplayController.createRawPixel(fabgl::RGB222(0, 0, 0));
    Serial.printf("VIEWPORT: row0=%p row1=%p stride=%d rawBlack=%02X (expected C0)\n",
                  vpRow[0], vpRow[1], (int)(vpRow[1] - vpRow[0]), rawBlack);
  }

  // スロット0(BIOS 32KB)は連続で必要。FabGLは最大連続空きを約40KB残すので入る。
  slot_mem[0] = (uint8_t*)heap_caps_malloc(0x8000, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (slot_mem[0]) {
    Serial.println("Slot 0 (Fast RAM) allocated in Internal SRAM.");
  } else {
    slot_mem[0] = (uint8_t*)ps_malloc(0x8000);
    Serial.println("Slot 0 fell back to PSRAM (slower)!");
  }
  if (slot_mem[0]) {
    memset(slot_mem[0], 0, 0x8000);
  } else {
    Serial.println("Slot 0 RAM Allocation Failed!");
    while (1)
      ;
  }

  slot_mem[1] = (uint8_t*)ps_malloc(0x10000);
  if (slot_mem[1]) {
    memset(slot_mem[1], 0xFF, 0x10000);
    Serial.println("Slot 1 allocated in PSRAM.");
  } else {
    Serial.println("Slot 1 RAM Allocation Failed!");
    while (1)
      ;
  }
  slot_mem[2] = (uint8_t*)ps_malloc(0x10000);
  if (slot_mem[2]) {
    memset(slot_mem[2], 0xFF, 0x10000);
    Serial.println("Slot 2 allocated in PSRAM.");
  } else {
    Serial.println("Slot 2 RAM Allocation Failed!");
    while (1)
      ;
  }
  // スロット3(RAM 64KB)は8KB×8ページで取る。ビューポート確保後は64KBの連続領域が
  // まず残らないため。Z80からは msx1_rd_map/msx1_wr_map(8KB単位)経由なので連続である必要はない。
  {
    int internalPages = 0;
    for (int i = 0; i < 8; i++) {
      slot3_page[i] = (uint8_t*)heap_caps_malloc(0x2000, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      if (slot3_page[i]) internalPages++;
      else slot3_page[i] = (uint8_t*)ps_malloc(0x2000);
      if (!slot3_page[i]) {
        Serial.println("Slot 3 RAM Allocation Failed!");
        while (1)
          ;
      }
      memset(slot3_page[i], 0, 0x2000);
    }
    Serial.printf("Slot 3 RAM: %d/8 pages in Internal SRAM%s\n", internalPages,
                  internalPages == 8 ? "." : " (rest in PSRAM, slower)!");
  }
  printHeap("after slots");

  vdp = tms9918a_create();
  tms9918a_reset(vdp);
  vdp->active_raster = vdp->rasterbuffer0;
  vdp->visible_raster = vdp->rasterbuffer1;

  // Initialize RGB222 colourmap from msx_palette (00BBGGRR format)
  for (int i = 0; i < 16; i++) {
    vdp->colourmap[i] = 0xC0 | 
                        ((msx_palette[i].B >> 6) << 4) |
                        ((msx_palette[i].G >> 6) << 2) |
                         (msx_palette[i].R >> 6);
  }

  soundGenerator.attach(&psgGen);
  psgGen.enable(true);
  psgGen.setVolume(100);

  for (int i = 0; i < 32; i++) {
    scc_waveform[0][i] = Pseudo_Vocal[i];
    scc_waveform[1][i] = Metallic_Lead[i];
    scc_waveform[2][i] = Dirty_Bass[i];
    scc_waveform[3][i] = Warm_Body_PhaseShift[i];
  }

  for (int i = 0; i < 5; i++) {
    scc_ch[i] = new SCCGenerator();
    soundGenerator.attach(scc_ch[i]);
    scc_ch[i]->enable(true);
    scc_ch[i]->setVolume(0);
  }

  soundGenerator.setVolume(64);
  soundGenerator.play(true);
  psgGen.enable(true);

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, SPI)) {
    File biosFile = SD.open(SD_ROOT "/MSX.ROM");
    if (biosFile) {
      size_t size = biosFile.size();
      if (size > 0x8000) size = 0x8000;
      biosFile.read(slot_mem[0], size);
      biosFile.close();
      Serial.printf("Main BIOS loaded: %d bytes to Slot 0\n", size);
    } else {
      Serial.println("Error: /MSX.ROM not found!");
    }

#ifdef USE_FDC
    initDiskPSRAM();
    bool hasDiskRom = SD.exists(SD_ROOT "/DISK.ROM");
#else
    bool hasDiskRom = false;
#endif
    bool hasFmPac = SD.exists(SD_ROOT "/FMPAC.ROM");
    bootSlot[1] = hasFmPac ? ITEM_FMPAC : "";  // スロット2の既定は FM-PAC(MSX2 版と同じ)
    loadJoyKey();
    Serial1.begin(115200, SERIAL_8N1, 39, -1);   // 起動メニューもポート1のコントローラで操作できる
    bootMenu(hasDiskRom, hasFmPac);           // スロット1/2・Disk BIOS・ディスク1/2・マウスを選ぶ(ディスクはここで読み込む)

    // ---- カートリッジ(スロット1 / 2)----
    for (int s = 1; s <= 2; s++) {
      const String& item = bootSlot[s - 1];
      if (item == ITEM_SCC) {
        cartInsert(s, nullptr, 0, MAPPER_SCC);
        Serial.printf("Slot %d: SCC cartridge (no ROM)\n", s);
      } else if (item == ITEM_FMPAC) {
        if (!loadFmPac(s)) Serial.println("FM-PAC load failed");
      } else if (item != "") {
        loadCartridge(s, SD_ROOT "/ROM/" + item);
      }
    }

#ifdef USE_FDC
    // ---- DISK.ROM: 空いているスロット(2 → 1)、どちらも使っていればスロット3-1(拡張スロット)----
    File diskRomFile;
    if (hasDiskRom && bootDiskBios) diskRomFile = SD.open(SD_ROOT "/DISK.ROM");
    if (diskRomFile) {
      size_t size = diskRomFile.size();
      if (size > 0x4000) size = 0x4000;  // 通常16KB
      diskRomFile.read(&slot_mem[2][0x4000], size);
      diskRomFile.close();
      diskPrim = bootSlot[1] == "" ? 2 : bootSlot[0] == "" ? 1 : 3;
      slot3Expanded = (diskPrim == 3);
      Serial.printf("DiskROM Magic: %02X %02X in slot %s\n", slot_mem[2][0x4000], slot_mem[2][0x4001],
                    diskPrim == 3 ? "3-1 (slot 3 expanded)" : diskPrim == 2 ? "2" : "1");
#ifdef DISK_IF_MEMORY_MAPPED
      // ソニー/フィリップス型はFDCレジスタ 7FF8h〜7FFFh を絶対番地で参照する。
      // 1つも無ければ別方式(I/Oポート型や松下型)のDISK.ROMなので警告だけ出す。
      {
        const uint8_t* r = &slot_mem[2][0x4000];
        int refs = 0;
        for (size_t i = 0; i + 1 < size; i++) {
          if (r[i + 1] == 0x7F && r[i] >= 0xF8) refs++;
        }
        Serial.printf("DiskROM: 7FF8h-7FFFh refs=%d%s\n", refs,
                      refs ? "" : "  WARNING: not a Sony/Philips-type DISK.ROM"
                                  " (for I/O port type, comment out DISK_IF_MEMORY_MAPPED)");
      }
#endif
    } else {
      Serial.println(hasDiskRom ? "Disk BIOS: OFF (selected in boot menu)" : "Warning: /disk.rom not found. FDD will be disabled.");
    }

    wd2793Reset();
#endif
    mousePort = bootMouse;
    mouseOn = bootMouse != 0;
  } else {
    Serial.println("SD open failed.");
  }
  memset(dummy_page, 0xFF, 8192);

  for (int i = 0; i < 12; i++) {
    msx_key_matrix[i] = 0xFF;
  }

  slot_reg = 0x00;
  ssr3 = 0;
  updateMemoryMap();

  memset(&cpu, 0, sizeof(Z80));
  ResetZ80(&cpu);
  cpu.IPeriod = 512;
  cpu.SPtr.W = 0xF380;
  cpu.AF.B.h = 0x00;

  // Clear status bar area and switch to game display
  vpClearRows(0, 199);   // ROM選択画面の残りを消してからゲーム表示へ
  displayMode = DisplayMode::GAME;

  xTaskCreatePinnedToCore(cpuTask, "cpuTask", 8192, NULL, 24, NULL, 0);

  Serial.println("MSX Booting with Dual Core...");
  Serial.end();
  Serial1.begin(115200, SERIAL_8N1, 39, -1);
}

void updateAllSound() {
  if (isPaused) {
    psgGen.setVolume(0);
    for (int i = 0; i < 5; i++) {
      scc_ch[i]->setVolume(0);
    }
  } else {
    updateSCCSound();
    psgGen.fillBuffer();
    for (int i = 0; i < 5; i++) {
      scc_ch[i]->fillBuffer();
    }
    if (fmGen) fmGen->fillBuffer();
  }
}

void loop() {
  if (frameReady) {
    frameReady = false;
    if (!isPaused) {
      tms9918a_rasterize(vdp);
      flip_buffer(vdp);
      // UI表示中(ファイル選択など)は画面を上書きしない
      if (displayMode == DisplayMode::GAME) presentGameFrame();
    }
    updateAllSound();
    updateController();
    updateKeyboard();
    updateMouse();
    saveFmPacSramIfChanged();
  }
  updateAllSound();

  if (isPaused) {
    vTaskDelay(pdMS_TO_TICKS(5));
    updateKeyboard();
  } else {
    delay(1);
  }
}