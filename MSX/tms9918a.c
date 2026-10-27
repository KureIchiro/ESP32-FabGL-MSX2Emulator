#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <esp_attr.h>
#include <string.h>
#include "tms9918a.h"

#if defined(__GNUC__)
#define TMS_INLINE static inline __attribute__((always_inline))
#else
#define TMS_INLINE static __inline
#endif

uint8_t sram_framebuffer[16384] DRAM_ATTR __attribute__((aligned(4)));

static inline uint32_t expand_bits_to_color(uint8_t nibble, uint8_t color) {
	uint32_t res = 0;
	if (nibble & 0x08) res |= (uint32_t)color;
	if (nibble & 0x04) res |= (uint32_t)color << 8;
	if (nibble & 0x02) res |= (uint32_t)color << 16;
	if (nibble & 0x01) res |= (uint32_t)color << 24;
	return res;
}

static inline uint32_t expand_mag_bits_to_color(uint8_t two_bits, uint8_t color) {
	uint32_t res = 0;
	if (two_bits & 0x02) {
		res |= (uint32_t)color;
		res |= (uint32_t)color << 8;
	}
	if (two_bits & 0x01) {
		res |= (uint32_t)color << 16;
		res |= (uint32_t)color << 24;
	}
	return res;
}

static IRAM_ATTR void tms9918a_render_slice(struct tms9918a *vdp, int y, uint8_t *sprat, uint16_t bits, unsigned int width) {
	int x = sprat[1];
	if (sprat[3] & 0x80) x -= 32;

	int mag = vdp->reg[1] & 0x01;
	int real_width = mag ? (width << 1) : width;
	uint8_t foreground = vdp->colourmap[sprat[3] & 0x0F];

	if (width == 8) bits &= 0xFF00;
	uint8_t *out = &vdp->active_raster[256 * y];

	if (!mag) {
		for (int i = 0; i < (int)width; i += 4) {
			int target_x = x + i;
			uint8_t nibble = (uint8_t)((bits >> (12 - i)) & 0x0F);
			if (nibble == 0) continue;

			if (target_x >= 0 && target_x <= 252) {
				if (nibble == 0x0F) {
					out[(target_x + 0) ^ 2] = foreground;
					out[(target_x + 1) ^ 2] = foreground;
					out[(target_x + 2) ^ 2] = foreground;
					out[(target_x + 3) ^ 2] = foreground;
				} else {
					for (int b = 0; b < 4; b++) {
						if ((nibble >> (3 - b)) & 1) out[(target_x + b) ^ 2] = foreground;
					}
				}
			} else {
				for (int b = 0; b < 4; b++) {
					if ((nibble >> (3 - b)) & 1) {
						int tx = target_x + b;
						if (tx >= 0 && tx < 256) out[tx ^ 2] = foreground;
					}
				}
			}
		}
	} else {
		for (int i = 0; i < (int)width; i += 2) {
			int target_x = x + (i * 2);
			uint8_t two_bits = (uint8_t)((bits >> (14 - i)) & 0x03);

			if (two_bits == 0) continue;

			if (target_x >= 0 && target_x <= (256 - 4)) {
				if (two_bits == 0x03) {
					out[(target_x + 0) ^ 2] = foreground;
					out[(target_x + 1) ^ 2] = foreground;
					out[(target_x + 2) ^ 2] = foreground;
					out[(target_x + 3) ^ 2] = foreground;
				} else {
					for (int b = 0; b < 2; b++) {
						if ((two_bits >> (1 - b)) & 1) {
							int tx = target_x + (b * 2);
							out[tx ^ 2] = foreground;
							out[(tx + 1) ^ 2] = foreground;
						}
					}
				}
			} else {
				for (int b = 0; b < 2; b++) {
					if ((two_bits >> (1 - b)) & 1) {
						int tx = target_x + (b * 2);
						if (tx >= 0 && tx < 256) out[tx ^ 2] = foreground;
						if (tx + 1 >= 0 && tx + 1 < 256) out[(tx + 1) ^ 2] = foreground;
					}
				}
			}
		}
	}
}

