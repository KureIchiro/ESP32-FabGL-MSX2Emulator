#ifndef _TMS9918A_H
#define _TMS9918A_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
void tms9918a_rasterize(struct tms9918a *vdp);
void tms9918a_reset(struct tms9918a *vdp);
void tms9918a_write(struct tms9918a *vdp, uint8_t addr, uint8_t val);
uint8_t tms9918a_read(struct tms9918a *vdp, uint8_t addr);
int tms9918a_irq_pending(struct tms9918a *vdp);
void flip_buffer(struct tms9918a *vdp);
struct tms9918a *tms9918a_create(void);
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
};
extern uint8_t sram_framebuffer[];

#ifdef __cplusplus
}
#endif
#endif
