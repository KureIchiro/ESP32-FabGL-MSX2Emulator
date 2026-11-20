/* ============================================================================
 *  z80core.h - Z80 CPU core for the TTGO VGA32 emulator collection
 *
 *  Copyright (c) 2026 Kure Ichiro
 *  MIT License: Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 *  associated documentation files (the "Software"), to deal in the Software without restriction, including without
 *  limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
 *  Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *  The above copyright notice and this permission notice shall be included in all copies or substantial portions
 *  of the Software.
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *  THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *  CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 *  Written from the Z80 documentation (Zilog's user manual and the public descriptions of the undocumented
 *  behaviour: flags 5 and 3, MEMPTR, the undocumented opcodes). Made for small hosts (ESP32): one interpreter
 *  loop with the program counter and the cycle counter in local variables, memory through 8 KB page pointers
 *  (a NULL page goes to the read / write callbacks).
 *
 *  Use:
 *    z80core_init(&z); set z.rpage[] / z.wpage[] and the callbacks; z80core_reset(&z);
 *    left = z80core_run(&z, cycles);      runs until the cycles are used (returns the cycles left, usually <= 0)
 *    z.irq = 1 / 0                    the maskable interrupt line (level). With irq_autoclear 1 it is cleared
 *                                     when the CPU takes the interrupt (an interrupt held until taken)
 *    z80core_nmi(&z)                      a non-maskable interrupt
 *  Inside a callback z->icount holds the cycles left (the current instruction already counted); a callback may
 *  change it (wait states; 0 ends z80core_run after the instruction).
 *  The host must be little endian (ESP32, x86).
 * ============================================================================ */
#ifndef Z80CORE_H
#define Z80CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
  uint16_t w;
  struct { uint8_t l, h; } b;
} z80_pair_t;

typedef struct z80 z80_t;
struct z80 {
  z80_pair_t af, bc, de, hl, ix, iy, sp, pc, wz;
  uint16_t af2, bc2, de2, hl2;
  uint8_t i, r, r7, im;
  uint8_t iff1, iff2, halted, ei_delay;
  uint8_t irq, irq_autoclear, irq_vector, nmi;
  int32_t icount;
  uint8_t *rpage[8];
  uint8_t *wpage[8];
  uint8_t (*read)(z80_t *z, uint16_t addr);
  void (*write)(z80_t *z, uint16_t addr, uint8_t value);
  uint8_t (*in)(z80_t *z, uint16_t port);
  void (*out)(z80_t *z, uint16_t port, uint8_t value);
  void *user;
};

#define z80_state_size offsetof(z80_t, rpage)

void z80core_init(z80_t *z);
void z80core_reset(z80_t *z);
int  z80core_run(z80_t *z, int cycles);
void z80core_nmi(z80_t *z);
uint8_t z80core_get_r(const z80_t *z);
int  z80core_int(z80_t *z, uint8_t vector);
int  z80core_nmi_now(z80_t *z);
extern const uint8_t z80core_cyc_op[256], z80core_cyc_cb[256], z80core_cyc_ed[256];

#ifdef __cplusplus
}
#endif

#endif
