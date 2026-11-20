/* z80core_config.h - MSX2: the memory map of msx2.cpp for z80core
   Opcodes and operands come from op_map (8 KB pages). Data reads / writes use msx2_rd_map / msx2_wr_map; a NULL
   page (FDC, FM-PAC, cartridge bank switching, ...) and FFFFh (the expanded slot register) call RdZ80Slow /
   WrZ80Slow in msx2.cpp. The I/O sees the cycles left in the line (z->icount, vdpLineCycle). */
#ifndef Z80CORE_CONFIG_H
#define Z80CORE_CONFIG_H
#include <stdint.h>

extern uint8_t *op_map[];
extern uint8_t *msx2_rd_map[], *msx2_wr_map[];
uint8_t RdZ80Slow(uint16_t addr);
void WrZ80Slow(uint16_t addr, uint8_t value);
uint8_t InZ80(uint16_t port);
void OutZ80(uint16_t port, uint8_t value);

#define Z80CORE_OP(a) (op_map[(a) >> 13][(a) & 0x1FFF])
#define Z80CORE_RD(a) ({ const uint8_t *pr_ = msx2_rd_map[(a) >> 13]; uint8_t vr_; \
  if (__builtin_expect(pr_ != 0, 1)) vr_ = pr_[(a) & 0x1FFF]; else { Z80CORE_SYNC_OUT(); vr_ = RdZ80Slow(a); Z80CORE_SYNC_IN(); } vr_; })
#define Z80CORE_WR(a, v) do { uint8_t *pw_ = msx2_wr_map[(a) >> 13]; \
  if (__builtin_expect(pw_ != 0 && (a) != 0xFFFF, 1)) pw_[(a) & 0x1FFF] = (v); \
  else { Z80CORE_SYNC_OUT(); WrZ80Slow((a), (v)); Z80CORE_SYNC_IN(); } } while (0)
#define Z80CORE_IN(p) ({ Z80CORE_SYNC_OUT(); const uint8_t vi_ = InZ80(p); Z80CORE_SYNC_IN(); vi_; })
#define Z80CORE_OUT(p, v) do { Z80CORE_SYNC_OUT(); OutZ80((p), (v)); Z80CORE_SYNC_IN(); } while (0)
#define Z80CORE_M1_WAIT 1

#endif
