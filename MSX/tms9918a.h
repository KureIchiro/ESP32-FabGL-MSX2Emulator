#ifndef _TMS9918A_H
#define _TMS9918A_H
#include <stdint.h>

#ifndef TMS_VBLANK_192
#define TMS_VBLANK_192 1
#endif

#ifdef __cplusplus
extern "C" {
void tms9918a_rasterize(struct tms9918a *vdp);
void tms9918a_reset(struct tms9918a *vdp);
void tms9918a_write(struct tms9918a *vdp, uint8_t addr, uint8_t val);
uint8_t tms9918a_read(struct tms9918a *vdp, uint8_t addr);
int tms9918a_irq_pending(struct tms9918a *vdp);
void flip_buffer(struct tms9918a *vdp);
struct tms9918a *tms9918a_create(void);
void tms9918a_vblank(struct tms9918a *vdp, int snapshot);
void tms9918a_vram_changed(struct tms9918a *vdp);
#endif

struct tms9918a {
  uint8_t reg[8];
  uint8_t status;
  unsigned int latch;
  unsigned int read;
  uint16_t addr;
  uint16_t memmask;
  int trace;
	int limit_sprites;

  uint8_t *framebuffer;
  uint8_t *active_raster;
  uint8_t *visible_raster;
  uint8_t colourmap[16];

  uint8_t rasterbuffer0[256 * 192];
  uint8_t rasterbuffer1[256 * 192];
	uint8_t sprite_line_list[192][32];
	uint8_t sprite_line_count[192];
	uint8_t colbuf[256 + 64];

	uint8_t *rvram;
	uint8_t *rreg;
	uint8_t rreg_buf[8];
	uint16_t dirty_lo, dirty_hi;
};
extern uint8_t sram_framebuffer[];

#ifdef __cplusplus
}
#endif
#endif
