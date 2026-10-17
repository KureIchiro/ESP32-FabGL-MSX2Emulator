/**
 * emu2413 v1.5.9 (Highly Optimized for Low Memory / Embedded Systems)
 * https://github.com/digital-sound-antiques/emu2413
 * Copyright (C) 2020 Mitsutaka Okazaki
 */
// 【最適化】Arduinoのesp32コアは既定で -Os。このファイルは lookup_wave /
// to_linear / lookup_exp_table / min / max / calc_slot_mod / calc_slot_car
// といった10〜50命令の小関数を、1出力サンプルあたり約102回呼ぶ構造で、
// Xtensaの CALL8/RETW ウィンドウ回転が関数本体と同等のコストになっている。
// 宣言の INLINE は __inline__ 止まりの「ヒント」で、-Os はこれを無視する。
// ※このファイルには IRAM_ATTR が1つも無いので、O3でコードが膨らんでも
//   IRAM溢れのリスクは無い(増えるのはフラッシュ側。約6KB→約11KB)。
// ※演算は全て整数。浮動小数点は OPLL_RateConv と OPLL_setPanFine だけで、
//   どちらもこのプロジェクトでは未使用のため音質への影響は無い。
#pragma GCC optimize("O3")

#include <pgmspace.h>
#include "esp_attr.h"
#include "emu2413.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef INLINE
#if defined(_MSC_VER)
#define INLINE __inline
#elif defined(__GNUC__)
#define INLINE __inline__
#else
#define INLINE inline
#endif
#endif

#define OPLL_TONE_NUM 1
/* clang-format off */
static const uint8_t default_inst[OPLL_TONE_NUM][(16 + 3) * 8] = {{
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, // 0: User
0x71,0x61,0x18,0x17,0xd0,0x78,0x00,0x17, // 1: Violin
0x13,0x41,0x1a,0x0d,0xd8,0xf7,0x23,0x13, // 2: Guitar
0x13,0x01,0x99,0x00,0xf2,0xc4,0x21,0x23, // 3: Piano

0x21,0x61,0x1b,0x07,0xaf,0x64,0x40,0x27,// 4: Flute
0x32,0x21,0x1e,0x06,0xe1,0x76,0x01,0x28, // 5: Clarinet
0x31,0x22,0x16,0x05,0xe0,0x71,0x00,0x18, // 6: Oboe
0x21,0x61,0x1d,0x07,0x82,0x81,0x11,0x07, // 7: Trumpet

0x33,0x21,0x2d,0x13,0xb0,0x70,0x00,0x07, // 8: Organ
0x61,0x61,0x1b,0x06,0x64,0x65,0x10,0x17, // 9: Horn
0x41,0x61,0x0b,0x18,0x85,0xf0,0x81,0x07, // A: Synthesizer
0x33,0x01,0xC3,0x51,0xec,0xef,0x10,0x06, // B: Harpsichord

0x16,0xc1,0x24,0x07,0xe8,0xe8,0x22,0x12, // C: Vibraphone
0x61,0x50,0x0c,0x05,0xd2,0xf5,0x40,0x42, // D: Synthsizer Bass
0x01,0x01,0x55,0x03,0xe9,0x90,0x03,0x02, // E: Acoustic Bass
0x21,0x21,0x87,0x02,0xf1,0xe4,0xe2,0xf5, // F: Electric Guitar

0x01,0x01,0x18,0x08,0xef,0xfa,0x6c,0x6e, // R: Bass Drum
0x01,0x01,0x80,0x00,0xda,0xea,0xa9,0x68, // R: High-Hat / Snare Drum
0x05,0x01,0x80,0x00,0xea,0xe9,0x5b,0x56, // R: Tom-tom / Top Cymbal
}};
/* clang-format on */

/* phase increment counter */
#define DP_BITS 19
#define DP_WIDTH (1 << DP_BITS)
#define DP_BASE_BITS (DP_BITS - PG_BITS)

/* dynamic range of envelope output */
#define EG_STEP 0.375
#define EG_BITS 7
#define EG_MUTE ((1 << EG_BITS) - 1)
#define EG_MAX (EG_MUTE - 4)

/* dynamic range of total level */
#define TL_STEP 0.75
#define TL_BITS 6

/* damper speed before key-on. key-scale affects. */
#define DAMPER_RATE 12

#define TL2EG(d) ((d) << 1)

/* sine table */
#define PG_BITS 10 /* 2^10 = 1024 length sine table */
#define PG_WIDTH (1 << PG_BITS)

/* exp_table[x] = round((exp2((double)x / 256.0) - 1) * 1024) */
static const uint16_t exp_table[256] DRAM_ATTR = {
  0, 3, 6, 8, 11, 14, 17, 20, 22, 25, 28, 31, 34, 37, 40, 42,
  45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84, 87, 90,
  93, 96, 99, 102, 105, 108, 111, 114, 117, 120, 123, 126, 130, 133, 136, 139,
  142, 145, 148, 152, 155, 158, 161, 164, 168, 171, 174, 177, 181, 184, 187, 190,
  194, 197, 200, 204, 207, 210, 214, 217, 220, 224, 227, 231, 234, 237, 241, 244,
  248, 251, 255, 258, 262, 265, 268, 272, 276, 279, 283, 286, 290, 293, 297, 300,
  304, 308, 311, 315, 318, 322, 326, 329, 333, 337, 340, 344, 348, 352, 355, 359,
  363, 367, 370, 374, 378, 382, 385, 389, 393, 397, 401, 405, 409, 412, 416, 420,
  424, 428, 432, 436, 440, 444, 448, 452, 456, 460, 464, 468, 472, 476, 480, 484,
  488, 492, 496, 501, 505, 509, 513, 517, 521, 526, 530, 534, 538, 542, 547, 551,
  555, 560, 564, 568, 572, 577, 581, 585, 590, 594, 599, 603, 607, 612, 616, 621,
  625, 630, 634, 639, 643, 648, 652, 657, 661, 666, 670, 675, 680, 684, 689, 693,
  698, 703, 708, 712, 717, 722, 726, 731, 736, 741, 745, 750, 755, 760, 765, 770,
  774, 779, 784, 789, 794, 799, 804, 809, 814, 819, 824, 829, 834, 839, 844, 849,
  854, 859, 864, 869, 874, 880, 885, 890, 895, 900, 906, 911, 916, 921, 927, 932,
  937, 942, 948, 953, 959, 964, 969, 975, 980, 986, 991, 996, 1002, 1007, 1013, 1018
};

