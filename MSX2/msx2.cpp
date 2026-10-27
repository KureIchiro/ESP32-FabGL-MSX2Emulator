// ============================================================================
//  MSX2+ マシン本体  (構成は msx2.h を参照)
// ============================================================================
#ifdef ARDUINO
#pragma GCC optimize("O3")
#endif
#include "msx2.h"
#include "v9958.h"
#include "Z80.h"
#include <string.h>
#if MSX2_PROFILE
#include <stdarg.h>
#include <stdio.h>
#endif

static int cpuCyclesPerLine = 228;
#define CPU_CYCLES_PER_LINE cpuCyclesPerLine

extern "C" {
  Z80 msx2_cpu;
  uint8_t *op_map[8];
  uint8_t *msx2_rd_map[8];
  uint8_t *msx2_wr_map[8];
  byte RdZ80Slow(zword a);
  void WrZ80Slow(zword a, byte v);
  byte RdZ80(zword a);
  void WrZ80(zword a, byte v);
  byte InZ80(zword port);
  void OutZ80(zword port, byte v);
  void PatchZ80(Z80 *R);
}

static MSX2_DRAM uint8_t emptyPage[0x2000];
static uint8_t *pageRd[4][4][8];
static uint8_t *pageWr[4][4][8];
static bool expanded[4] = { true, false, false, true };
static uint8_t psr;
static uint8_t ssr[4];

static uint8_t *diskRom;
static bool     diskVisible;

static uint8_t *ramSeg[MSX2_MAPPER_SEGMENTS];
static uint8_t  mapReg[4];

struct Cart {
  uint8_t   *rom;
  uint32_t   mask8k;
  Msx2Mapper mapper;
  uint8_t    bank[4];
  bool       sccActive;
};
static Cart cart[3];
static uint8_t sccWave[4][32];
static bool    sccReadable;

struct FmPac {
  uint8_t *rom;
  uint8_t  bankMask;
  uint8_t *sram;
  uint8_t  enable;
  uint8_t  bank;
  uint8_t  r1ffe, r1fff;
  bool     sramOn;
};
static FmPac fmpac;
static bool  fmpacVisible;
static volatile uint32_t fmpacSramWrites;

static uint8_t *kanjiFont;
static uint32_t kanjiSize;
static uint32_t kanjiAddr[2];
static uint8_t  f4reg;
static bool     f4Inverted;
static uint8_t  ppiC;
static uint8_t  psgAddr, psgReg[16];
static bool     fmEnabled;

volatile uint8_t msx2_key_matrix[16];
volatile uint8_t msx2_joy[2] = { 0x7F, 0x7F };

volatile int32_t msx2_mouse_x, msx2_mouse_y;
volatile uint8_t msx2_mouse_buttons;
static int     mousePort;
static int     mousePhase = 3;
static int8_t  mouseRelX, mouseRelY;
static int32_t mouseLastX, mouseLastY;
void msx2SetMousePort(int port) { mousePort = (port == 1 || port == 2) ? port : 0; }

static uint32_t mouseStrobeAt;
static bool     mouseStrobed;
static const uint32_t MOUSE_IDLE_LINES = 262 * 4;

static void mouseStrobe() {
  mouseStrobeAt = msx2LineClock();
  mouseStrobed = true;
  if (mousePhase == 3) {
    int32_t x = msx2_mouse_x, y = msx2_mouse_y;
    int dx = -(x - mouseLastX), dy = y - mouseLastY;
    if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
    if (dy > 127) dy = 127; else if (dy < -127) dy = -127;
    mouseRelX = (int8_t)dx; mouseRelY = (int8_t)dy;
    mouseLastX -= dx;
    mouseLastY += dy;
  }
  mousePhase = (mousePhase + 1) & 3;
}

static uint8_t mouseRead(uint8_t joy) {
  uint8_t n;
  switch (mousePhase) {
    case 0:  n = (uint8_t)(mouseRelX >> 4); break;
    case 1:  n = (uint8_t)mouseRelX; break;
    case 2:  n = (uint8_t)(mouseRelY >> 4); break;
    default: n = (uint8_t)mouseRelY; break;
  }
  if (!mouseStrobed || msx2LineClock() - mouseStrobeAt > MOUSE_IDLE_LINES) n = 0x0F;
  uint8_t b = msx2_mouse_buttons;
  return (uint8_t)((joy & 0xC0) | ((b & 1) ? 0 : 0x10) | ((b & 2) ? 0 : 0x20) | (n & 0x0F));
}

static void updateMemoryMap();