TMS_INLINE uint16_t tms9918a_sprite_bits(struct tms9918a *vdp, int y, const uint8_t *sprat, const uint8_t *spdat) {
	int row = *sprat;
	if (row > 0xE0)
		row -= 0x100;
	row += 1;
	row = y - row;
	if (vdp->reg[1] & 0x01)
		row >>= 1;
	if ((vdp->reg[1] & 0x02) == 0)
		return (uint16_t)(spdat[row] << 8);
	return (uint16_t)((spdat[row] << 8) | spdat[row + 16]);
}

static IRAM_ATTR void tms9918a_render_sprite(struct tms9918a *vdp, int y, uint8_t *sprat, uint8_t *spdat) {
	unsigned int width = (vdp->reg[1] & 0x02) ? 16 : 8;
	tms9918a_render_slice(vdp, y, sprat, tms9918a_sprite_bits(vdp, y, sprat, spdat), width);
}

TMS_INLINE uint32_t tms9918a_sprite_mask(uint16_t bits, int mag) {
	uint32_t b = bits;
	if (!mag) return b << 16;
	b = (b | (b << 8)) & 0x00FF00FFu;
	b = (b | (b << 4)) & 0x0F0F0F0Fu;
	b = (b | (b << 2)) & 0x33333333u;
	b = (b | (b << 1)) & 0x55555555u;
	return b | (b << 1);
}

void prepare_sprites(struct tms9918a *vdp) {
	memset(vdp->sprite_line_count, 0, 192);
	uint8_t *sprat = vdp->framebuffer + ((vdp->reg[5] & 0x7F) << 7);
	int spheight = vdp->reg[1] & 0x02 ? 16 : 8;
	int mag = vdp->reg[1] & 0x01;
	if (mag) spheight <<= 1;

	for (int i = 0; i < 32; i++) {
		int ypos = sprat[i * 4];
		if (ypos == 0xD0) break;
		if (ypos > 0xE0) ypos -= 256;
		ypos += 1;

		int start_y = (ypos < 0) ? 0 : ypos;
		int end_y = (ypos + spheight > 192) ? 192 : ypos + spheight;

		for (int y = start_y; y < end_y; y++) {
			if (vdp->sprite_line_count[y] < 32) {
				vdp->sprite_line_list[y][vdp->sprite_line_count[y]++] = i;
			}
		}
	}
}

static IRAM_ATTR void tms9918a_sprite_line(struct tms9918a *vdp, int y) {
	uint8_t *sprat_base = vdp->framebuffer + ((vdp->reg[5] & 0x7F) << 7);
	uint8_t *spdat = vdp->framebuffer + ((vdp->reg[6] & 0x07) << 11);
	unsigned int spshft = 3;

	uint8_t patMask = (vdp->reg[1] & 0x02) ? 0xFC : 0xFF;

	int count = vdp->sprite_line_count[y];

	if (vdp->limit_sprites && count > 4) {
		if (!(vdp->status & 0x40))
			vdp->status = (vdp->status & 0xA0) | 0x40 | (vdp->sprite_line_list[y][4] & 0x1F);
		count = 4;
	}

	if (count > 1 && !(vdp->status & 0x20)) {
		uint32_t occ[9] = { 0 };
		int mag = vdp->reg[1] & 0x01;
		for (int n = 0; n < count; n++) {
			const uint8_t *sprat = sprat_base + vdp->sprite_line_list[y][n] * 4;
			uint32_t m = tms9918a_sprite_mask(tms9918a_sprite_bits(vdp, y, sprat, spdat + ((sprat[2] & patMask) << spshft)), mag);
			int x = sprat[1] - ((sprat[3] & 0x80) ? 32 : 0);
			if (x < 0) { m = (x <= -32) ? 0 : (m << -x); x = 0; }
			else if (x > 224) m &= ~(0xFFFFFFFFu >> (256 - x));
			uint32_t w = (uint32_t)x >> 5, s = (uint32_t)x & 31;
			uint32_t p0 = m >> s, p1 = s ? m << (32 - s) : 0;
			if ((p0 & occ[w]) | (p1 & occ[w + 1])) { vdp->status |= 0x20; break; }
			occ[w] |= p0; occ[w + 1] |= p1;
		}
	}

	for (int n = count - 1; n >= 0; n--) {
		uint8_t i = vdp->sprite_line_list[y][n];
		uint8_t *sprat = sprat_base + (i * 4);

		uint8_t foreground = sprat[3] & 0x0F;
		if (foreground == 0) continue;

		tms9918a_render_sprite(vdp, y, sprat, spdat + ((sprat[2] & patMask) << spshft));
	}
}