/* ROM Only: Only 1/4 of the sine wave is stored to save memory (512 Bytes) */
static const uint16_t logsin_table[256] DRAM_ATTR = {
  2137,
  1731,
  1543,
  1419,
  1326,
  1252,
  1190,
  1137,
  1091,
  1050,
  1013,
  979,
  949,
  920,
  894,
  869,
  846,
  825,
  804,
  785,
  767,
  749,
  732,
  717,
  701,
  687,
  672,
  659,
  646,
  633,
  621,
  609,
  598,
  587,
  576,
  566,
  556,
  546,
  536,
  527,
  518,
  509,
  501,
  492,
  484,
  476,
  468,
  461,
  453,
  446,
  439,
  432,
  425,
  418,
  411,
  405,
  399,
  392,
  386,
  380,
  375,
  369,
  363,
  358,
  352,
  347,
  341,
  336,
  331,
  326,
  321,
  316,
  311,
  307,
  302,
  297,
  293,
  289,
  284,
  280,
  276,
  271,
  267,
  263,
  259,
  255,
  251,
  248,
  244,
  240,
  236,
  233,
  229,
  226,
  222,
  219,
  215,
  212,
  209,
  205,
  202,
  199,
  196,
  193,
  190,
  187,
  184,
  181,
  178,
  175,
  172,
  169,
  167,
  164,
  161,
  159,
  156,
  153,
  151,
  148,
  146,
  143,
  141,
  138,
  136,
  134,
  131,
  129,
  127,
  125,
  122,
  120,
  118,
  116,
  114,
  112,
  110,
  108,
  106,
  104,
  102,
  100,
  98,
  96,
  94,
  92,
  91,
  89,
  87,
  85,
  83,
  82,
  80,
  78,
  77,
  75,
  74,
  72,
  70,
  69,
  67,
  66,
  64,
  63,
  62,
  60,
  59,
  57,
  56,
  55,
  53,
  52,
  51,
  49,
  48,
  47,
  46,
  45,
  43,
  42,
  41,
  40,
  39,
  38,
  37,
  36,
  35,
  34,
  33,
  32,
  31,
  30,
  29,
  28,
  27,
  26,
  25,
  24,
  23,
  23,
  22,
  21,
  20,
  20,
  19,
  18,
  17,
  17,
  16,
  15,
  15,
  14,
  13,
  13,
  12,
  12,
  11,
  10,
  10,
  9,
  9,
  8,
  8,
  7,
  7,
  7,
  6,
  6,
  5,
  5,
  5,
  4,
  4,
  4,
  3,
  3,
  3,
  2,
  2,
  2,
  2,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
};

/* pitch modulator */
static const int8_t pm_table[8][8] DRAM_ATTR = {
  { 0, 0, 0, 0, 0, 0, 0, 0 },     // fnum = 000xxxxxx
  { 0, 0, 1, 0, 0, 0, -1, 0 },    // fnum = 001xxxxxx
  { 0, 1, 2, 1, 0, -1, -2, -1 },  // fnum = 010xxxxxx
  { 0, 1, 3, 1, 0, -1, -3, -1 },  // fnum = 011xxxxxx
  { 0, 2, 4, 2, 0, -2, -4, -2 },  // fnum = 100xxxxxx
  { 0, 2, 5, 2, 0, -2, -5, -2 },  // fnum = 101xxxxxx
  { 0, 3, 6, 3, 0, -3, -6, -3 },  // fnum = 110xxxxxx
  { 0, 3, 7, 3, 0, -3, -7, -3 },  // fnum = 111xxxxxx
};

/* amplitude lfo table */
static const uint8_t am_table[210] DRAM_ATTR = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
                                                 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
                                                 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5,
                                                 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,
                                                 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9,
                                                 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11,
                                                 12, 12, 12, 12, 12, 12, 12, 12,
                                                 13, 13, 13,
                                                 12, 12, 12, 12, 12, 12, 12, 12,
                                                 11, 11, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10,
                                                 9, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8, 8, 8, 8, 8, 8,
                                                 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6,
                                                 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
                                                 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
                                                 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };

/* envelope decay increment step table */
static const uint8_t eg_step_tables[4][8] DRAM_ATTR = {
  { 0, 1, 0, 1, 0, 1, 0, 1 },
  { 0, 1, 0, 1, 1, 1, 0, 1 },
  { 0, 1, 1, 1, 0, 1, 1, 1 },
  { 0, 1, 1, 1, 1, 1, 1, 1 },
};

enum __OPLL_EG_STATE { ATTACK,
                       DECAY,
                       SUSTAIN,
                       RELEASE,
                       DAMP,
                       UNKNOWN };

static const uint32_t ml_table[16] DRAM_ATTR = { 1, 1 * 2, 2 * 2, 3 * 2, 4 * 2, 5 * 2, 6 * 2, 7 * 2,
                                                 8 * 2, 9 * 2, 10 * 2, 10 * 2, 12 * 2, 12 * 2, 15 * 2, 15 * 2 };

/* kl_table_int: Pre-calculated integer values representing decibel decay steps. 
   Removes the need for floating point math and huge memory allocations. */
static const int32_t kl_table_int[16] DRAM_ATTR = { 0, 18, 24, 27, 30, 32, 33, 35, 36, 37, 38, 39, 39, 40, 41, 42 };

static OPLL_PATCH null_patch = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static OPLL_PATCH (*default_patch)[(16 + 3) * 2] = NULL;

#ifndef min
static INLINE int min(int i, int j) {
  return (i < j) ? i : j;
}
#endif
#ifndef max
static INLINE int max(int i, int j) {
  return (i > j) ? i : j;
}
#endif

/***************************************************

       Internal Sample Rate Converter (Lightweight)

****************************************************/
/* Memory hogging Sinc-interpolation was replaced with lightweight linear interpolation. */