#if MSX2_PROFILE
enum { PN_BANK, PN_MAP, PN_IRQ, PN_HALT, PN_COUNT };
static uint32_t profN[PN_COUNT];
static uint32_t profMapCyc, profVioCyc;
#define PROF_COUNT(i) (profN[i]++)
#else
#define PROF_COUNT(i) ((void)0)
#endif

static uint8_t rtcAddr, rtcMode, rtcRegs[4][13];
static int     rtcFrames;
static const uint8_t kRtcMask0[13] = { 15, 7, 15, 7, 15, 3, 7, 15, 3, 15, 1, 15, 15 };

static void rtcInit() {
  memset(rtcRegs, 0, sizeof(rtcRegs));
  uint8_t *t = rtcRegs[0];
  t[7] = 1;
  t[9] = 1;
  t[11] = 46 % 10; t[12] = 46 / 10;
  t[6] = 4;
  rtcRegs[1][10] = 1;
  rtcMode = 0x08;
}

static void rtcTickSecond() {
  static const uint8_t dpm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  uint8_t *t = rtcRegs[0];
  int sec = t[0] + t[1] * 10, min = t[2] + t[3] * 10, hour = t[4] + t[5] * 10;
  int day = t[7] + t[8] * 10, mon = t[9] + t[10] * 10, year = t[11] + t[12] * 10;
  if (++sec >= 60) {
    sec = 0;
    if (++min >= 60) {
      min = 0;
      if (++hour >= 24) {
        hour = 0;
        t[6] = (t[6] + 1) % 7;
        int m = (mon >= 1 && mon <= 12) ? mon - 1 : 0;
        int dim = dpm[m] + ((m == 1 && (year % 4) == 0) ? 1 : 0);
        if (++day > dim) {
          day = 1;
          if (++mon > 12) { mon = 1; year = (year + 1) % 100; }
        }
      }
    }
  }
  t[0] = sec % 10; t[1] = sec / 10; t[2] = min % 10; t[3] = min / 10;
  t[4] = hour % 10; t[5] = hour / 10; t[7] = day % 10; t[8] = day / 10;
  t[9] = mon % 10; t[10] = mon / 10; t[11] = year % 10; t[12] = year / 10;
}

static uint8_t rtcRead() {
  int a = rtcAddr & 15;
  if (a == 13) return 0xF0 | rtcMode;
  if (a >= 13) return 0xFF;
  int blk = rtcMode & 3;
  uint8_t v = rtcRegs[blk][a];
  if (blk == 0) v &= kRtcMask0[a];
  return 0xF0 | (v & 15);
}

static void rtcWrite(uint8_t v) {
  int a = rtcAddr & 15;
  v &= 15;
  if (a == 13) { rtcMode = v; return; }
  if (a >= 13) return;
  rtcRegs[rtcMode & 3][a] = v;
}

static void setRom(int prim, int sub, int firstPage, uint8_t *rom, uint32_t size) {
  for (uint32_t off = 0; off + 0x2000 <= size && firstPage + (int)(off >> 13) < 8; off += 0x2000) {
    pageRd[prim][sub][firstPage + (off >> 13)] = rom + off;
  }
}

void msx2SetRom(int prim, int sub, uint16_t addr, uint8_t *rom, uint32_t size) {
  setRom(prim & 3, sub & 3, addr >> 13, rom, size);
}
void msx2SetBios(uint8_t *bios)                 { setRom(0, 0, 0, bios, 0x8000); }
void msx2SetSubRom(uint8_t *sub)                { setRom(3, 1, 0, sub, 0x4000); }
void msx2SetKanjiBasic(uint8_t *rom, uint32_t s){ setRom(3, 1, 2, rom, s > 0x8000 ? 0x8000 : s); }
void msx2SetMusicRom(uint8_t *fm, int prim, int sub) { setRom(prim & 3, sub & 3, 2, fm, 0x4000); fmEnabled = (fm != nullptr); }
void msx2SetSlotExpanded(int prim, bool e)      { expanded[prim & 3] = e; }
void msx2SetF4Inverted(bool inv)                { f4Inverted = inv; }
void msx2SetDiskRom(uint8_t *disk)              { diskRom = disk; setRom(3, 2, 2, disk, 0x4000); }
void msx2SetKanjiFont(uint8_t *font, uint32_t s){ kanjiFont = font; kanjiSize = s; }
void msx2SetRamSegment(int seg, uint8_t *mem)   { if (seg >= 0 && seg < MSX2_MAPPER_SEGMENTS) ramSeg[seg] = mem; }

static void cartUpdatePages(int s) {
  Cart &c = cart[s];
  for (int i = 0; i < 4; i++) {
    pageRd[s][0][2 + i] = c.rom ? c.rom + ((uint32_t)(c.bank[i] & c.mask8k) << 13) : nullptr;
  }
}

