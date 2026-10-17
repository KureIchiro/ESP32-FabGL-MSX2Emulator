#ifndef _EMU2413_H_
#define _EMU2413_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPLL_DEBUG 0

enum OPLL_TONE_ENUM { OPLL_2413_TONE = 0, OPLL_VRC7_TONE = 1, OPLL_281B_TONE = 2 };

/* voice data */
typedef struct __OPLL_PATCH {
  uint32_t TL, FB, EG, ML, AR, DR, SL, RR, KR, KL, AM, PM, WS;
} OPLL_PATCH;

/* slot */
typedef struct __OPLL_SLOT {
  uint8_t number;

  /* type flags:
   * 000000SM
   *       |+-- M: 0:modulator 1:carrier
   *       +--- S: 0:normal 1:single slot mode (sd, tom, hh or cym)
   */
  uint8_t type;

  OPLL_PATCH *patch; /* voice parameter */

  /* slot output */
  int32_t output[2]; /* output value, latest and previous. */

  /* phase generator (pg) */
  uint32_t pg_phase;    /* pg phase */
  uint32_t pg_out;      /* pg output, as index of wave table */
  uint8_t pg_keep;      /* if 1, pg_phase is preserved when key-on */
  uint16_t blk_fnum;    /* (block << 9) | f-number */
  uint16_t fnum;        /* f-number (9 bits) */
  uint8_t blk;          /* block (3 bits) */

  /* envelope generator (eg) */
  uint8_t eg_state;  /* current state */
  int32_t volume;    /* current volume */
  uint8_t key_flag;  /* key-on flag 1:on 0:off */
  uint8_t sus_flag;  /* key-sus option 1:on 0:off */
  uint16_t tll;      /* total level + key scale level*/
  uint8_t rks;       /* key scale offset (rks) for eg speed */
  uint8_t eg_rate_h; /* eg speed rate high 4bits */
  uint8_t eg_rate_l; /* eg speed rate low 2bits */
  uint32_t eg_shift; /* shift for eg global counter, controls envelope speed */
  uint32_t eg_out;   /* eg output */

  uint32_t update_requests; /* flags to debounce update */

#if OPLL_DEBUG
  uint8_t last_eg_state;
#endif
} OPLL_SLOT;

/* mask */
#define OPLL_MASK_CH(x) (1 << (x))
#define OPLL_MASK_HH (1 << (9))
#define OPLL_MASK_CYM (1 << (10))
#define OPLL_MASK_TOM (1 << (11))
#define OPLL_MASK_SD (1 << (12))
#define OPLL_MASK_BD (1 << (13))
#define OPLL_MASK_RHYTHM (OPLL_MASK_HH | OPLL_MASK_CYM | OPLL_MASK_TOM | OPLL_MASK_SD | OPLL_MASK_BD)

/* rate converter */
typedef struct __OPLL_RateConv {
  int ch;
  float timer;
  float f_ratio;
  int16_t **buf;
} OPLL_RateConv;

OPLL_RateConv *OPLL_RateConv_new(float f_inp, float f_out, int ch);
void OPLL_RateConv_reset(OPLL_RateConv *conv);
void OPLL_RateConv_putData(OPLL_RateConv *conv, int ch, int16_t data);
int16_t OPLL_RateConv_getData(OPLL_RateConv *conv, int ch);
void OPLL_RateConv_delete(OPLL_RateConv *conv);

typedef struct __OPLL {
  uint32_t clk;
  uint32_t rate;
  uint8_t chip_type;
  uint32_t adr;
  float inp_step;
  float out_step;
  float out_time;
  uint8_t reg[0x40];
  uint8_t test_flag;
  uint32_t slot_key_status;
  uint8_t rhythm_mode;
  uint32_t eg_counter;
  uint8_t phase_mult;
  uint32_t pm_phase;
  int32_t am_phase;
  uint8_t lfo_am;
  uint32_t noise;
  uint8_t short_noise;
  int32_t patch_number[9];
  OPLL_SLOT slot[18];
  OPLL_PATCH patch[19 * 2];
  uint8_t pan[16];
  float pan_fine[16][2];
  uint32_t mask;

  /* channel output */
  /* 0..8:tone 9:bd 10:hh 11:sd 12:tom 13:cym */
  int16_t ch_out[14];

  int16_t mix_out[2];

  OPLL_RateConv *conv;
} OPLL;

OPLL *OPLL_new(uint32_t clk, uint32_t rate);
void OPLL_delete(OPLL *);

void OPLL_reset(OPLL *);
void OPLL_resetPatch(OPLL *, uint8_t);

/**
 * Set output wave sampling rate.
 */
void OPLL_setRate(OPLL *opll, uint32_t rate);

/**
 * Set internal calcuration quality. (Deprecated)
 */
void OPLL_setQuality(OPLL *opll, uint8_t q);

/**
 * Set pan pot (extra function - not YM2413 chip feature)
 */
void OPLL_setPan(OPLL *opll, uint32_t ch, uint8_t pan);

/**
 * Set fine-grained panning
 */
void OPLL_setPanFine(OPLL *opll, uint32_t ch, float pan[2]);

/**
 * Set chip type. 0:YM2413 1:VRC7
 */
void OPLL_setChipType(OPLL *opll, uint8_t type);

void OPLL_writeIO(OPLL *opll, uint32_t reg, uint8_t val);
void OPLL_writeReg(OPLL *opll, uint32_t reg, uint8_t val);

/**
 * Advance internal state by one 49716Hz tick WITHOUT computing audio output.
 * Call this 2x before OPLL_calc() to step the chip at 49716Hz while only
 * synthesizing audio once per 16384Hz output sample.
 */
void OPLL_advance(OPLL *opll);

/**
 * Calculate one sample
 */
int16_t OPLL_calc(OPLL *opll);

/**
 * Calulate stereo sample
 */
void OPLL_calcStereo(OPLL *opll, int32_t out[2]);

void OPLL_setPatch(OPLL *, const uint8_t *dump);
void OPLL_copyPatch(OPLL *, int32_t, OPLL_PATCH *);

/**
 * Force to refresh.
 */
void OPLL_forceRefresh(OPLL *);

void OPLL_dumpToPatch(const uint8_t *dump, OPLL_PATCH *patch);
void OPLL_patchToDump(const OPLL_PATCH *patch, uint8_t *dump);
void OPLL_getDefaultPatch(int32_t type, int32_t num, OPLL_PATCH *);

/**
 *  Set channel mask
 */
uint32_t OPLL_setMask(OPLL *, uint32_t mask);

/**
 * Toggler channel mask flag
 */
uint32_t OPLL_toggleMask(OPLL *, uint32_t mask);

/* for compatibility */
#define OPLL_set_rate OPLL_setRate
#define OPLL_set_quality OPLL_setQuality
#define OPLL_set_pan OPLL_setPan
#define OPLL_set_pan_fine OPLL_setPanFine
#define OPLL_calc_stereo OPLL_calcStereo
#define OPLL_reset_patch OPLL_resetPatch
#define OPLL_dump2patch OPLL_dumpToPatch
#define OPLL_patch2dump OPLL_patchToDump
#define OPLL_setChipMode OPLL_setChipType

#ifdef __cplusplus
}
#endif

#endif