#pragma once
// ============================================================================
//  ジョイスティック(アタリ仕様 → シリアル変換器、IO39)の補助。MSX1 版・MSX2 版で同じファイル。
//
//  1) マウスをつないだポートのコントローラ → キーボード(PC-88 版の JoyToKey と同じ考え方)
//     起動メニューの Mouse で PORT 1 / 2 を選んだとき、そのポートのコントローラ(1P / 2P)は
//     ゲームにはジョイスティックとして渡さず(ポートはマウスが使う)、キーを押す:
//       方向 = カーソルキー ↑↓←→、ボタン1〜3 = 起動メニューの Joy B1〜B3 で決めたキー。
//     Mouse = NONE のときは何もしない(有効・無効はマウスの設定だけで決まる)。
//     押している間は毎フレーム押下にし(キーボードで同じキーを離しても押したまま)、
//     コントローラ側を離したときだけ離す。
//
//  2) 起動メニュー・ファイル選択をポート1(1P)のコントローラで操作する
//       ↑↓ = カーソル(押し続けると連続)、←→ = 1画面送り、ボタン1 = 決定(Enter)、ボタン2 = 取り消し(ESC)
//
//  変換器からの1バイト: bit7 = 2P、bit0〜3 = ↑↓←→、bit4〜6 = ボタン1〜3(0 = 押下)
// ============================================================================
#include <Arduino.h>

// Joy B1〜B3 で押すキー(0xRB: R = キーマトリクスの行、B = ビット。0xFF = なし)。
// 既定は PC-88 版と同じ SPACE / RETURN / ESC
static uint8_t joyKey[3] = { 0x80, 0x77, 0x72 };

// state: 1P / 2P のどちらか1バイト(下位7ビット、0 = 押下)。enabled = そのポートにマウスをつないでいる
static void joyKeysApply(uint8_t state, bool enabled, volatile uint8_t *matrix) {
  static const uint8_t dirKey[4] = { 0x85, 0x86, 0x84, 0x87 };   // ↑ ↓ ← →(行8)
  static uint8_t was = 0;                                        // 前のフレームで押していた入力(bit = 入力)
  uint8_t now = enabled ? (uint8_t)(~state & 0x7F) : 0;
  for (int i = 0; i < 7; i++) {
    uint8_t rb = i < 4 ? dirKey[i] : joyKey[i - 4];
    if (rb == 0xFF) continue;
    uint8_t r = rb >> 4, m = (uint8_t)(1 << (rb & 7));
    if (now & (1 << i)) matrix[r] &= (uint8_t)~m;
    //else if (was & (1 << i)) matrix[r] |= m;
  }
  was = now;
}

// 起動メニュー・ファイル選択用: ポート1のコントローラを読んで操作を返す
//   'U' 'D' 'L' 'R' = 方向(押した瞬間と、0.4 秒押し続けたあと 0.12 秒ごと)、'A' = ボタン1、'B' = ボタン2、0 = なし
// 変換器は約 60Hz で送ってくる。0.1 秒届かなければ離したことにする
static int joyMenuPoll() {
  static uint8_t state = 0x7F, last = 0x7F;
  static uint32_t lastRx = 0, repeatAt = 0;
  uint32_t now = millis();
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    if (!(b & 0x80)) { state = b & 0x7F; lastRx = now; }
  }
  if (now - lastRx > 100) state = 0x7F;
  uint8_t pressed = (uint8_t)(~state & 0x7F), prev = (uint8_t)(~last & 0x7F);
  uint8_t edge = pressed & ~prev;
  last = state;
  if (edge & 0x10) return 'A';
  if (edge & 0x20) return 'B';
  static const char dirs[4] = { 'U', 'D', 'L', 'R' };
  for (int i = 0; i < 4; i++) {
    if (!(pressed & (1 << i))) continue;
    if (edge & (1 << i)) { repeatAt = now + 400; return dirs[i]; }
    if ((int32_t)(now - repeatAt) >= 0) { repeatAt = now + 120; return dirs[i]; }
  }
  return 0;
}

// 起動メニュー・ファイル選択で、コントローラの操作を PS/2 のスキャンコードに直す(e0 = E0 付きのキー)
static int joyMenuScancode(int a, bool *e0) {
  *e0 = true;
  switch (a) {
    case 'U': return 0x75;                 // ↑
    case 'D': return 0x72;                 // ↓
    case 'L': return 0x7D;                 // PageUp
    case 'R': return 0x7A;                 // PageDown
    case 'A': *e0 = false; return 0x5A;    // Enter
    case 'B': *e0 = false; return 0x76;    // ESC
    default: return 0;
  }
}