void msx2SetCartridge(int s, uint8_t *rom, uint32_t size, Msx2Mapper mapper) {
  if (s != 1 && s != 2) return;
  Cart &c = cart[s];
  memset(&c, 0, sizeof(c));
  c.rom = rom;
  c.mapper = mapper;
  uint32_t banks = size >> 13;
  c.mask8k = banks ? banks - 1 : 0;
  switch (mapper) {
    case MSX2_MAP_ASCII8:  c.bank[0] = c.bank[1] = c.bank[2] = c.bank[3] = 0; break;
    case MSX2_MAP_ASCII16: c.bank[0] = 0; c.bank[1] = 1; c.bank[2] = 0; c.bank[3] = 1; break;
    case MSX2_MAP_RTYPE:   c.bank[0] = 46; c.bank[1] = 47; c.bank[2] = 0; c.bank[3] = 1; break;
    default:               c.bank[0] = 0; c.bank[1] = 1; c.bank[2] = 2; c.bank[3] = 3; break;
  }
  cartUpdatePages(s);
}

static void mapperUpdatePages() {
  for (int i = 0; i < 4; i++) {
    uint8_t *seg = ramSeg[mapReg[i] & (MSX2_MAPPER_SEGMENTS - 1)];
    for (int k = 0; k < 2; k++) {
      uint8_t *p = seg ? seg + k * 0x2000 : nullptr;
      pageRd[3][0][2 * i + k] = p;
      pageWr[3][0][2 * i + k] = p;
    }
  }
}

static void updateMemoryMap() {
#if MSX2_PROFILE
  uint32_t t0 = msx2ProfClock();
  PROF_COUNT(PN_MAP);
#endif
  for (int p = 0; p < 8; p++) {
    int sh = (p >> 1) * 2;
    int prim = (psr >> sh) & 3;
    int sub = expanded[prim] ? ((ssr[prim] >> sh) & 3) : 0;
    uint8_t *rd = pageRd[prim][sub][p];
    op_map[p] = msx2_rd_map[p] = rd ? rd : emptyPage;
    msx2_wr_map[p] = pageWr[prim][sub][p];
  }
  if (expanded[psr >> 6]) msx2_rd_map[7] = nullptr;
  diskVisible = diskRom && ((psr >> 2) & 3) == 3 && ((ssr[3] >> 2) & 3) == 2;
  if (diskVisible) msx2_rd_map[3] = nullptr;
  fmpacVisible = fmpac.rom && ((psr >> 2) & 3) == 2;
  if (fmpacVisible) msx2_rd_map[3] = nullptr;
  int p2 = (psr >> 4) & 3;
  sccReadable = (p2 == 1 || p2 == 2) && cart[p2].sccActive;
  if (sccReadable) msx2_rd_map[4] = nullptr;
#if MSX2_PROFILE
  profMapCyc += msx2ProfClock() - t0;
#endif
}

static uint8_t MSX2_FAST readSpecial(uint16_t a) {
  if (a == 0xFFFF) {
    int prim = psr >> 6;
    if (expanded[prim]) return (uint8_t)~ssr[prim];
  }
  if (diskVisible && a >= 0x7FF8 && a <= 0x7FFF) return msx2FdcRead(a & 7);
  if (fmpacVisible && (a == 0x7FF6 || a == 0x7FF7)) return (a == 0x7FF6) ? fmpac.enable : fmpac.bank;
  if (sccReadable && a >= 0x9800 && a <= 0x9FFF) {
    Cart &c = cart[(psr >> 4) & 3];
    if (c.mapper != MSX2_MAP_KONAMI || a <= 0x98FF) {
      uint8_t r = a & 0xFF;
      return r < 0x80 ? sccWave[r >> 5][r & 0x1F] : 0xFF;
    }
  }
  return op_map[a >> 13][a & 0x1FFF];
}

static void sccWrite(uint16_t a, uint8_t v) {
  uint8_t r = a & 0xFF;
  if (r < 0x80) sccWave[r >> 5][r & 0x1F] = v;
  msx2SccWrite(a, v);
}

static inline uint8_t readByte(uint16_t a) {
  const uint8_t *p = msx2_rd_map[a >> 13];
  if (MSX2_LIKELY(p != nullptr)) return p[a & 0x1FFF];
  return readSpecial(a);
}

