// ============================================================================
//  MSX2+ for ESP32 - ビルド設定と機種依存マクロ
//  ESP32(Arduino)とPC(host/ のテスト用)の両方からインクルードする。
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MSX2_MAPPER_SEGMENTS 16

#define MSX2_SCREEN_W 512
#define MSX2_SCREEN_H 212

#define MSX2_LINES_PER_FRAME 262
#define MSX2_CYCLES_PER_LINE 228

#ifdef ARDUINO
  #include <esp_attr.h>
  #define MSX2_FAST IRAM_ATTR
  #define MSX2_DRAM DRAM_ATTR
#else
  #define MSX2_FAST
  #define MSX2_DRAM
#endif

#if defined(__GNUC__)
  #define MSX2_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define MSX2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define MSX2_LIKELY(x)   (x)
  #define MSX2_UNLIKELY(x) (x)
#endif

#ifndef MSX2_VDP_CMD_SPEED
  #define MSX2_VDP_CMD_SPEED 100
#endif

#ifndef MSX2_PROFILE
  #define MSX2_PROFILE 0
#endif

#if MSX2_PROFILE
  #if defined(__XTENSA__)
    static inline __attribute__((always_inline)) uint32_t msx2ProfClock() {
      uint32_t c;
      __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
      return c;
    }
  #elif defined(_MSC_VER)
    #include <intrin.h>
    static inline uint32_t msx2ProfClock() { return (uint32_t)__rdtsc(); }
  #elif defined(__x86_64__) || defined(__i386__)
    #include <x86intrin.h>
    static inline uint32_t msx2ProfClock() { return (uint32_t)__rdtsc(); }
  #else
    static inline uint32_t msx2ProfClock() { return 0; }
  #endif
  #if defined(__GNUC__)
    #define MSX2_PROF_COLD __attribute__((noinline, cold))
  #else
    #define MSX2_PROF_COLD
  #endif
#endif