static IRAM_ATTR void tms9918a_raster_sprites(struct tms9918a *vdp) {
	memset(vdp->colbuf, 0xFF, sizeof(vdp->colbuf));

	unsigned int i;
	for (i = 0; i < 192; i++)
		tms9918a_sprite_line(vdp, i);
}

static void IRAM_ATTR tms9918a_rasterize_g1(struct tms9918a *vdp) {
	uint8_t *p_base = vdp->framebuffer + ((vdp->reg[2] & 0x0F) << 10);
	uint8_t *pattern = vdp->framebuffer + ((vdp->reg[4] & 0x07) << 11);
	uint8_t *colour = vdp->framebuffer + (vdp->reg[3] << 6);
	uint8_t *out = vdp->active_raster;

	for (int char_row = 0; char_row < 24; char_row++) {
		for (int scanline = 0; scanline < 8; scanline++) {
			uint8_t *p_line = p_base + char_row * 32;
			for (int x = 0; x < 32; x++) {
				uint8_t code = *p_line++;
				uint8_t b = pattern[(code << 3) + scanline];
				uint8_t c = colour[code >> 3];
				uint8_t c_arr[2] = { vdp->colourmap[c & 0x0F], vdp->colourmap[c >> 4] };

				*((uint32_t *)out) = c_arr[(b >> 5) & 1] | (c_arr[(b >> 4) & 1] << 8) | (c_arr[(b >> 7) & 1] << 16) | (c_arr[(b >> 6) & 1] << 24);
				*((uint32_t *)(out + 4)) = c_arr[(b >> 1) & 1] | (c_arr[b & 1] << 8) | (c_arr[(b >> 3) & 1] << 16) | (c_arr[(b >> 2) & 1] << 24);
				out += 8;
			}
		}
	}
	tms9918a_raster_sprites(vdp);
}

static IRAM_ATTR void tms9918a_rasterize_g2(struct tms9918a *vdp) {
	uint8_t *p_base = vdp->framebuffer + ((vdp->reg[2] & 0x0F) << 10);
	uint8_t *pattern0 = vdp->framebuffer + ((vdp->reg[4] & 0x04) << 11);
	uint8_t *colour0 = vdp->framebuffer + ((vdp->reg[3] & 0x80) << 6);
	uint8_t *out = vdp->active_raster;

	for (int char_row = 0; char_row < 24; char_row++) {
		uint8_t *pattern = pattern0;
		uint8_t *colour = colour0;

		if (char_row >= 8 && char_row < 16) {
			if (vdp->reg[4] & 0x01) pattern += 0x0800;
			if (vdp->reg[3] & 0x20) colour += 0x0800;
		} else if (char_row >= 16) {
			if (vdp->reg[4] & 0x02) pattern += 0x1000;
			if (vdp->reg[3] & 0x40) colour += 0x1000;
		}

		for (int scanline = 0; scanline < 8; scanline++) {
			uint8_t *p_line = p_base + char_row * 32;
			for (int x = 0; x < 32; x++) {
				uint8_t code = *p_line++;
				uint8_t b = pattern[(code << 3) + scanline];
				uint8_t c = colour[(code << 3) + scanline];
				uint8_t c_arr[2] = { vdp->colourmap[c & 0x0F], vdp->colourmap[c >> 4] };

				*((uint32_t *)out) = c_arr[(b >> 5) & 1] | (c_arr[(b >> 4) & 1] << 8) | (c_arr[(b >> 7) & 1] << 16) | (c_arr[(b >> 6) & 1] << 24);
				*((uint32_t *)(out + 4)) = c_arr[(b >> 1) & 1] | (c_arr[b & 1] << 8) | (c_arr[(b >> 3) & 1] << 16) | (c_arr[(b >> 2) & 1] << 24);
				out += 8;
			}
		}
	}
	tms9918a_raster_sprites(vdp);
}