static void cartWrite(int s, uint16_t a, uint8_t v) {
  Cart &c = cart[s];
  if ((!c.rom && c.mapper != MSX2_MAP_SCC) || a < 0x4000 || a >= 0xC000) return;
  switch (c.mapper) {
    case MSX2_MAP_KONAMI:
      if (c.sccActive && a >= 0x9800 && a <= 0x98FF) { sccWrite(a, v); return; }
      if (a >= 0x9000 && a <= 0x97FF) c.sccActive = ((v & 0x3F) == 0x3F);
      c.bank[(a - 0x4000) >> 13] = v;
      break;
    case MSX2_MAP_KONAMI_SCC:
      if (a >= 0x9800 && a <= 0x9FFF) { if (c.sccActive) sccWrite(0x9800 | (a & 0xFF), v); return; }
      if ((a & 0x1800) != 0x1000) return;
      if (a >= 0x9000 && a <= 0x97FF) c.sccActive = ((v & 0x3F) == 0x3F);
      c.bank[(a - 0x4000) >> 13] = v;
      break;
    case MSX2_MAP_SCC:
      if (a >= 0x9800 && a <= 0x9FFF) { if (c.sccActive) sccWrite(0x9800 | (a & 0xFF), v); return; }
      if (a >= 0x9000 && a <= 0x97FF) {
        bool on = ((v & 0x3F) == 0x3F);
        if (on == c.sccActive) return;
        c.sccActive = on;
        updateMemoryMap();
      }
      return;
    case MSX2_MAP_ASCII8:
      if (a >= 0x6000 && a < 0x8000) c.bank[(a >> 11) & 3] = v;
      else return;
      break;
    case MSX2_MAP_ASCII16:
      if (a >= 0x6000 && a < 0x6800)      { c.bank[0] = v * 2; c.bank[1] = v * 2 + 1; }
      else if (a >= 0x7000 && a < 0x7800) { c.bank[2] = v * 2; c.bank[3] = v * 2 + 1; }
      else return;
      break;
    case MSX2_MAP_RTYPE:
      if (a >= 0x7000 && a < 0x8000) { uint8_t b = (v % 24) * 2; c.bank[2] = b; c.bank[3] = b + 1; }
      else return;
      break;
    default:
      return;
  }
  PROF_COUNT(PN_BANK);
  cartUpdatePages(s);
  updateMemoryMap();
}

static void fmpacUpdatePages() {
  if (!fmpac.rom) return;
  uint8_t *b = fmpac.rom + ((uint32_t)(fmpac.bank & fmpac.bankMask) << 14);
  pageRd[2][0][2] = fmpac.sramOn ? fmpac.sram : b;
  pageRd[2][0][3] = fmpac.sramOn ? emptyPage : b + 0x2000;
}

static void fmpacCheckSram() {
  bool on = fmpac.sram && fmpac.r1ffe == 0x4D && fmpac.r1fff == 0x69;
  if (on == fmpac.sramOn) return;
  fmpac.sramOn = on;
  if (on) { fmpac.sram[0x1FFE] = 0x4D; fmpac.sram[0x1FFF] = 0x69; }
  PROF_COUNT(PN_BANK);
  fmpacUpdatePages();
  updateMemoryMap();
}

static void fmpacWrite(uint16_t a, uint8_t v) {
  if (a < 0x4000 || a >= 0x8000) return;
  uint16_t o = a & 0x3FFF;
  switch (o) {
    case 0x1FFE: case 0x1FFF:
      if (!(fmpac.enable & 0x10)) {
        if (o == 0x1FFE) fmpac.r1ffe = v; else fmpac.r1fff = v;
        fmpacCheckSram();
      }
      return;
    case 0x3FF4: case 0x3FF5:
      msx2FmWrite(o & 1, v);
      return;
    case 0x3FF6:
      fmpac.enable = v & 0x11;
      if (fmpac.enable & 0x10) { fmpac.r1ffe = fmpac.r1fff = 0; fmpacCheckSram(); }
      return;
    case 0x3FF7:
      if ((v & 3) != fmpac.bank) {
        fmpac.bank = v & 3;
        PROF_COUNT(PN_BANK);
        fmpacUpdatePages();
        updateMemoryMap();
      }
      return;
    default:
      if (fmpac.sramOn && o < 0x1FFE && fmpac.sram[o] != v) { fmpac.sram[o] = v; fmpacSramWrites++; }
      return;
  }
}
uint32_t msx2FmPacSramWrites() { return fmpacSramWrites; }

void msx2SetFmPac(uint8_t *rom, uint32_t size, uint8_t *sram8k) {
  fmpac.rom = rom;
  fmpac.sram = sram8k;
  uint32_t banks = size >> 14;
  fmpac.bankMask = (uint8_t)(banks > 1 ? banks - 1 : 0);
}