OPLL_RateConv *OPLL_RateConv_new(float f_inp, float f_out, int ch) {
  OPLL_RateConv *conv = calloc(1, sizeof(OPLL_RateConv));
  conv->ch = ch;
  conv->f_ratio = f_inp / f_out;
  /* Instead of huge sinc tap arrays, we just keep 2 samples per channel for linear interp */
  conv->buf = malloc(sizeof(void *) * ch);
  for (int i = 0; i < ch; i++) {
    conv->buf[i] = calloc(2, sizeof(int16_t));
  }
  return conv;
}

void OPLL_RateConv_reset(OPLL_RateConv *conv) {
  conv->timer = 0;
  for (int i = 0; i < conv->ch; i++) {
    memset(conv->buf[i], 0, sizeof(int16_t) * 2);
  }
}

void OPLL_RateConv_putData(OPLL_RateConv *conv, int ch, int16_t data) {
  int16_t *buf = (int16_t *)conv->buf[ch];
  buf[0] = buf[1];
  buf[1] = data;
}

int16_t OPLL_RateConv_getData(OPLL_RateConv *conv, int ch) {
  int16_t *buf = (int16_t *)conv->buf[ch];
  return buf[0] + (int16_t)((buf[1] - buf[0]) * conv->timer);
}

void OPLL_RateConv_delete(OPLL_RateConv *conv) {
  for (int i = 0; i < conv->ch; i++) {
    free(conv->buf[i]);
  }
  free(conv->buf);
  free(conv);
}

/***************************************************

                  Create tables (Simplified)

****************************************************/
/* Replaced huge memory allocation logic with runtime calculators */

static void makeDefaultPatch(void) {
  int i, j;
  if (default_patch != NULL) {
    for (i = 0; i < OPLL_TONE_NUM; i++)
      for (j = 0; j < 19; j++)
        OPLL_getDefaultPatch(i, j, &default_patch[i][j * 2]);
  }
}

static uint8_t table_initialized = 0;

static void initializeTables(void) {
  makeDefaultPatch();
  table_initialized = 1;
}

/*********************************************************

                      Synthesizing

*********************************************************/
#define SLOT_BD1 12
#define SLOT_BD2 13
#define SLOT_HH 14
#define SLOT_SD 15
#define SLOT_TOM 16
#define SLOT_CYM 17

/* utility macros */
#define MOD(o, x) (&(o)->slot[(x) << 1])
#define CAR(o, x) (&(o)->slot[((x) << 1) | 1])
#define BIT(s, b) (((s) >> (b)) & 1)

static INLINE int get_parameter_rate(OPLL_SLOT *slot) {
  if ((slot->type & 1) == 0 && slot->key_flag == 0) {
    return 0;
  }
  switch (slot->eg_state) {
    case ATTACK:
      return slot->patch->AR;
    case DECAY:
      return slot->patch->DR;
    case SUSTAIN:
      return slot->patch->EG ? 0 : slot->patch->RR;
    case RELEASE:
      if (slot->sus_flag) {
        return 5;
      } else if (slot->patch->EG) {
        return slot->patch->RR;
      } else {
        return 7;
      }
    case DAMP:
      return DAMPER_RATE;
    default:
      return 0;
  }
}

enum SLOT_UPDATE_FLAG {
  UPDATE_WS = 1,
  UPDATE_TLL = 2,
  UPDATE_RKS = 4,
  UPDATE_EG = 8,
  UPDATE_ALL = 255,
};

static INLINE void request_update(OPLL_SLOT *slot, int flag) {
  slot->update_requests |= flag;
}

/* Real-time TLL calculation (Replaces 128KB tll_table) */
static INLINE uint32_t calc_tll(uint16_t blk_fnum, int TL, int KL) {
  if (KL == 0) return TL2EG(TL);
  int fnum_idx = (blk_fnum >> 5) & 15;
  int block_idx = blk_fnum >> 9;
  int32_t tmp = kl_table_int[fnum_idx] - 6 * (7 - block_idx);
  if (tmp <= 0) return TL2EG(TL);
  return (uint32_t)((tmp >> (3 - KL)) * 4 / 3) + TL2EG(TL);
}

/* Real-time RKS calculation (Replaces rks_table) */
static INLINE uint8_t calc_rks(uint16_t blk_fnum, uint8_t KR) {
  uint8_t blk_fnum8 = blk_fnum >> 8;
  return KR ? blk_fnum8 : (blk_fnum8 >> 2);
}

static void commit_slot_update(OPLL_SLOT *slot) {
  if (slot->update_requests & UPDATE_TLL) {
    int TL = ((slot->type & 1) == 0) ? slot->patch->TL : slot->volume;
    slot->tll = calc_tll(slot->blk_fnum, TL, slot->patch->KL);
  }

  if (slot->update_requests & UPDATE_RKS) {
    slot->rks = calc_rks(slot->blk_fnum, slot->patch->KR);
  }

  if (slot->update_requests & (UPDATE_RKS | UPDATE_EG)) {
    int p_rate = get_parameter_rate(slot);

    if (p_rate == 0) {
      slot->eg_shift = 0;
      slot->eg_rate_h = 0;
      slot->eg_rate_l = 0;
      return;
    }

    slot->eg_rate_h = min(15, p_rate + (slot->rks >> 2));
    slot->eg_rate_l = slot->rks & 3;
    if (slot->eg_state == ATTACK) {
      slot->eg_shift = (0 < slot->eg_rate_h && slot->eg_rate_h < 12) ? (13 - slot->eg_rate_h) : 0;
    } else {
      slot->eg_shift = (slot->eg_rate_h < 13) ? (13 - slot->eg_rate_h) : 0;
    }
  }

  slot->update_requests = 0;
}

static void reset_slot(OPLL_SLOT *slot, int number) {
  slot->number = number;
  slot->type = number % 2;
  slot->pg_keep = 0;
  slot->pg_phase = 0;
  slot->output[0] = 0;
  slot->output[1] = 0;
  slot->eg_state = RELEASE;
  slot->eg_shift = 0;
  slot->rks = 0;
  slot->tll = 0;
  slot->key_flag = 0;
  slot->sus_flag = 0;
  slot->blk_fnum = 0;
  slot->blk = 0;
  slot->fnum = 0;
  slot->volume = 0;
  slot->pg_out = 0;
  slot->eg_out = EG_MUTE;
  slot->patch = &null_patch;
}