static void IRAM_ATTR tms9918a_rasterize_mc(struct tms9918a *vdp) {
	uint8_t *p_base = vdp->framebuffer + ((vdp->reg[2] & 0x0F) << 10);
	uint8_t *pattern = vdp->framebuffer + ((vdp->reg[4] & 0x07) << 11);
	uint8_t *out = vdp->active_raster;

	for (int char_row = 0; char_row < 24; char_row++) {
		for (int scanline = 0; scanline < 8; scanline++) {
			uint8_t *p_line = p_base + char_row * 32;
			int block_y = (scanline >> 2) & 1;
			int pat_offset = (char_row & 3) << 1;

			for (int x = 0; x < 32; x++) {
				uint8_t code = *p_line++;
				uint8_t c = pattern[(code << 3) + pat_offset + block_y];
				uint8_t c_arr[2] = { vdp->colourmap[c >> 4], vdp->colourmap[c & 0x0F] };

				uint32_t p1 = c_arr[0] | (c_arr[0] << 8) | (c_arr[0] << 16) | (c_arr[0] << 24);
				uint32_t p2 = c_arr[1] | (c_arr[1] << 8) | (c_arr[1] << 16) | (c_arr[1] << 24);
				*((uint32_t *)out) = p1;
				*((uint32_t *)(out + 4)) = p2;
				out += 8;
			}
		}
	}
	tms9918a_raster_sprites(vdp);
}

static void IRAM_ATTR tms9918a_rasterize_text(struct tms9918a *vdp) {
	uint8_t *p_base = vdp->framebuffer + ((vdp->reg[2] & 0x0F) << 10);
	uint8_t *pattern = vdp->framebuffer + ((vdp->reg[4] & 0x07) << 11);
	uint8_t *out = vdp->active_raster;
	uint8_t bg = vdp->colourmap[vdp->reg[7] & 0x0F];
	uint8_t fg = vdp->colourmap[vdp->reg[7] >> 4];
	uint8_t c_arr[2] = { bg, fg };
	uint32_t bg_block = bg | (bg << 8) | (bg << 16) | (bg << 24);

	for (int char_row = 0; char_row < 24; char_row++) {
		for (int scanline = 0; scanline < 8; scanline++) {
			uint8_t *p_line = p_base + char_row * 40;
			uint8_t *line_start = out;

			*((uint32_t *)out) = bg_block;
			out += 4;
			*((uint32_t *)out) = bg_block;
			out += 4;

			int out_idx = 8;
			for (int x = 0; x < 40; x++) {
				uint8_t code = *p_line++;
				uint8_t b = pattern[(code << 3) + scanline];
				line_start[out_idx++ ^ 2] = c_arr[(b >> 7) & 1];
				line_start[out_idx++ ^ 2] = c_arr[(b >> 6) & 1];
				line_start[out_idx++ ^ 2] = c_arr[(b >> 5) & 1];
				line_start[out_idx++ ^ 2] = c_arr[(b >> 4) & 1];
				line_start[out_idx++ ^ 2] = c_arr[(b >> 3) & 1];
				line_start[out_idx++ ^ 2] = c_arr[(b >> 2) & 1];
			}
			out += 240;

			*((uint32_t *)out) = bg_block;
			out += 4;
			*((uint32_t *)out) = bg_block;
			out += 4;
		}
	}
}

