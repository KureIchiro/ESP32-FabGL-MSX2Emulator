#ifndef _EMU2413_H_
#define _EMU2413_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPLL_DEBUG 0

enum OPLL_TONE_ENUM { OPLL_2413_TONE = 0, OPLL_VRC7_TONE = 1, OPLL_281B_TONE = 2 };

typedef struct __OPLL_PATCH {
  uint32_t TL, FB, EG, ML, AR, DR, SL, RR, KR, KL, AM, PM, WS;
} OPLL_PATCH;

typedef struct __OPLL_SLOT {
  uint8_t number;

  uint8_t type;

  OPLL_PATCH *patch;

  int32_t output[2];

  uint32_t pg_phase;
  uint32_t pg_out;
  uint8_t pg_keep;
  uint16_t blk_fnum;
  uint16_t fnum;
  uint8_t blk;

  uint8_t eg_state;
  int32_t volume;
  uint8_t key_flag;
  uint8_t sus_flag;
  uint16_t tll;
  uint8_t rks;
  uint8_t eg_rate_h;
  uint8_t eg_rate_l;
  uint32_t eg_shift;
  uint32_t eg_out;

  uint32_t update_requests;

#if OPLL_DEBUG
  uint8_t last_eg_state;
#endif
} OPLL_SLOT;

#define OPLL_MASK_CH(x) (1 << (x))
#define OPLL_MASK_HH (1 << (9))
#define OPLL_MASK_CYM (1 << (10))
#define OPLL_MASK_TOM (1 << (11))
#define OPLL_MASK_SD (1 << (12))
#define OPLL_MASK_BD (1 << (13))
#define OPLL_MASK_RHYTHM (OPLL_MASK_HH | OPLL_MASK_CYM | OPLL_MASK_TOM | OPLL_MASK_SD | OPLL_MASK_BD)

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

  int16_t ch_out[14];

  int16_t mix_out[2];

  OPLL_RateConv *conv;
} OPLL;

OPLL *OPLL_new(uint32_t clk, uint32_t rate);
void OPLL_delete(OPLL *);

void OPLL_reset(OPLL *);
void OPLL_resetPatch(OPLL *, uint8_t);

void OPLL_setRate(OPLL *opll, uint32_t rate);

void OPLL_setQuality(OPLL *opll, uint8_t q);

void OPLL_setPan(OPLL *opll, uint32_t ch, uint8_t pan);

void OPLL_setPanFine(OPLL *opll, uint32_t ch, float pan[2]);

void OPLL_setChipType(OPLL *opll, uint8_t type);

void OPLL_writeIO(OPLL *opll, uint32_t reg, uint8_t val);
void OPLL_writeReg(OPLL *opll, uint32_t reg, uint8_t val);

void OPLL_advance(OPLL *opll);

int16_t OPLL_calc(OPLL *opll);

void OPLL_calcStereo(OPLL *opll, int32_t out[2]);

void OPLL_setPatch(OPLL *, const uint8_t *dump);
void OPLL_copyPatch(OPLL *, int32_t, OPLL_PATCH *);

void OPLL_forceRefresh(OPLL *);

void OPLL_dumpToPatch(const uint8_t *dump, OPLL_PATCH *patch);
void OPLL_patchToDump(const OPLL_PATCH *patch, uint8_t *dump);
void OPLL_getDefaultPatch(int32_t type, int32_t num, OPLL_PATCH *);

uint32_t OPLL_setMask(OPLL *, uint32_t mask);

uint32_t OPLL_toggleMask(OPLL *, uint32_t mask);

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