static INLINE void slotOn(OPLL *opll, int i) {
  OPLL_SLOT *slot = &opll->slot[i];
  slot->key_flag = 1;
  slot->eg_state = DAMP;
  request_update(slot, UPDATE_EG);
}

static INLINE void slotOff(OPLL *opll, int i) {
  OPLL_SLOT *slot = &opll->slot[i];
  slot->key_flag = 0;
  if (slot->type & 1) {
    slot->eg_state = RELEASE;
    request_update(slot, UPDATE_EG);
  }
}

static INLINE void update_key_status(OPLL *opll) {
  const uint8_t r14 = opll->reg[0x0e];
  const uint8_t rhythm_mode = BIT(r14, 5);
  uint32_t new_slot_key_status = 0;
  uint32_t updated_status;
  int ch;

  for (ch = 0; ch < 9; ch++)
    if (opll->reg[0x20 + ch] & 0x10)
      new_slot_key_status |= 3 << (ch * 2);

  if (rhythm_mode) {
    if (r14 & 0x10) new_slot_key_status |= 3 << SLOT_BD1;
    if (r14 & 0x01) new_slot_key_status |= 1 << SLOT_HH;
    if (r14 & 0x08) new_slot_key_status |= 1 << SLOT_SD;
    if (r14 & 0x04) new_slot_key_status |= 1 << SLOT_TOM;
    if (r14 & 0x02) new_slot_key_status |= 1 << SLOT_CYM;
  }

  updated_status = opll->slot_key_status ^ new_slot_key_status;

  if (updated_status) {
    int i;
    for (i = 0; i < 18; i++)
      if (BIT(updated_status, i)) {
        if (BIT(new_slot_key_status, i)) {
          slotOn(opll, i);
        } else {
          slotOff(opll, i);
        }
      }
  }

  opll->slot_key_status = new_slot_key_status;
}

static INLINE void set_patch(OPLL *opll, int32_t ch, int32_t num) {
  opll->patch_number[ch] = num;
  MOD(opll, ch)->patch = &opll->patch[num * 2 + 0];
  CAR(opll, ch)->patch = &opll->patch[num * 2 + 1];
  request_update(MOD(opll, ch), UPDATE_ALL);
  request_update(CAR(opll, ch), UPDATE_ALL);
}

static INLINE void set_sus_flag(OPLL *opll, int ch, int flag) {
  CAR(opll, ch)->sus_flag = flag;
  request_update(CAR(opll, ch), UPDATE_EG);
  if (MOD(opll, ch)->type & 1) {
    MOD(opll, ch)->sus_flag = flag;
    request_update(MOD(opll, ch), UPDATE_EG);
  }
}

static INLINE void set_volume(OPLL *opll, int ch, int volume) {
  CAR(opll, ch)->volume = volume;
  request_update(CAR(opll, ch), UPDATE_TLL);
}

static INLINE void set_slot_volume(OPLL_SLOT *slot, int volume) {
  slot->volume = volume;
  request_update(slot, UPDATE_TLL);
}

