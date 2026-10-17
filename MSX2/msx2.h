// ============================================================================
//  MSX2+ マシン本体 (スロット / メモリマッパー / I/O / RTC / 漢字ROM / 1フレーム実行)
//
//  スロット構成 (HB-F1XDJ に準じる。BIOSが拡張スロットを探索するので、
//  サブROMやディスクROMの位置は実機と同じでなくても動く)
//    0-0 : MSX2+ BIOS + BASIC (0000h-7FFFh)
//    0-2 : MSX-MUSIC (4000h-7FFFh)            ※ROMがあれば
//    1   : カートリッジ A
//    2   : カートリッジ B
//    3-0 : RAM (メモリマッパー)
//    3-1 : サブROM (0000h-3FFFh) + 漢字BASIC (4000h-BFFFh)
//    3-2 : ディスクROM (4000h-7FFFh, ソニー型FDC 7FF8h-7FFFh)
//
//  ESP32固有の処理(音・FDDのイメージ・キーボード)は msx2_hooks の関数で
//  フロントエンド(.ino / host)につなぐ。
// ============================================================================
#pragma once
#include "msx2_config.h"

// MSX2_MAP_SCC: ROM の無い SCC だけのカートリッジ(SD スナッチャーのサウンドカートリッジの代わり。RAM は無し)
enum Msx2Mapper { MSX2_MAP_NONE, MSX2_MAP_KONAMI, MSX2_MAP_ASCII8, MSX2_MAP_ASCII16, MSX2_MAP_RTYPE, MSX2_MAP_KONAMI_SCC, MSX2_MAP_SCC };

// ---- ROM / RAM の登録 (msx2Reset の前に呼ぶ。データは呼び出し側が保持する) ----
void msx2SetBios(uint8_t *bios32k);
void msx2SetSubRom(uint8_t *sub16k);
void msx2SetKanjiBasic(uint8_t *rom, uint32_t size);      // 32KB (4000h-BFFFh)
void msx2SetMusicRom(uint8_t *fm16k, int prim = 0, int sub = 2);
// FM-PAC をスロット2に挿す(ROM 16KB×1〜4バンク、sram は 8KB)。内蔵MSX-MUSICとは併用しない
void msx2SetFmPac(uint8_t *rom, uint32_t size, uint8_t *sram8k);
// 基本スロットを拡張するか(既定: 0 と 3 を拡張)。C-BIOS はスロット0が拡張されていない前提
void msx2SetSlotExpanded(int prim, bool expanded);
// F4h(ハード/ソフトリセットの区別)がビット反転型か。C-BIOS は反転型の前提
void msx2SetF4Inverted(bool inverted);
void msx2SetDiskRom(uint8_t *disk16k);
void msx2SetKanjiFont(uint8_t *font, uint32_t size);      // 128KB(第1水準) or 256KB(第2水準まで)
void msx2SetCartridge(int slot, uint8_t *rom, uint32_t size, Msx2Mapper mapper);  // slot = 1 or 2(MSX2_MAP_SCC は rom = nullptr)
void msx2SetRamSegment(int seg, uint8_t *mem16k);          // seg = 0..MSX2_MAPPER_SEGMENTS-1
// 任意の位置にROMを置く(C-BIOSのロゴROMなど)。addr は8KB単位
void msx2SetRom(int prim, int sub, uint16_t addr, uint8_t *rom, uint32_t size);

void msx2Reset();
void msx2RunFrame();                                       // 262ライン分(Z80+VDP)

// Z80 の速さ: 1ラインに実行するクロック数(既定 205 = 実機相当。270 / 335 で速くする)
void msx2SetCpuCycles(int cyclesPerLine);
int  msx2GetCpuCycles();

// FM-PAC の SRAM に書き込んだ回数(フロントエンドが変わったのを見て SD に保存する)
uint32_t msx2FmPacSramWrites();

// ---- 入力 (フロントエンドが書く) ----
extern volatile uint8_t msx2_key_matrix[16];              // 押下=0
extern volatile uint8_t msx2_joy[2];                      // PSG R#14 に返す値(押下=0)
// マウス: port = 0 なし / 1 / 2(そのポートのジョイスティックの代わり)。
// 動きはフロントエンドが積算した値(右・上が +)を書き、ボタンは bit0 = 左、bit1 = 右(1 = 押下)
void msx2SetMousePort(int port);
extern volatile int32_t msx2_mouse_x, msx2_mouse_y;
extern volatile uint8_t msx2_mouse_buttons;

// ---- フロントエンドが実装するフック ----
void    msx2PsgWrite(int reg, uint8_t v);
void    msx2FmWrite(int port, uint8_t v);                  // port 0=アドレス(7Ch) 1=データ(7Dh)
void    msx2SccWrite(uint16_t addr, uint8_t v);            // Konami SCC 9800h-98FFh
uint32_t msx2LineClock();                                 // 起動からのライン数(15.734kHz。FDC の時間に使う)

// ---- ディスク(wd2793.cpp が実装) ----
uint8_t msx2FdcRead(int reg);                              // reg = 0..7 (7FF8h〜)
void    msx2FdcWrite(int reg, uint8_t v);

// ---- デバッグ用 ----
uint8_t msx2PrimarySlots();
uint8_t msx2SubSlots(int prim);

#if MSX2_PROFILE
// ---- 処理時間の内訳 (msx2_config.h の MSX2_PROFILE) ----
// msx2Reset または前回の呼び出しから後に実行したフレームの集計を2行(各行 '\n' 終わり)の
// 文字列にして buf に入れ、集計を0に戻す。戻り値は文字数。clockPerUs は msx2ProfClock() の
// 1us あたりのカウント(ESP32 は CPU の MHz = 240)。
//   1行目 "PROF us" 1フレームの時間(us)の avg/max: run=msx2RunFrame全体, vdp=vdpLine (spr=うち
//         スプライト, out=うち out256/out512), z80=ExecZ80+割込み判定 (vio=うちVDPポート 98h〜9Bh の
//         処理, cmd=vio のうちVDPコマンド, map=うち updateMemoryMap)。worst = run が最大だった
//         フレームの内訳。longest cmd = R#46 書き込み1回で最も長かったコマンド (CMD/LOP NX×NY 時間)
//   2行目 "PROF n/frame" 1フレームあたりの回数: 98h の書き込み/読み出し, 受け付けた割込み,
//         HALT で始まったライン数, カートリッジ/FM-PAC のバンク切り替え, メモリマップの作り直し,
//         表示オンで描いたライン数(ln), スプライトを重ねたライン数(spl), 表示スプライトの延べ数(spv),
//         VDPコマンドの種類ごとの
//         数 (カッコ内は us/frame), 名目ドット数, HMMC/LMMC/LMCM のデータ転送数
int msx2ProfReport(char *buf, int size, uint32_t clockPerUs);
#endif