static void MSX2_FAST writeSpecial(uint16_t a, uint8_t v) {
  int page = a >> 13;
  int sh = (page >> 1) * 2;
  int prim = (psr >> sh) & 3;
  if (a == 0xFFFF && expanded[prim]) {
    ssr[prim] = v;
    updateMemoryMap();
    return;
  }
  uint8_t *p = msx2_wr_map[page];
  if (p) { p[a & 0x1FFF] = v; return; }
  if (prim == 2 && fmpac.rom) { fmpacWrite(a, v); return; }
  if (prim == 1 || prim == 2) { cartWrite(prim, a, v); return; }
  if (diskVisible && a >= 0x7FF8 && a <= 0x7FFF) msx2FdcWrite(a & 7, v);
}

static inline void writeByte(uint16_t a, uint8_t v) {
  uint8_t *p = msx2_wr_map[a >> 13];
  if (MSX2_LIKELY(p != nullptr && a != 0xFFFF)) { p[a & 0x1FFF] = v; return; }
  writeSpecial(a, v);
}

static uint8_t MSX2_FAST readIO(uint8_t p) {
  switch (p) {
    case 0x98: case 0x99: case 0x9A: case 0x9B: {
#if MSX2_PROFILE
      uint32_t t0 = msx2ProfClock();
      uint8_t v = vdpReadPort(p & 3);
      profVioCyc += msx2ProfClock() - t0;
#else
      uint8_t v = vdpReadPort(p & 3);
#endif
      if (MSX2_UNLIKELY(msx2_cpu.IRequest == INT_IRQ) && !vdpIrq()) msx2_cpu.IRequest = INT_NONE;
      return v;
    }
    case 0xA2:
      if (psgAddr == 14) {
        int port = (psgReg[15] >> 6) & 1;
        if (mousePort == port + 1) return mouseRead(msx2_joy[port]);
        return msx2_joy[port];
      }
      return psgReg[psgAddr];
    case 0xA8: return psr;
    case 0xA9: return msx2_key_matrix[ppiC & 15];
    case 0xAA: return ppiC;
    case 0xB5: return rtcRead();
    case 0xD9: case 0xDB: {
      int n = (p == 0xDB);
      uint32_t a = kanjiAddr[n] + (n ? 0x20000 : 0);
      uint8_t v = (kanjiFont && a < kanjiSize) ? kanjiFont[a] : 0xFF;
      kanjiAddr[n] = (kanjiAddr[n] & ~0x1F) | ((kanjiAddr[n] + 1) & 0x1F);
      return v;
    }
    case 0xF4: return f4reg;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
      return mapReg[p & 3] | (uint8_t)~(MSX2_MAPPER_SEGMENTS - 1);
    default:
      return 0xFF;
  }
}

static void MSX2_FAST writeIO(uint8_t p, uint8_t v) {
  switch (p) {
    case 0x98: case 0x99: case 0x9A: case 0x9B:
#if MSX2_PROFILE
    {
      uint32_t t0 = msx2ProfClock();
      vdpWritePort(p & 3, v);
      profVioCyc += msx2ProfClock() - t0;
    }
#else
      vdpWritePort(p & 3, v);
#endif
      if (MSX2_UNLIKELY(msx2_cpu.IRequest == INT_IRQ) && !vdpIrq()) msx2_cpu.IRequest = INT_NONE;
      return;
    case 0xA0: psgAddr = v & 15; return;
    case 0xA1:
      if (psgAddr == 15 && mousePort && ((psgReg[15] ^ v) & (mousePort == 1 ? 0x10 : 0x20))) mouseStrobe();
      psgReg[psgAddr] = v;
      msx2PsgWrite(psgAddr, v);
      return;
    case 0xA8: psr = v; updateMemoryMap(); return;
    case 0xAA: ppiC = v; return;
    case 0xAB:
      if (!(v & 0x80)) {
        uint8_t bit = 1 << ((v >> 1) & 7);
        ppiC = (v & 1) ? (ppiC | bit) : (ppiC & ~bit);
      }
      return;
    case 0x7C: case 0x7D:
      if (fmEnabled || (fmpac.rom && (fmpac.enable & 1))) msx2FmWrite(p & 1, v);
      return;
    case 0xB4: rtcAddr = v & 15; return;
    case 0xB5: rtcWrite(v); return;
    case 0xD8: kanjiAddr[0] = (kanjiAddr[0] & 0x1F800) | ((v & 0x3F) << 5); return;
    case 0xD9: kanjiAddr[0] = (kanjiAddr[0] & 0x007E0) | ((uint32_t)(v & 0x3F) << 11); return;
    case 0xDA: kanjiAddr[1] = (kanjiAddr[1] & 0x1F800) | ((v & 0x3F) << 5); return;
    case 0xDB: kanjiAddr[1] = (kanjiAddr[1] & 0x007E0) | ((uint32_t)(v & 0x3F) << 11); return;
    case 0xF4:
      f4reg = f4Inverted ? (v | 0x7F) : ((f4reg & 0x20) | (v & 0xA0));
      return;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
      mapReg[p & 3] = v;
      mapperUpdatePages();
      updateMemoryMap();
      return;
    default:
      return;
  }
}

