// ============================================================================
//  MSX2+ for ESP32 - ビルド設定と機種依存マクロ
//  ESP32(Arduino)とPC(host/ のテスト用)の両方からインクルードする。
// ============================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

// ---- メモリマッパーのRAM容量(16KBセグメント数) ----
// HB-F1XDJ実機は64KB(4)。MSX-DOS2やアプリをPCとして使うなら多いほうがよい。
// セグメント0〜3(64KB)は内部RAMに置き、残りはPSRAMに置く。
#define MSX2_MAPPER_SEGMENTS 16      // 16 = 256KB

// ---- 画面 ----
#define MSX2_SCREEN_W 512            // 出力ビューポートの幅(256ドットのモードは横2倍)
#define MSX2_SCREEN_H 212            // 出力ビューポートの高さ(192ラインのモードは上下に10ラインずつ枠)

// ---- タイミング(NTSC) ----
#define MSX2_LINES_PER_FRAME 262
#define MSX2_CYCLES_PER_LINE 228     // 3.579545MHz / 15.734kHz

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

// ---- VDPコマンドの実行速度 (実機に対する %) ----
// 100: 実機と同じ速さ。ブロック転送(HMMM/HMMV/LMMM/LMMV/YMMM)はラインごとに少しずつ進め、
//      終わるまで S#2 の CE を 1 にする。LINE/SRCH は結果をすぐ書き、CE だけ実機の時間ぶん 1 にする。
//      大きな転送(512x212 の HMMM など)を1フレームでまとめて処理しなくなる。
// 0  : R#46 を書いた瞬間に全部終える(2026-10-06 までの動作)。
#ifndef MSX2_VDP_CMD_SPEED
  #define MSX2_VDP_CMD_SPEED 100
#endif

// ---- 処理時間の内訳 (開発中の確認用) ----
// 1: 1フレームの中で VDP描画 / Z80 / VDPコマンド に使ったCPUサイクルと、VRAMアクセス・
//    コマンド・バンク切り替えなどの回数を数える。MSX2.ino は MSX2_PERF_LOG の FRAME 行の
//    あとに300フレームぶんの集計を出す (msx2ProfReport)。
// 0: 数えるコードは全部消える(元と同じコード)。
// (2026-10-06 実機で全ソフト 16.7ms 以内を確認したので 0 にした。速度を調べ直すときは 1 に戻す)
#ifndef MSX2_PROFILE
  #define MSX2_PROFILE 0
#endif

#if MSX2_PROFILE
  // CPUのサイクルカウンタ。32bit で折り返すが、差しか使わない(1区間は1フレームより短い)。
  #if defined(__XTENSA__)
    // ESP32: CCOUNT (240MHz、コアごと。cpuTask はコア0固定)。esp_cpu_get_ccount() と同じ命令
    static inline __attribute__((always_inline)) uint32_t msx2ProfClock() {
      uint32_t c;
      __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
      return c;
    }
  #elif defined(_MSC_VER)
    #include <intrin.h>
    static inline uint32_t msx2ProfClock() { return (uint32_t)__rdtsc(); }   // PC: TSC
  #elif defined(__x86_64__) || defined(__i386__)
    #include <x86intrin.h>
    static inline uint32_t msx2ProfClock() { return (uint32_t)__rdtsc(); }
  #else
    static inline uint32_t msx2ProfClock() { return 0; }                     // 時間は0(回数だけ)
  #endif
  // 1フレームに1回だけ通る集計処理。IRAM の関数に展開させない(フラッシュに置く)
  #if defined(__GNUC__)
    #define MSX2_PROF_COLD __attribute__((noinline, cold))
  #else
    #define MSX2_PROF_COLD
  #endif
#endif