static INLINE void set_fnumber(OPLL *opll, int ch, int fnum) {
  OPLL_SLOT *car = CAR(opll, ch);
  OPLL_SLOT *mod = MOD(opll, ch);
  car->fnum = fnum;
  car->blk_fnum = (car->blk_fnum & 0xe00) | (fnum & 0x1ff);
  mod->fnum = fnum;
  mod->blk_fnum = (mod->blk_fnum & 0xe00) | (fnum & 0x1ff);
  request_update(car, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
  request_update(mod, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
}

static INLINE void set_block(OPLL *opll, int ch, int blk) {
  OPLL_SLOT *car = CAR(opll, ch);
  OPLL_SLOT *mod = MOD(opll, ch);
  car->blk = blk;
  car->blk_fnum = ((blk & 7) << 9) | (car->blk_fnum & 0x1ff);
  mod->blk = blk;
  mod->blk_fnum = ((blk & 7) << 9) | (mod->blk_fnum & 0x1ff);
  request_update(car, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
  request_update(mod, UPDATE_EG | UPDATE_RKS | UPDATE_TLL);
}

static INLINE void update_rhythm_mode(OPLL *opll) {
  const uint8_t new_rhythm_mode = (opll->reg[0x0e] >> 5) & 1;

  if (opll->rhythm_mode != new_rhythm_mode) {
    if (new_rhythm_mode) {
      opll->slot[SLOT_HH].type = 3;
      opll->slot[SLOT_HH].pg_keep = 1;
      opll->slot[SLOT_SD].type = 3;
      opll->slot[SLOT_TOM].type = 3;
      opll->slot[SLOT_CYM].type = 3;
      opll->slot[SLOT_CYM].pg_keep = 1;
      set_patch(opll, 6, 16);
      set_patch(opll, 7, 17);
      set_patch(opll, 8, 18);
      set_slot_volume(&opll->slot[SLOT_HH], ((opll->reg[0x37] >> 4) & 15) << 2);
      set_slot_volume(&opll->slot[SLOT_TOM], ((opll->reg[0x38] >> 4) & 15) << 2);
    } else {
      opll->slot[SLOT_HH].type = 0;
      opll->slot[SLOT_HH].pg_keep = 0;
      opll->slot[SLOT_SD].type = 1;
      opll->slot[SLOT_TOM].type = 0;
      opll->slot[SLOT_CYM].type = 1;
      opll->slot[SLOT_CYM].pg_keep = 0;
      set_patch(opll, 6, opll->reg[0x36] >> 4);
      set_patch(opll, 7, opll->reg[0x37] >> 4);
      set_patch(opll, 8, opll->reg[0x38] >> 4);
    }
  }
  opll->rhythm_mode = new_rhythm_mode;
}

static void update_ampm(OPLL *opll) {
  if (opll->test_flag & 2) {
    opll->pm_phase = 0;
    opll->am_phase = 0;
  } else {
    opll->pm_phase += (opll->test_flag & 8) ? 1024 : 1;
    opll->am_phase += (opll->test_flag & 8) ? 64 : 1;
  }
  opll->lfo_am = am_table[(opll->am_phase >> 6) % sizeof(am_table)];
}

static void update_noise(OPLL *opll, int cycle) {
  int i;
  for (i = 0; i < cycle; i++) {
    if (opll->noise & 1) {
      opll->noise ^= 0x800200;
    }
    opll->noise >>= 1;
  }
}

static void update_short_noise(OPLL *opll) {

  const uint32_t pg_hh = opll->slot[SLOT_HH].pg_out;
  const uint32_t pg_cym = opll->slot[SLOT_CYM].pg_out;

  const uint8_t h_bit2 = BIT(pg_hh, PG_BITS - 8);
  const uint8_t h_bit7 = BIT(pg_hh, PG_BITS - 3);
  const uint8_t h_bit3 = BIT(pg_hh, PG_BITS - 7);

  const uint8_t c_bit3 = BIT(pg_cym, PG_BITS - 7);
  const uint8_t c_bit5 = BIT(pg_cym, PG_BITS - 5);

  opll->short_noise = (h_bit2 ^ h_bit7) | (h_bit3 ^ c_bit5) | (c_bit3 ^ c_bit5);
}

static INLINE void calc_phase(OPLL_SLOT *slot, int32_t pm_phase, uint8_t reset, uint8_t mult) {
  const int8_t pm = slot->patch->PM ? pm_table[(slot->fnum >> 6) & 7][(pm_phase >> 10) & 7] : 0;
  if (reset) {
    slot->pg_phase = 0;
  }
  slot->pg_phase += ((((slot->fnum & 0x1ff) * 2 + pm) * ml_table[slot->patch->ML]) << slot->blk >> 2) * mult;
  slot->pg_phase &= (DP_WIDTH - 1);
  slot->pg_out = slot->pg_phase >> DP_BASE_BITS;
}

static INLINE uint8_t lookup_attack_step(OPLL_SLOT *slot, uint32_t counter) {
  int index;
  switch (slot->eg_rate_h) {
    case 12:
      index = (counter & 0xc) >> 1;
      return 4 - eg_step_tables[slot->eg_rate_l][index];
    case 13:
      index = (counter & 0xc) >> 1;
      return 3 - eg_step_tables[slot->eg_rate_l][index];
    case 14:
      index = (counter & 0xc) >> 1;
      return 2 - eg_step_tables[slot->eg_rate_l][index];
    case 0:
    case 15:
      return 0;
    default:
      index = counter >> slot->eg_shift;
      return eg_step_tables[slot->eg_rate_l][index & 7] ? 4 : 0;
  }
}

static INLINE uint8_t lookup_decay_step(OPLL_SLOT *slot, uint32_t counter) {
  int index;
  switch (slot->eg_rate_h) {
    case 0:
      return 0;
    case 13:
      index = ((counter & 0xc) >> 1) | (counter & 1);
      return eg_step_tables[slot->eg_rate_l][index];
    case 14:
      index = ((counter & 0xc) >> 1);
      return eg_step_tables[slot->eg_rate_l][index] + 1;
    case 15:
      return 2;
    default:
      index = counter >> slot->eg_shift;
      return eg_step_tables[slot->eg_rate_l][index & 7];
  }
}

static INLINE void start_envelope(OPLL_SLOT *slot) {
  if (min(15, slot->patch->AR + (slot->rks >> 2)) == 15) {
    slot->eg_state = DECAY;
    slot->eg_out = 0;
  } else {
    slot->eg_state = ATTACK;
  }
  request_update(slot, UPDATE_EG);
}

static INLINE void calc_envelope(OPLL_SLOT *slot, OPLL_SLOT *buddy, uint16_t eg_counter, uint8_t test) {
  uint32_t mask = (1 << slot->eg_shift) - 1;
  uint8_t s;

  if (slot->eg_state == ATTACK) {
    if (0 < slot->eg_out && 0 < slot->eg_rate_h && (eg_counter & mask & ~3) == 0) {
      s = lookup_attack_step(slot, eg_counter);
      if (0 < s) {
        slot->eg_out = max(0, ((int)slot->eg_out - (slot->eg_out >> s) - 1));
      }
    }
  } else {
    if (slot->eg_rate_h > 0 && (eg_counter & mask) == 0) {
      slot->eg_out = min(EG_MUTE, slot->eg_out + lookup_decay_step(slot, eg_counter));
    }
  }

  switch (slot->eg_state) {
    case DAMP:
      if (slot->eg_out >= EG_MAX && (eg_counter & mask) == 0) {
        start_envelope(slot);
        // slot->type に関わらず、自身の pg_phase をリセット
        if (!slot->pg_keep) {
          slot->pg_phase = 0;
        }
        // 相方（buddy）が存在すれば、相方の pg_phase も同時にリセット
        if (buddy && !buddy->pg_keep) {
          buddy->pg_phase = 0;
        }
      }
      break;

    case ATTACK:
      if (slot->eg_out == 0) {
        slot->eg_state = DECAY;
        request_update(slot, UPDATE_EG);
      }
      break;

    case DECAY:
      if ((slot->eg_out >> 3) >= slot->patch->SL) {
        slot->eg_state = SUSTAIN;
        request_update(slot, UPDATE_EG);
      }
      break;

    case SUSTAIN:
    case RELEASE:
    default:
      break;
  }

  if (test) {
    slot->eg_out = 0;
  }
}

static void update_slots(OPLL *opll) {
  int i;
  opll->eg_counter += opll->phase_mult;

  for (i = 0; i < 18; i++) {
    OPLL_SLOT *slot = &opll->slot[i];
    OPLL_SLOT *buddy = NULL;
    if (slot->type == 0) buddy = &opll->slot[i + 1];
    if (slot->type == 1) buddy = &opll->slot[i - 1];

    if (slot->update_requests) {
      commit_slot_update(slot);
    }
    calc_envelope(slot, buddy, opll->eg_counter, opll->test_flag & 1);
    calc_phase(slot, opll->pm_phase, opll->test_flag & 4, opll->phase_mult);
  }
}

static INLINE int16_t lookup_exp_table(uint16_t i) {
  int16_t t = (exp_table[(i & 0xff) ^ 0xff] + 1024);
  int16_t res = t >> ((i & 0x7f00) >> 8);
  return ((i & 0x8000) ? ~res : res) << 1;
}

/* Synthesize wave on the fly from 1/4th logsin_table (Replaces 4KB Wave Tables) */
static INLINE uint16_t lookup_wave(uint16_t phase, uint8_t ws) {
  if (ws == 1 && phase >= (PG_WIDTH / 2)) return 0xfff;  // halfsin

  uint16_t p = phase & (PG_WIDTH / 2 - 1);
  if (p >= (PG_WIDTH / 4)) p = (PG_WIDTH / 2 - 1) - p;  // Mirroring

  uint16_t val = logsin_table[p];
  if (phase >= (PG_WIDTH / 2)) val |= 0x8000;  // Sign inversion
  return val;
}

static INLINE int16_t to_linear(uint16_t h, OPLL_SLOT *slot, int16_t am) {
  uint16_t att;
  if (slot->eg_out > EG_MAX)
    return 0;

  att = min(EG_MUTE, (slot->eg_out + slot->tll + am)) << 4;
  return lookup_exp_table(h + att);
}

static INLINE int16_t calc_slot_car(OPLL *opll, int ch, int16_t fm) {
  OPLL_SLOT *slot = CAR(opll, ch);
  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;
  uint16_t phase = (slot->pg_out + 2 * (fm >> 1)) & (PG_WIDTH - 1);

  slot->output[1] = slot->output[0];
  slot->output[0] = to_linear(lookup_wave(phase, slot->patch->WS), slot, am);
  return slot->output[0];
}

static INLINE int16_t calc_slot_mod(OPLL *opll, int ch) {
  OPLL_SLOT *slot = MOD(opll, ch);
  int16_t fm = slot->patch->FB > 0 ? (slot->output[1] + slot->output[0]) >> (9 - slot->patch->FB) : 0;
  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;
  uint16_t phase = (slot->pg_out + fm) & (PG_WIDTH - 1);

  slot->output[1] = slot->output[0];
  slot->output[0] = to_linear(lookup_wave(phase, slot->patch->WS), slot, am);
  return slot->output[0];
}

static INLINE int16_t calc_slot_tom(OPLL *opll) {
  OPLL_SLOT *slot = &opll->slot[SLOT_TOM];
  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;
  return to_linear(lookup_wave(slot->pg_out, slot->patch->WS), slot, am);
}

static INLINE int16_t calc_slot_snare(OPLL *opll, uint8_t noise) {
  OPLL_SLOT *slot = &opll->slot[SLOT_SD];
  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;
  uint16_t phase = slot->pg_out;

  if (BIT(slot->pg_out, PG_BITS - 2)) {
    phase += noise ? 0x100 : 0x230;
  } else {
    phase += noise ? 0x230 : 0x100;
  }
  phase &= (PG_WIDTH - 1);

  return to_linear(lookup_wave(phase, slot->patch->WS), slot, am);
}

static INLINE int16_t calc_slot_hat(OPLL *opll, uint8_t short_noise, uint8_t noise) {
  OPLL_SLOT *slot = &opll->slot[SLOT_HH];
  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;
  uint16_t phase;
  if (short_noise) {
    phase = noise ? 0x300 : 0x80;
  } else {
    phase = noise ? 0x1f0 : 0x1d0;
  }
  return to_linear(lookup_wave(phase, slot->patch->WS), slot, am);
}

static INLINE int16_t calc_slot_cym(OPLL *opll, uint8_t short_noise) {
  OPLL_SLOT *slot = &opll->slot[SLOT_CYM];
  uint8_t am = slot->patch->AM ? opll->lfo_am : 0;
  uint16_t phase = short_noise ? 0x280 : 0x100;
  uint8_t cym_bit = (slot->pg_phase >> 14) & 1;
  uint8_t hh_bit = (opll->slot[14].pg_phase >> 13) & 1;
  uint8_t metallic_bit = cym_bit ^ hh_bit;
  uint8_t noise_bit = opll->noise & 1;

  if (metallic_bit) {
    phase = short_noise ? 0x1A0 : 0x300;
  }

  if (noise_bit) {
    phase ^= 0x080;
  }

  return to_linear(lookup_wave(phase, slot->patch->WS), slot, am);
}

static void OPLL_update_output(OPLL *opll) {
  int32_t i;
  uint8_t r14 = opll->reg[0x0e];
  uint8_t rhythm = BIT(r14, 5);

  update_ampm(opll);
  update_noise(opll, 1);
  update_short_noise(opll);
  update_slots(opll);
  memset(opll->ch_out, 0, sizeof(opll->ch_out));

  for (i = 0; i < 6; i++) {
    if (!(opll->mask & OPLL_MASK_CH(i))) {
      int16_t feedback = calc_slot_mod(opll, i);
      opll->ch_out[i] = calc_slot_car(opll, i, feedback);
    }
  }

  if (!rhythm) {
    for (i = 6; i < 9; i++) {
      if (!(opll->mask & OPLL_MASK_CH(i))) {
        int16_t feedback = calc_slot_mod(opll, i);
        opll->ch_out[i] = calc_slot_car(opll, i, feedback);
      }
    }
  } else {
    if (!(opll->mask & OPLL_MASK_BD)) {
      int16_t feedback = calc_slot_mod(opll, 6);
      opll->ch_out[9] = (calc_slot_car(opll, 6, feedback) << 1);
    }
    uint8_t noise = opll->noise & 1;
    if (!(opll->mask & OPLL_MASK_HH)) {
      opll->ch_out[10] = calc_slot_hat(opll, opll->short_noise, noise);
    }
    if (!(opll->mask & OPLL_MASK_SD)) {
      opll->ch_out[11] = calc_slot_snare(opll, noise);
    }
    if (!(opll->mask & OPLL_MASK_TOM)) {
      opll->ch_out[12] = calc_slot_tom(opll);
    }
    if (!(opll->mask & OPLL_MASK_CYM)) {
      opll->ch_out[13] = (calc_slot_cym(opll, opll->short_noise) * 3) >> 1;
    }
    calc_slot_mod(opll, 7);
    calc_slot_mod(opll, 8);
  }
}

static void OPLL_mix_output(OPLL *opll, int32_t out[2]) {
  int32_t i;
  out[0] = 0;
  out[1] = 0;
  for (i = 0; i < 14; i++) {
    if (opll->pan[i] & 2) out[0] += opll->ch_out[i];  // Left
    if (opll->pan[i] & 1) out[1] += opll->ch_out[i];  // Right
  }
}

/*********************************************************

                     External APIs

*********************************************************/
void OPLL_advance(OPLL *opll) {
  update_ampm(opll);
  update_noise(opll, 1);
  update_short_noise(opll);
  update_slots(opll);
}

int16_t OPLL_calc(OPLL *opll) {
  if (opll->conv) {
    opll->conv->f_ratio = 49716.0f / 16384.0f;
    opll->conv->timer += opll->conv->f_ratio;
    while (opll->conv->timer >= 1.0f) {
      OPLL_update_output(opll);
      int32_t mix[2];
      OPLL_mix_output(opll, mix);
      OPLL_RateConv_putData(opll->conv, 0, (int16_t)((mix[0] + mix[1]) / 2));
      opll->conv->timer -= 1.0f;
    }
    return OPLL_RateConv_getData(opll->conv, 0);
  } else {
    OPLL_update_output(opll);
    int32_t mix[2];
    OPLL_mix_output(opll, mix);
    return (int16_t)((mix[0] + mix[1]) / 2);
  }
}

void OPLL_calcStereo(OPLL *opll, int32_t out[2]) {
  if (opll->conv) {
    opll->conv->timer += opll->conv->f_ratio;
    while (opll->conv->timer >= 1.0f) {
      OPLL_update_output(opll);
      int32_t mix[2];
      OPLL_mix_output(opll, mix);
      OPLL_RateConv_putData(opll->conv, 0, (int16_t)mix[0]);
      OPLL_RateConv_putData(opll->conv, 1, (int16_t)mix[1]);
      opll->conv->timer -= 1.0f;
    }
    out[0] = OPLL_RateConv_getData(opll->conv, 0);
    out[1] = OPLL_RateConv_getData(opll->conv, 1);
  } else {
    OPLL_update_output(opll);
    int32_t mix[2];
    OPLL_mix_output(opll, mix);
    out[0] = mix[0];
    out[1] = mix[1];
  }
}

void OPLL_writeReg(OPLL *opll, uint32_t reg, uint8_t val) {
  int ch;
  if (reg >= 0x40) return;
  opll->reg[reg] = val;

  if (reg <= 0x07) {
    OPLL_PATCH *p = &opll->patch[0];
    OPLL_PATCH *c = &opll->patch[1];
    switch (reg) {
      case 0x00:
        p->AM = (val >> 7) & 1;
        p->PM = (val >> 6) & 1;
        p->EG = (val >> 5) & 1;
        p->KR = (val >> 4) & 1;
        p->ML = val & 15;
        //if (p->ML > 4) p->ML = 4;
        break;
      case 0x01:
        c->AM = (val >> 7) & 1;
        c->PM = (val >> 6) & 1;
        c->EG = (val >> 5) & 1;
        c->KR = (val >> 4) & 1;
        c->ML = val & 15;
        //if (c->ML > 4) c->ML = 4;
        break;
      case 0x02:
        p->KL = (val >> 6) & 3;
        p->TL = val & 63;
        break;
      case 0x03:
        c->KL = (val >> 6) & 3;
        p->FB = (val >> 1) & 7;
        c->WS = val & 1;
        break;
      case 0x04:
        p->AR = (val >> 4) & 15;
        p->DR = val & 15;
        break;
      case 0x05:
        c->AR = (val >> 4) & 15;
        c->DR = val & 15;
        break;
      case 0x06:
        p->SL = (val >> 4) & 15;
        p->RR = val & 15;
        break;
      case 0x07:
        c->SL = (val >> 4) & 15;
        c->RR = val & 15;
        break;
    }
    return;
  }

  if (reg == 0x0e) {
    update_rhythm_mode(opll);
    update_key_status(opll);
    return;
  }

  // F-Number Low
  if (0x10 <= reg && reg <= 0x18) {
    ch = reg - 0x10;
    set_fnumber(opll, ch, (opll->reg[0x20 + ch] & 1) << 8 | val);
    return;
  }

  // Block / F-Number High / KeyOn / Sustain
  if (0x20 <= reg && reg <= 0x28) {
    ch = reg - 0x20;
    set_fnumber(opll, ch, (val & 1) << 8 | opll->reg[0x10 + ch]);
    set_block(opll, ch, (val >> 1) & 7);
    set_sus_flag(opll, ch, (val >> 5) & 1);
    update_key_status(opll);
    return;
  }

  if (0x30 <= reg && reg <= 0x38) {
    ch = reg - 0x30;
    set_volume(opll, ch, (val & 15) << 2);
    if (opll->rhythm_mode && ch >= 6) {
      if (ch == 6) {
        set_slot_volume(&opll->slot[SLOT_BD2], (val & 15) << 2);
      } else if (ch == 7) {
        set_slot_volume(&opll->slot[SLOT_HH], ((val >> 4) & 15) << 2);
        set_slot_volume(&opll->slot[SLOT_SD], (val & 15) << 2);
      } else if (ch == 8) {
        set_slot_volume(&opll->slot[SLOT_TOM], ((val >> 4) & 15) << 2);
        set_slot_volume(&opll->slot[SLOT_CYM], (val & 15) << 2);
      }
    } else {
      set_patch(opll, ch, val >> 4);
    }
    return;
  }
}

OPLL *OPLL_new(uint32_t clk, uint32_t rate) {
  OPLL *opll = calloc(1, sizeof(OPLL));
  if (!table_initialized) {
    initializeTables();
  }
  opll->clk = clk;
  opll->rate = rate;
  opll->mask = 0;

  for (int i = 0; i < 16; i++) {
    opll->pan[i] = 3;
  }

  uint32_t internal_hz = clk / 72;
  if (rate != internal_hz) {
    uint32_t mult = (internal_hz + rate / 2) / rate;  // 四捨五入
    opll->phase_mult = (uint8_t)(mult < 1 ? 1 : (mult > 8 ? 8 : mult));
  } else {
    opll->phase_mult = 1;
  }
 
  OPLL_reset(opll);
  return opll;
}

void OPLL_delete(OPLL *opll) {
  if (opll->conv) {
    OPLL_RateConv_delete(opll->conv);
  }
  free(opll);
}

void OPLL_reset(OPLL *opll) {
  int i;
  opll->adr = 0;
  opll->test_flag = 0;
  opll->slot_key_status = 0;
  opll->rhythm_mode = 0;
  opll->eg_counter = 0;
  opll->pm_phase = 0;
  opll->am_phase = 0;
  opll->lfo_am = 0;
  opll->noise = 0x7ffff;
  opll->short_noise = 0;

  for (i = 0; i < 18; i++) {
    reset_slot(&opll->slot[i], i);
  }
  for (i = 0; i < 9; i++) {
    opll->patch_number[i] = 0;
  }

  memset(opll->reg, 0, sizeof(opll->reg));
  OPLL_resetPatch(opll, OPLL_2413_TONE);

  for (i = 0; i < 9; i++) {
    set_patch(opll, i, 0);
  }
}

void OPLL_resetPatch(OPLL *opll, uint8_t tone_type) {
  int i;
  opll->chip_type = tone_type;
  for (i = 0; i < 19; i++) {
    const uint8_t *dump = default_inst[tone_type] + (i * 8);
    uint8_t buf[8];
    for (int j = 0; j < 8; j++) {
      buf[j] = pgm_read_byte(dump + j);
    }
    OPLL_dumpToPatch(buf, &opll->patch[i * 2]);
  }
}

void OPLL_dumpToPatch(const uint8_t *dump, OPLL_PATCH *patch) {
  patch[0].AM = (dump[0] >> 7) & 1;
  patch[0].PM = (dump[0] >> 6) & 1;
  patch[0].EG = (dump[0] >> 5) & 1;
  patch[0].KR = (dump[0] >> 4) & 1;
  patch[0].ML = dump[0] & 15;

  patch[1].AM = (dump[1] >> 7) & 1;
  patch[1].PM = (dump[1] >> 6) & 1;
  patch[1].EG = (dump[1] >> 5) & 1;
  patch[1].KR = (dump[1] >> 4) & 1;
  patch[1].ML = dump[1] & 15;

  patch[0].KL = (dump[2] >> 6) & 3;
  patch[0].TL = dump[2] & 63;

  patch[1].KL = (dump[3] >> 6) & 3;
  patch[0].WS = (dump[3] >> 4) & 1;
  patch[1].WS = (dump[3] >> 3) & 1;
  patch[0].FB = dump[3] & 7;

  patch[0].AR = (dump[4] >> 4) & 15;
  patch[0].DR = dump[4] & 15;
  patch[1].AR = (dump[5] >> 4) & 15;
  patch[1].DR = dump[5] & 15;

  patch[0].SL = (dump[6] >> 4) & 15;
  patch[0].RR = dump[6] & 15;
  patch[1].SL = (dump[7] >> 4) & 15;
  patch[1].RR = dump[7] & 15;
}

void OPLL_patchToDump(const OPLL_PATCH *patch, uint8_t *dump) {
  dump[0] = (patch[0].AM << 7) | (patch[0].PM << 6) | (patch[0].EG << 5) | (patch[0].KR << 4) | patch[0].ML;
  dump[1] = (patch[1].AM << 7) | (patch[1].PM << 6) | (patch[1].EG << 5) | (patch[1].KR << 4) | patch[1].ML;
  dump[2] = (patch[0].KL << 6) | patch[0].TL;
  dump[3] = (patch[1].KL << 6) | (patch[0].WS << 4) | (patch[1].WS << 3) | patch[0].FB;
  dump[4] = (patch[0].AR << 4) | patch[0].DR;
  dump[5] = (patch[1].AR << 4) | patch[1].DR;
  dump[6] = (patch[0].SL << 4) | patch[0].RR;
  dump[7] = (patch[1].SL << 4) | patch[1].RR;
}

void OPLL_getDefaultPatch(int32_t type, int32_t num, OPLL_PATCH *patch) {
  const uint8_t *dump = default_inst[type] + (num * 8);
  uint8_t buf[8];
  for (int j = 0; j < 8; j++) {
    buf[j] = pgm_read_byte(dump + j);
  }
  OPLL_dumpToPatch(buf, patch);
}

void OPLL_setRate(OPLL *opll, uint32_t rate) {
  opll->rate = rate;
  if (opll->conv) {
    OPLL_RateConv_delete(opll->conv);
    opll->conv = NULL;
  }
  uint32_t internal_hz = opll->clk / 72;
  if (rate != internal_hz) {
    uint32_t mult = (internal_hz + rate / 2) / rate;
    opll->phase_mult = (uint8_t)(mult < 1 ? 1 : (mult > 8 ? 8 : mult));
  } else {
    opll->phase_mult = 1;
  }
}

void OPLL_setQuality(OPLL *opll, uint8_t q) {
  (void)opll;
  (void)q;
}

void OPLL_setPan(OPLL *opll, uint32_t ch, uint8_t pan) {
  if (ch < 16) opll->pan[ch] = pan;
}

void OPLL_setPanFine(OPLL *opll, uint32_t ch, float pan[2]) {
  if (ch < 16) {
    if (pan[0] == 0.0f) opll->pan[ch] = 1;       // Right Only
    else if (pan[1] == 0.0f) opll->pan[ch] = 2;  // Left Only
    else opll->pan[ch] = 3;                      // Center
  }
}

void OPLL_setChipType(OPLL *opll, uint8_t type) {
  opll->chip_type = type;
}
void OPLL_writeIO(OPLL *opll, uint32_t reg, uint8_t val) {
  if (reg & 1) OPLL_writeReg(opll, opll->adr, val);
  else opll->adr = val;
}

void OPLL_setPatch(OPLL *opll, const uint8_t *dump) {
  OPLL_dumpToPatch(dump, &opll->patch[0]);
  for (int i = 0; i < 9; i++) {
    if (opll->patch_number[i] == 0) set_patch(opll, i, 0);
  }
}

void OPLL_copyPatch(OPLL *opll, int32_t num, OPLL_PATCH *patch) {
  memcpy(&opll->patch[num * 2], patch, sizeof(OPLL_PATCH) * 2);
}

void OPLL_forceRefresh(OPLL *opll) {
  for (int i = 0; i < 9; i++) {
    set_patch(opll, i, opll->patch_number[i]);
  }
}

uint32_t OPLL_setMask(OPLL *opll, uint32_t mask) {
  uint32_t ret = opll->mask;
  opll->mask = mask;
  return ret;
}

uint32_t OPLL_toggleMask(OPLL *opll, uint32_t mask) {
  uint32_t ret = opll->mask;
  opll->mask ^= mask;
  return ret;
}