extern "C" {
byte MSX2_FAST RdZ80(zword a)         { return readByte(a); }
void MSX2_FAST WrZ80(zword a, byte v) { writeByte(a, v); }
byte MSX2_FAST RdZ80Slow(zword a)         { return readSpecial(a); }
void MSX2_FAST WrZ80Slow(zword a, byte v) { writeSpecial(a, v); }
byte MSX2_FAST InZ80(zword port)      { return readIO((uint8_t)port); }
void MSX2_FAST OutZ80(zword port, byte v) { writeIO((uint8_t)port, v); }
void PatchZ80(Z80 *R) { (void)R; }
}

int vdpLineCycle() {
  int c = CPU_CYCLES_PER_LINE - msx2_cpu.ICount;
  if (c < 0) c = 0;
  return c * MSX2_CYCLES_PER_LINE / CPU_CYCLES_PER_LINE;
}

void msx2SetCpuCycles(int n) {
  if (n < 100) n = 100;
  cpuCyclesPerLine = n;
  msx2_cpu.IPeriod = n;
}
int msx2GetCpuCycles() { return cpuCyclesPerLine; }

#if MSX2_PROFILE
enum { PT_RUN, PT_VDP, PT_SPR, PT_OUT, PT_Z80, PT_VIO, PT_CMD, PT_MAP, PT_COUNT };
static struct {
  uint32_t frames;
  uint64_t sum[PT_COUNT];
  uint32_t max[PT_COUNT];
  uint32_t worst[PT_COUNT];
  uint64_t opCycles[16];
  uint32_t opCount[16];
  uint32_t n[PN_COUNT];
  uint32_t vramW, vramR, data, lines, sprLines, sprVis;
  uint32_t modeLines[32];
  uint32_t hsLines, adjLines;
  uint64_t dots;
  uint32_t cmdMax;
  uint16_t cmdMaxNx, cmdMaxNy;
  uint8_t  cmdMaxCmd;
} profW;

static void profClear() {
  memset(&profW, 0, sizeof(profW));
  memset(profN, 0, sizeof(profN));
  profMapCyc = profVioCyc = 0;
  memset(vdpProf(), 0, sizeof(VdpProf));
}

static MSX2_PROF_COLD void profEndFrame(uint32_t run, uint32_t vdp, uint32_t z80) {
  VdpProf *v = vdpProf();
  uint32_t cmd = 0;
  for (int i = 0; i < 16; i++) {
    cmd += v->cmdCycles[i];
    profW.opCycles[i] += v->cmdCycles[i];
    profW.opCount[i] += v->cmdCount[i];
  }
  const uint32_t t[PT_COUNT] = { run, vdp, v->sprCycles, v->outCycles, z80, profVioCyc, cmd, profMapCyc };
  for (int i = 0; i < PT_COUNT; i++) {
    profW.sum[i] += t[i];
    if (t[i] > profW.max[i]) profW.max[i] = t[i];
  }
  if (run >= profW.worst[PT_RUN]) memcpy(profW.worst, t, sizeof(t));
  if (v->cmdMax > profW.cmdMax) {
    profW.cmdMax = v->cmdMax;
    profW.cmdMaxCmd = v->cmdMaxCmd;
    profW.cmdMaxNx = v->cmdMaxNx;
    profW.cmdMaxNy = v->cmdMaxNy;
  }
  for (int i = 0; i < PN_COUNT; i++) profW.n[i] += profN[i];
  profW.vramW += v->vramWrites;
  profW.vramR += v->vramReads;
  profW.data += v->cmdData;
  profW.lines += v->lines;
  profW.sprLines += v->sprLines;
  profW.sprVis += v->sprVis;
  for (int i = 0; i < 32; i++) profW.modeLines[i] += v->modeLines[i];
  profW.hsLines += v->hsLines;
  profW.adjLines += v->adjLines;
  profW.dots += v->cmdDots;
  profW.frames++;
  memset(v, 0, sizeof(*v));
  memset(profN, 0, sizeof(profN));
  profMapCyc = profVioCyc = 0;
}

