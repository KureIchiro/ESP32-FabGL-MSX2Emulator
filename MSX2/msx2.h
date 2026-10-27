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

enum Msx2Mapper { MSX2_MAP_NONE, MSX2_MAP_KONAMI, MSX2_MAP_ASCII8, MSX2_MAP_ASCII16, MSX2_MAP_RTYPE, MSX2_MAP_KONAMI_SCC, MSX2_MAP_SCC };

void msx2SetBios(uint8_t *bios32k);
void msx2SetSubRom(uint8_t *sub16k);
void msx2SetKanjiBasic(uint8_t *rom, uint32_t size);
void msx2SetMusicRom(uint8_t *fm16k, int prim = 0, int sub = 2);
void msx2SetFmPac(uint8_t *rom, uint32_t size, uint8_t *sram8k);
void msx2SetSlotExpanded(int prim, bool expanded);
void msx2SetF4Inverted(bool inverted);
void msx2SetDiskRom(uint8_t *disk16k);
void msx2SetKanjiFont(uint8_t *font, uint32_t size);
void msx2SetCartridge(int slot, uint8_t *rom, uint32_t size, Msx2Mapper mapper);
void msx2SetRamSegment(int seg, uint8_t *mem16k);
void msx2SetRom(int prim, int sub, uint16_t addr, uint8_t *rom, uint32_t size);

void msx2Reset();
void msx2RunFrame();

void msx2SetCpuCycles(int cyclesPerLine);
int  msx2GetCpuCycles();

uint32_t msx2FmPacSramWrites();

extern volatile uint8_t msx2_key_matrix[16];
extern volatile uint8_t msx2_joy[2];
void msx2SetMousePort(int port);
extern volatile int32_t msx2_mouse_x, msx2_mouse_y;
extern volatile uint8_t msx2_mouse_buttons;

void    msx2PsgWrite(int reg, uint8_t v);
void    msx2FmWrite(int port, uint8_t v);
void    msx2SccWrite(uint16_t addr, uint8_t v);
uint32_t msx2LineClock();

uint8_t msx2FdcRead(int reg);
void    msx2FdcWrite(int reg, uint8_t v);

uint8_t msx2Leds();
uint8_t msx2PrimarySlots();
uint8_t msx2SubSlots(int prim);

#if MSX2_PROFILE
int msx2ProfReport(char *buf, int size, uint32_t clockPerUs);
#endif
