// ============================================================================
//  WD2793 FDC + ソニー(フィリップス)型ディスクインターフェース (7FF8h-7FFFh)
//  (MSX2版 MSX2\wd2793.h と同じ動作。入口の名前だけ MSX1 用)
//  ディスクイメージ(.DSK)はフロントエンドがメモリに読み込んで渡す。
// ============================================================================
#pragma once
#include <stdint.h>

// 起動からのライン数(15.734kHz)。インデックスパルスと入れ替え中の時間に使う。MSX.ino が実装する。
uint32_t fdcLineClock();

// buf にイメージ全体(size バイト)が入っている。NULL で取り出し。
void wd2793SetDisk(int drive, uint8_t *buf, uint32_t size);
bool wd2793HasDisk(int drive);
bool wd2793Busy();                     // アクセスランプ用
void wd2793Reset();

// ソニー型のレジスタ。reg = 0..7 (7FF8h〜7FFFh)。0〜3 は WD2793 本体、4 サイド、5 ドライブ、7 INTRQ/DRQ。
uint8_t wd2793Read(int reg);
void    wd2793Write(int reg, uint8_t v);

// I/O ポート型(Microsol 等)用: ドライブを直接選ぶ(0/1、それ以外は「なし」)。サイドは wd2793Write(4, side)。
void    wd2793SelectDrive(int drive);
