// ============================================================================
//  WD2793 FDC + ソニー(フィリップス)型ディスクインターフェース (7FF8h-7FFFh)
//  ディスクイメージ(.DSK)はフロントエンドがメモリに読み込んで渡す。
//  msx2.h の msx2FdcRead / msx2FdcWrite はここで実装する。
// ============================================================================
#pragma once
#include "msx2_config.h"

// buf にイメージ全体(size バイト)が入っている。NULL で取り出し。
void wd2793SetDisk(int drive, uint8_t *buf, uint32_t size);
bool wd2793HasDisk(int drive);
bool wd2793Busy();                     // アクセスランプ用
void wd2793Reset();
// コマンドを書いたあとに呼ぶ(PC のテスト用。ESP32 では nullptr のまま)
extern void (*wd2793CmdHook)(uint8_t cmd, uint8_t track, uint8_t sector, uint8_t side, int drive, uint8_t status);