void IRAM_ATTR tms9918a_rasterize(struct tms9918a *vdp) {
	prepare_sprites(vdp);
	unsigned int mode = (vdp->reg[1] >> 2) & 0x06;
	mode |= (vdp->reg[0] & 0x02) >> 1;

	uint8_t backdrop = vdp->colourmap[vdp->reg[7] & 0x0F];
	if ((vdp->reg[1] & 0x40) == 0)
		memset(vdp->active_raster, backdrop, 256 * 192);
	else {
		switch (mode) {
			case 0: tms9918a_rasterize_g1(vdp); break;
			case 1: tms9918a_rasterize_g2(vdp); break;
			case 2: tms9918a_rasterize_mc(vdp); break;
			case 4: tms9918a_rasterize_text(vdp); break;
			default: memset(vdp->active_raster, backdrop, 256 * 192);
		}
	}
	vdp->status |= 0x80;
}

void IRAM_ATTR tms9918a_write(struct tms9918a *vdp, uint8_t addr, uint8_t val) {
	switch (addr & 1) {
		case 0:
			vdp->framebuffer[vdp->addr] = val;
			vdp->addr++;
			vdp->addr &= vdp->memmask;
			vdp->latch = 0;
			break;
		case 1:
			if (vdp->latch == 0) {
				vdp->addr &= 0xFF00;
				vdp->addr |= val;
				vdp->latch = 1;
				return;
			}
			vdp->latch = 0;
			switch (val & 0xC0) {
				case 0x00:
					vdp->addr &= 0xFF;
					vdp->addr |= (val << 8);
					vdp->read = 1;
					vdp->addr &= vdp->memmask;
					break;
				case 0x40:
					vdp->addr &= 0xFF;
					vdp->addr |= (val << 8);
					vdp->addr &= vdp->memmask;
					vdp->read = 0;
					break;
				case 0x80:
					vdp->reg[val & 7] = vdp->addr & 0xFF;
					break;
			}
	}
}

uint8_t IRAM_ATTR tms9918a_read(struct tms9918a *vdp, uint8_t addr) {
	uint8_t r;
	if ((addr & 1) == 0) {
		r = vdp->framebuffer[vdp->addr++ & vdp->memmask];
	} else {
		r = vdp->status;
		vdp->status = 0;
	}
	vdp->latch = 0;
	return r;
}

void tms9918a_reset(struct tms9918a *vdp) {
	memset(vdp->reg, 0, sizeof(vdp->reg));
	vdp->addr = 0;
	vdp->latch = 0;
	vdp->read = 0;
	vdp->status = 0;
}

struct tms9918a *tms9918a_create(void) {
	struct tms9918a *vdp = (struct tms9918a *)ps_malloc(sizeof(struct tms9918a));
	if (vdp) {
		memset(vdp, 0, sizeof(struct tms9918a));
		vdp->framebuffer = sram_framebuffer;
		vdp->memmask = 0x3FFF;
		vdp->limit_sprites = 1;
	}
	return vdp;
}

int tms9918a_irq_pending(struct tms9918a *vdp) {
	if (vdp->reg[1] & 0x20)
		return vdp->status & 0x80;
	return 0;
}

void IRAM_ATTR flip_buffer(struct tms9918a *vdp) {
	vdp->visible_raster = vdp->active_raster;
	if (vdp->active_raster == vdp->rasterbuffer0) {
		vdp->active_raster = vdp->rasterbuffer1;
	} else {
		vdp->active_raster = vdp->rasterbuffer0;
	}
}
