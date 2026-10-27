// ============================================================================
//  WD2793 FDC + ソニー(フィリップス)型ディスクインターフェース (7FF8h-7FFFh)
//  (MSX2版 MSX2\wd2793.h と同じ動作。入口の名前だけ MSX1 用)
//  ディスクイメージ(.DSK)はフロントエンドがメモリに読み込んで渡す。
// ============================================================================
#pragma once
#include <stdint.h>

uint32_t fdcLineClock();

void wd2793SetDisk(int drive, uint8_t *buf, uint32_t size);
bool wd2793HasDisk(int drive);
bool wd2793Busy();
void wd2793Reset();

uint8_t wd2793Read(int reg);
void    wd2793Write(int reg, uint8_t v);

void    wd2793SelectDrive(int drive);