static void profAppend(char *buf, int size, int *pos, const char *fmt, ...) {
  if (*pos >= size - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(buf + *pos, size - *pos, fmt, ap);
  va_end(ap);
  if (r > 0) *pos = (*pos + r < size - 1) ? *pos + r : size - 1;
}

static const char *profRate(char *s, uint64_t n, uint64_t frames) {
  uint64_t c = n * 100 / frames;
  if (c < 1000)       snprintf(s, 24, "%u.%02u", (unsigned)(c / 100), (unsigned)(c % 100));
  else if (c < 10000) snprintf(s, 24, "%u.%u", (unsigned)(c / 100), (unsigned)(c / 10 % 10));
  else                snprintf(s, 24, "%u", (unsigned)(c / 100));
  return s;
}

int msx2ProfReport(char *buf, int size, uint32_t clockPerUs) {
  if (!buf || size <= 0) return 0;
  buf[0] = 0;
  uint64_t cpu = clockPerUs ? clockPerUs : 1;
  uint64_t f = profW.frames ? profW.frames : 1;
  unsigned avg[PT_COUNT], mx[PT_COUNT], worst[PT_COUNT];
  for (int i = 0; i < PT_COUNT; i++) {
    avg[i] = (unsigned)(profW.sum[i] / (f * cpu));
    mx[i] = (unsigned)(profW.max[i] / cpu);
    worst[i] = (unsigned)(profW.worst[i] / cpu);
  }
  static const char *const opName[16] = { "STOP", "op1", "op2", "op3", "POINT", "PSET", "SRCH", "LINE",
                                          "LMMV", "LMMM", "LMCM", "LMMC", "HMMV", "HMMM", "YMMM", "HMMC" };
  int pos = 0;
  profAppend(buf, size, &pos,
             "PROF us avg/max (%uf): run %u/%u vdp %u/%u (spr %u/%u out %u/%u) z80 %u/%u"
             " (vio %u/%u cmd %u/%u map %u/%u) | worst: vdp %u spr %u z80 %u vio %u cmd %u | longest cmd: ",
             (unsigned)profW.frames, avg[PT_RUN], mx[PT_RUN], avg[PT_VDP], mx[PT_VDP], avg[PT_SPR], mx[PT_SPR],
             avg[PT_OUT], mx[PT_OUT], avg[PT_Z80], mx[PT_Z80], avg[PT_VIO], mx[PT_VIO], avg[PT_CMD], mx[PT_CMD],
             avg[PT_MAP], mx[PT_MAP],
             worst[PT_VDP], worst[PT_SPR], worst[PT_Z80], worst[PT_VIO], worst[PT_CMD]);
  if (profW.cmdMax)
    profAppend(buf, size, &pos, "%s/%X %ux%u %uus\n", opName[profW.cmdMaxCmd >> 4],
               (unsigned)(profW.cmdMaxCmd & 15), (unsigned)profW.cmdMaxNx, (unsigned)profW.cmdMaxNy,
               (unsigned)(profW.cmdMax / cpu));
  else
    profAppend(buf, size, &pos, "-\n");
  char a[24], b[24], c[24], d[24], e[24], g[24], h[24], k2[24], l[24];
  profAppend(buf, size, &pos, "PROF n/frame: 98h wr %s rd %s int %s halt %s bank %s map %s ln %s spl %s spv %s | cmd n(us):",
             profRate(a, profW.vramW, f), profRate(b, profW.vramR, f), profRate(c, profW.n[PN_IRQ], f),
             profRate(g, profW.n[PN_HALT], f), profRate(d, profW.n[PN_BANK], f), profRate(e, profW.n[PN_MAP], f),
             profRate(l, profW.lines, f), profRate(h, profW.sprLines, f), profRate(k2, profW.sprVis, f));
  static const char *const names[8] = { "HMMM", "LMMM", "HMMV", "LMMV", "YMMM", "LINE", "HMMC/LMMC", "other" };
  static const uint16_t ops[8] = { 1 << 0xD, 1 << 0x9, 1 << 0xC, 1 << 0x8, 1 << 0xE, 1 << 0x7,
                                   (1 << 0xF) | (1 << 0xB), 0x007F | (1 << 0xA) };
  for (int k = 0; k < 8; k++) {
    uint64_t n = 0, cyc = 0;
    for (int op = 0; op < 16; op++)
      if (ops[k] & (1 << op)) { n += profW.opCount[op]; cyc += profW.opCycles[op]; }
    profAppend(buf, size, &pos, " %s %s(%u)", names[k], profRate(a, n, f), (unsigned)(cyc / (f * cpu)));
  }
  profAppend(buf, size, &pos, " | dots %u data %s\n", (unsigned)(profW.dots / f), profRate(a, profW.data, f));
  profAppend(buf, size, &pos, "PROF lines/frame by mode:");
  for (int m = 0; m < 32; m++) {
    if (!profW.modeLines[m]) continue;
    const char *nm = nullptr;
    switch (m) {
      case 0x00: nm = "G1(SC1)"; break;  case 0x01: nm = "T1(SC0w40)"; break; case 0x02: nm = "MC(SC3)"; break;
      case 0x04: nm = "G2(SC2)"; break;  case 0x08: nm = "G3(SC4)"; break;    case 0x09: nm = "T2(SC0w80)"; break;
      case 0x0C: nm = "G4(SC5)"; break;  case 0x10: nm = "G5(SC6)"; break;    case 0x14: nm = "G6(SC7)"; break;
      case 0x1C: nm = "G7(SC8)"; break;  default: break;
    }
    if (nm) profAppend(buf, size, &pos, " %s %s", nm, profRate(a, profW.modeLines[m], f));
    else    profAppend(buf, size, &pos, " m%02X %s", m, profRate(a, profW.modeLines[m], f));
  }
  profAppend(buf, size, &pos, " | hscroll(R26/27) %s adjust(R18) %s\n", profRate(a, profW.hsLines, f),
             profRate(b, profW.adjLines, f));
  memset(&profW, 0, sizeof(profW));
  return pos;
}
#endif

void msx2Reset() {
  memset(emptyPage, 0xFF, sizeof(emptyPage));
  for (int i = 0; i < 16; i++) msx2_key_matrix[i] = 0xFF;
  psr = 0;
  memset(ssr, 0, sizeof(ssr));
  mapReg[0] = 3; mapReg[1] = 2; mapReg[2] = 1; mapReg[3] = 0;
  mapperUpdatePages();
  for (int s = 1; s <= 2; s++) { cart[s].sccActive = false; if (cart[s].rom) cartUpdatePages(s); }
  memset(sccWave, 0, sizeof(sccWave));
  mousePhase = 3;
  mouseStrobed = false;
  mouseLastX = msx2_mouse_x; mouseLastY = msx2_mouse_y;
  fmpac.enable = 0; fmpac.bank = 0; fmpac.r1ffe = fmpac.r1fff = 0; fmpac.sramOn = false;
  fmpacUpdatePages();
  kanjiAddr[0] = kanjiAddr[1] = 0;
  f4reg = f4Inverted ? 0xFF : 0x00;
  ppiC = 0;
  psgAddr = 0;
  memset(psgReg, 0, sizeof(psgReg));
  rtcInit();
  rtcFrames = 0;
  updateMemoryMap();

  vdpReset();
  memset(&msx2_cpu, 0, sizeof(msx2_cpu));
  ResetZ80(&msx2_cpu);
  msx2_cpu.IPeriod = CPU_CYCLES_PER_LINE;
  msx2_cpu.IAutoReset = 1;
#if MSX2_PROFILE
  profClear();
#endif
}

static uint32_t lineClock;
uint32_t msx2LineClock() { return lineClock; }

void MSX2_FAST msx2RunFrame() {
#if MSX2_PROFILE
  uint32_t tFrame = msx2ProfClock(), t0 = tFrame, vdpCycles = 0, z80Cycles = 0;
#endif
  for (int line = 0; line < MSX2_LINES_PER_FRAME; line++) {
    lineClock++;
    vdpLine(line);
#if MSX2_PROFILE
    uint32_t t1 = msx2ProfClock();
    vdpCycles += t1 - t0;
#endif
    if (vdpIrq()) {
      msx2_cpu.IRequest = INT_IRQ;
      if (msx2_cpu.IFF & IFF_1) {
        IntZ80(&msx2_cpu, INT_IRQ);
        PROF_COUNT(PN_IRQ);
      }
    } else {
      msx2_cpu.IRequest = INT_NONE;
    }
#if MSX2_PROFILE
    bool pending = (msx2_cpu.IRequest == INT_IRQ);
    if (msx2_cpu.IFF & IFF_HALT) PROF_COUNT(PN_HALT);
#endif
    ExecZ80(&msx2_cpu, CPU_CYCLES_PER_LINE);
#if MSX2_PROFILE
    if (pending && msx2_cpu.IRequest != INT_IRQ) PROF_COUNT(PN_IRQ);
    t0 = msx2ProfClock();
    z80Cycles += t0 - t1;
#endif
  }
  if (++rtcFrames >= 60) { rtcFrames = 0; rtcTickSecond(); }
#if MSX2_PROFILE
  profEndFrame(msx2ProfClock() - tFrame, vdpCycles, z80Cycles);
#endif
}

uint8_t msx2Leds() {
    static bool kanaManaged = false;
    if (psgReg[15] & 0x80) kanaManaged = true;
    return (!(ppiC & 0x40) ? 1 : 0) | ((kanaManaged && !(psgReg[15] & 0x80)) ? 2 : 0);
}
uint8_t msx2PrimarySlots() { return psr; }
uint8_t msx2SubSlots(int prim) { return ssr[prim & 3]; }
