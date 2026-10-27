#pragma once
#include <Arduino.h>

static uint8_t joyKey[3] = { 0x80, 0x77, 0x72 };

static void joyKeysApply(uint8_t state, bool enabled, volatile uint8_t *matrix) {
  static const uint8_t dirKey[4] = { 0x85, 0x86, 0x84, 0x87 };
  static uint8_t was = 0;
  uint8_t now = enabled ? (uint8_t)(~state & 0x7F) : 0;
  for (int i = 0; i < 7; i++) {
    uint8_t rb = i < 4 ? dirKey[i] : joyKey[i - 4];
    if (rb == 0xFF) continue;
    uint8_t r = rb >> 4, m = (uint8_t)(1 << (rb & 7));
    if (now & (1 << i)) matrix[r] &= (uint8_t)~m;
    else if (was & (1 << i)) matrix[r] |= m;
  }
  was = now;
}

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

static int joyMenuScancode(int a, bool *e0) {
  *e0 = true;
  switch (a) {
    case 'U': return 0x75;
    case 'D': return 0x72;
    case 'L': return 0x7D;
    case 'R': return 0x7A;
    case 'A': *e0 = false; return 0x5A;
    case 'B': *e0 = false; return 0x76;
    default: return 0;
  }
}
