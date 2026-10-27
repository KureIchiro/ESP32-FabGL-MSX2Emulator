// ============================================================================
//  V9958 (MSX2+ VDP)  実装メモは v9958.h を参照
// ============================================================================
#ifdef ARDUINO
#pragma GCC optimize("O3")
#endif
#include "v9958.h"
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

enum {
  MODE_G1 = 0x00, MODE_T1 = 0x01, MODE_MC = 0x02, MODE_G2 = 0x04, MODE_G3 = 0x08,
  MODE_T2 = 0x09, MODE_G4 = 0x0C, MODE_G5 = 0x10, MODE_G6 = 0x14, MODE_G7 = 0x1C
};

static VdpState S;
static uint8_t *vram;
static uint8_t **outRows;
static uint8_t syncBits;
static bool    vrFlag;
static bool    fhMatch;
static bool    is9938;
static int     adjY;

static MSX2_DRAM uint8_t palNat[16];
static MSX2_DRAM uint8_t g7Nat[256];
static MSX2_DRAM uint8_t g7SprNat[16];

alignas(4) static MSX2_DRAM uint8_t lb[1024];
alignas(4) static MSX2_DRAM uint8_t lb2[512];

static inline int  curMode() { return ((S.reg[1] >> 4) & 1) | ((S.reg[1] >> 2) & 2) | ((S.reg[0] << 1) & 0x1C); }
static inline bool planarMode(int m) { return m == MODE_G6 || m == MODE_G7; }
static inline bool v9938Mode(int m) { return (m & 0x18) != 0; }
static inline uint32_t planar(uint32_t a) { return ((a >> 1) | ((a & 1) << 16)) & 0x1FFFF; }

static MSX2_DRAM uint8_t shA[0x280];
static MSX2_DRAM uint8_t shP[0x800];
static uint32_t shA0 = 0x100000, shALen = 0, shP0 = 0x100000;

static inline void shPoke(uint32_t a, uint8_t v) {
  if (a - shA0 < shALen) shA[a - shA0] = v;
  if (a - shP0 < 0x800u) shP[a - shP0] = v;
}
static void shSync(uint32_t lo, uint32_t hi) {
  uint32_t a1 = shA0 + shALen;
  if (lo < a1 && hi > shA0) {
    uint32_t s = lo > shA0 ? lo : shA0, e = hi < a1 ? hi : a1;
    memcpy(shA + (s - shA0), vram + s, e - s);
  }
  uint32_t p1 = shP0 + 0x800;
  if (lo < p1 && hi > shP0) {
    uint32_t s = lo > shP0 ? lo : shP0, e = hi < p1 ? hi : p1;
    memcpy(shP + (s - shP0), vram + s, e - s);
  }
}
static inline void shSetup(uint32_t a0, uint32_t alen, uint32_t p0) {
  if (MSX2_LIKELY(a0 == shA0 && alen == shALen && p0 == shP0)) return;
  shA0 = a0; shALen = alen; shP0 = p0;
  memcpy(shA, vram + a0, alen);
  memcpy(shP, vram + p0, 0x800);
}
static inline void shInvalidate() { shA0 = shP0 = 0x100000; shALen = 0; }

#ifndef ARDUINO
int vdpShadowMismatch() {
  int n = 0;
  if (shALen) for (uint32_t i = 0; i < shALen; i++) n += shA[i] != vram[shA0 + i];
  if (shP0 < 0x20000) for (uint32_t i = 0; i < 0x800; i++) n += shP[i] != vram[shP0 + i];
  return n;
}
#endif

#if MSX2_PROFILE
static VdpProf P;
VdpProf *vdpProf() { return &P; }
#endif

static inline uint8_t nat(int r2, int g2, int b2) {
  return syncBits | (uint8_t)((b2 << 4) | (g2 << 2) | r2);
}
static void updatePalNat(int i) {
  uint16_t p = S.palette[i];
  palNat[i] = nat(((p >> 8) & 7) >> 1, ((p >> 4) & 7) >> 1, (p & 7) >> 1);
}

static const uint16_t kDefaultPalette[16] = {
  0x000, 0x000, 0x161, 0x373, 0x117, 0x237, 0x511, 0x267,
  0x711, 0x733, 0x661, 0x664, 0x141, 0x625, 0x555, 0x777
};
static const uint16_t kG7SpritePalette[16] = {
  0x000, 0x002, 0x030, 0x032, 0x300, 0x302, 0x330, 0x332,
  0x472, 0x007, 0x070, 0x077, 0x700, 0x707, 0x770, 0x777
};

void vdpInit(uint8_t *vramBuf, uint8_t **rows, uint8_t sync) {
  vram = vramBuf;
  outRows = rows;
  syncBits = sync;
  for (int i = 0; i < 256; i++) {
    int g = (i >> 5) & 7, r = (i >> 2) & 7, b = i & 3;
    g7Nat[i] = nat(r >> 1, g >> 1, b);
  }
  for (int i = 0; i < 16; i++) {
    uint16_t p = kG7SpritePalette[i];
    g7SprNat[i] = nat(((p >> 4) & 7) >> 1, ((p >> 8) & 7) >> 1, (p & 7) >> 1);
  }
  vdpReset();
}

struct VdpCmd {
  bool active;
  int  op, lop;
  int  sx, sy, dx, dy, nx, ny;
  int  dix, diy;
  int  x, y, sxc, i, rows;
  bool blk;
  int  bnx, bleft, rowCost, credit;
  int  syncCyc;
  uint8_t bclr;
  uint8_t regW;
};
static VdpCmd C;

static int pendLine = -1, pendRow;
static const int kRenderCycle = MSX2_CYCLES_PER_LINE * 9 / 16;
static void vdpSyncSlow();
static inline void vdpSync() { if (pendLine >= 0 || C.blk) vdpSyncSlow(); }

void vdpReset() {
  memset(&S, 0, sizeof(S));
  memset(&C, 0, sizeof(C));
  pendLine = -1;
  shInvalidate();
  memcpy(S.palette, kDefaultPalette, sizeof(kDefaultPalette));
  for (int i = 0; i < 16; i++) updatePalNat(i);
  S.blinkCounter = 1;
  vrFlag = false;
  adjY = 0;
}

void vdpSetV9938(bool v9938) { is9938 = v9938; }
bool vdpIsV9938() { return is9938; }

static inline int adjustOffset(int nib) { return nib < 8 ? -nib : 16 - nib; }

VdpState *vdpState() { return &S; }
uint8_t  *vdpVram()  { return vram; }
int vdpDisplayMode() { return curMode(); }
int vdpDisplayLines() { return (S.reg[9] & 0x80) ? 212 : 192; }

bool vdpIrq() {
  return ((S.status[0] & 0x80) && (S.reg[1] & 0x20)) ||
         ((S.status[1] & 0x01) && (S.reg[0] & 0x10));
}

static int cm;
static int cmW;
static int cmS;
static int cmBp;
static int cmPl;
static int cmYM;
static uint8_t  cmM;
static uint32_t cmRep;

static void cmdSetMode() {
  switch (curMode()) {
    case MODE_G4: cm = 0; cmW = 256; break;
    case MODE_G5: cm = 1; cmW = 512; break;
    case MODE_G6: cm = 2; cmW = 512; break;
    default:      cm = 3; cmW = 256; break;
  }
  cmS   = cm == 1 ? 2 : (cm == 3 ? 0 : 1);
  cmBp  = 3 - cmS;
  cmPl  = cm >= 2;
  cmYM  = cmPl ? 511 : 1023;
  cmM   = (uint8_t)((1u << (1 << cmBp)) - 1);
  cmRep = cm == 1 ? 0x55555555u : (cm == 3 ? 0x01010101u : 0x11111111u);
}
template<int M> struct CmK {
  enum { YM = M < 2 ? 1023 : 511, RS = M < 2 ? 7 : 8, XM = (M == 1 || M == 2) ? 511 : 255, W = XM + 1,
         S = M == 1 ? 2 : (M == 3 ? 0 : 1), BP = 3 - S, PK = (1 << S) - 1, PL = M >= 2, MASK = (1 << (1 << BP)) - 1 };
};
template<int M> static inline uint32_t cmAddrT(int x, int y) {
  uint32_t la = ((uint32_t)(y & CmK<M>::YM) << CmK<M>::RS) | ((uint32_t)(x & CmK<M>::XM) >> CmK<M>::S);
  return CmK<M>::PL ? ((la >> 1) | ((la & 1) << 16)) : la;
}
template<int M> static inline int cmShiftT(int x) { return ((~x) & CmK<M>::PK) << CmK<M>::BP; }
template<int M> static inline uint8_t pgetT(int x, int y) {
  return (vram[cmAddrT<M>(x, y)] >> cmShiftT<M>(x)) & CmK<M>::MASK;
}
template<int M> static inline void psetT(int x, int y, uint8_t c, int lop) {
  const uint32_t m = CmK<M>::MASK;
  uint32_t a = cmAddrT<M>(x, y);
  int sh = cmShiftT<M>(x);
  c &= m;
  if ((lop & 8) && c == 0) return;
  uint32_t b = vram[a], d = (b >> sh) & m, r;
  switch (lop & 7) {
    case 0:  r = c; break;
    case 1:  r = c & d; break;
    case 2:  r = c | d; break;
    case 3:  r = c ^ d; break;
    case 4:  r = (~c) & m; break;
    default: return;
  }
  vram[a] = (uint8_t)((b & ~(m << sh)) | (r << sh));
  shPoke(a, vram[a]);
}
static inline int cmPixPerByte() { return 1 << cmS; }
static inline uint8_t cmMask() { return cmM; }
static inline uint8_t pget(int x, int y) {
  switch (cm) {
    case 0:  return pgetT<0>(x, y);
    case 1:  return pgetT<1>(x, y);
    case 2:  return pgetT<2>(x, y);
    default: return pgetT<3>(x, y);
  }
}
static inline void pset(int x, int y, uint8_t c, int lop) {
  switch (cm) {
    case 0:  psetT<0>(x, y, c, lop); break;
    case 1:  psetT<1>(x, y, c, lop); break;
    case 2:  psetT<2>(x, y, c, lop); break;
    default: psetT<3>(x, y, c, lop); break;
  }
}

#if defined(__GNUC__)
typedef uint32_t __attribute__((may_alias)) cmU32;
#define CM_NOINLINE __attribute__((noinline))
#else
typedef uint32_t cmU32;
#define CM_NOINLINE
#endif
static inline uint32_t cmOff(uint32_t a) { return (a >> cmPl) | ((a & (uint32_t)cmPl) << 16); }
static inline int cmMin(int a, int b) { return a < b ? a : b; }
static inline uint32_t cmRow(int y) { return (uint32_t)(y & cmYM) << 7; }

static inline int cmAvailP(int x, int dix) { return x >= cmW ? 0 : (dix > 0 ? cmW - x : x + 1); }
static inline int cmAvailB(int x, int dix) {
  if (x >= cmW) return 0;
  int q = x >> cmS;
  return dix > 0 ? (cmW >> cmS) - q : q + 1;
}

struct CmRun { uint32_t d, s, s2; int n; };
static int cmRuns(int b0, int b1, int db, CmRun *r) {
  int k = 0;
  if (!cmPl) {
    if (b0 <= b1) { r[0].d = b0; r[0].s = b0 + db; r[0].s2 = b0 + db + 1; r[0].n = b1 - b0 + 1; k = 1; }
    return k;
  }
  int e = b0 + (b0 & 1), o = b0 | 1;
  if (e <= b1) { r[k].d = cmOff(e); r[k].s = cmOff(e + db); r[k].s2 = cmOff(e + db + 1); r[k].n = ((b1 - e) >> 1) + 1; k++; }
  if (o <= b1) { r[k].d = cmOff(o); r[k].s = cmOff(o + db); r[k].s2 = cmOff(o + db + 1); r[k].n = ((b1 - o) >> 1) + 1; k++; }
  return k;
}

static inline void cmFill(uint8_t *p, int n, uint8_t v) {
  if (n >= 8) memset(p, v, n); else while (n-- > 0) *p++ = v;
}
static inline void cmCopy(uint8_t *d, const uint8_t *s, int n) {
  if (n >= 8) memcpy(d, s, n); else while (n-- > 0) *d++ = *s++;
}
static void cmApply(uint8_t *p, int n, uint32_t A, uint32_t X) {
  for (; n > 0 && ((uintptr_t)p & 3); n--, p++) *p = (uint8_t)((*p & A) ^ X);
  cmU32 *w = (cmU32 *)p;
  for (; n >= 4; n -= 4, w++) *w = (*w & A) ^ X;
  for (p = (uint8_t *)w; n > 0; n--, p++) *p = (uint8_t)((*p & A) ^ X);
}

static bool cmLopAX(int lop, uint8_t c, uint32_t &A, uint32_t &X) {
  uint32_t m = cmM, a, x;
  c &= m;
  if ((lop & 8) && c == 0) return false;
  switch (lop & 7) {
    case 0:  a = 0;       x = c; break;
    case 1:  a = c;       x = 0; break;
    case 2:  a = ~c & m;  x = c; break;
    case 3:  a = m;       x = c; break;
    case 4:  a = 0;       x = ~c & m; break;
    default: return false;
  }
  A = a * cmRep; X = x * cmRep;
  return true;
}

template<int T> static inline uint32_t cmNzMask(uint32_t s) {
  if (T == 1) { uint32_t x = s | (s >> 2); x |= x >> 1; x &= 0x11111111u; return (x << 4) - x; }
  if (T == 2) { uint32_t x = (s | (s >> 1)) & 0x55555555u; return x | (x << 1); }
  uint32_t x = s | (s >> 4); x |= x >> 2; x |= x >> 1; x &= 0x01010101u; return (x << 8) - x;
}
template<int OP, int T> static inline uint32_t cmLop(uint32_t s, uint32_t t) {
  uint32_t r = OP == 0 ? s : OP == 1 ? (s & t) : OP == 2 ? (s | t) : OP == 3 ? (s ^ t) : ~s;
  if (T) r = t ^ ((t ^ r) & cmNzMask<T>(s));
  return r;
}
template<int OP, int T> static void cmLopRun(uint8_t *d, const uint8_t *s, int n) {
  for (; n > 0 && ((uintptr_t)d & 3); n--, d++, s++) *d = (uint8_t)cmLop<OP, T>(*s, *d);
  if (n >= 4) {
    cmU32 *w = (cmU32 *)d;
    int o = (int)((uintptr_t)s & 3);
    if (!o) {
      const cmU32 *v = (const cmU32 *)s;
      for (; n >= 4; n -= 4, w++, v++) *w = cmLop<OP, T>(*v, *w);
      s = (const uint8_t *)v;
    } else {
      const cmU32 *v = (const cmU32 *)(s - o);
      int sr = o * 8, sl = 32 - sr;
      uint32_t a = *v++;
      for (; n >= 4; n -= 4, w++) {
        uint32_t b = *v++;
        *w = cmLop<OP, T>((a >> sr) | (b << sl), *w);
        a = b;
      }
      s = (const uint8_t *)v - 4 + o;
    }
    d = (uint8_t *)w;
  }
  for (; n > 0; n--, d++, s++) *d = (uint8_t)cmLop<OP, T>(*s, *d);
}
static void cmCopyRun(uint8_t *d, const uint8_t *s, int n) { cmCopy(d, s, n); }
typedef void (*CmRunFn)(uint8_t *, const uint8_t *, int);
static const CmRunFn kCmLopRun[5][4] = {
  { cmCopyRun,       cmLopRun<0, 1>, cmLopRun<0, 2>, cmLopRun<0, 3> },
  { cmLopRun<1, 0>,  cmLopRun<1, 1>, cmLopRun<1, 2>, cmLopRun<1, 3> },
  { cmLopRun<2, 0>,  cmLopRun<2, 0>, cmLopRun<2, 0>, cmLopRun<2, 0> },
  { cmLopRun<3, 0>,  cmLopRun<3, 0>, cmLopRun<3, 0>, cmLopRun<3, 0> },
  { cmLopRun<4, 0>,  cmLopRun<4, 1>, cmLopRun<4, 2>, cmLopRun<4, 3> },
};
static uint8_t cmLopByte(int lop, uint32_t s, uint32_t t) {
  uint32_t r;
  switch (lop & 7) {
    case 0:  r = s; break;
    case 1:  r = s & t; break;
    case 2:  r = s | t; break;
    case 3:  r = s ^ t; break;
    default: r = ~s; break;
  }
  if (lop & 8) r = t ^ ((t ^ r) & (cmBp == 2 ? cmNzMask<1>(s) : cmBp == 1 ? cmNzMask<2>(s) : cmNzMask<3>(s)));
  return (uint8_t)r;
}
template<int M> CM_NOINLINE static void cmLmmmPixT(int xs, int xd, int n, int dix, uint32_t rs, uint32_t rd, int lop) {
  typedef CmK<M> K;
  uint8_t *vr = vram;
  const int op = lop & 7;
  const bool t = (lop & 8) != 0;
  for (; n > 0; n--, xs += dix, xd += dix) {
    uint32_t bs = (uint32_t)xs >> K::S, bd = (uint32_t)xd >> K::S;
    uint32_t c = ((uint32_t)vr[rs + (K::PL ? ((bs >> 1) | ((bs & 1) << 16)) : bs)] >> cmShiftT<M>(xs)) & K::MASK;
    if (t && c == 0) continue;
    uint32_t a = rd + (K::PL ? ((bd >> 1) | ((bd & 1) << 16)) : bd);
    int sh = cmShiftT<M>(xd);
    uint32_t b = vr[a], d = (b >> sh) & K::MASK, r;
    switch (op) {
      case 0:  r = c; break;
      case 1:  r = c & d; break;
      case 2:  r = c | d; break;
      case 3:  r = c ^ d; break;
      default: r = (~c) & K::MASK; break;
    }
    vr[a] = (uint8_t)((b & ~((uint32_t)K::MASK << sh)) | (r << sh));
  }
}
static void cmLmmmPix(int xs, int xd, int n, int dix, uint32_t rs, uint32_t rd, int lop) {
  switch (cm) {
    case 0:  cmLmmmPixT<0>(xs, xd, n, dix, rs, rd, lop); break;
    case 1:  cmLmmmPixT<1>(xs, xd, n, dix, rs, rd, lop); break;
    case 2:  cmLmmmPixT<2>(xs, xd, n, dix, rs, rd, lop); break;
    default: cmLmmmPixT<3>(xs, xd, n, dix, rs, rd, lop); break;
  }
}
static int cmEdges(int xa, int xb, int *b0, int *b1, int *eb, uint8_t *em) {
  int pk = (1 << cmS) - 1, ne = 0;
  uint32_t fm = 0xFFu >> ((xa & pk) << cmBp), lm = (0xFFu << ((pk - (xb & pk)) << cmBp)) & 0xFF;
  *b0 = xa >> cmS; *b1 = xb >> cmS;
  if (*b0 == *b1) fm &= lm, lm = 0xFF;
  if (fm != 0xFF) { eb[ne] = *b0; em[ne++] = (uint8_t)fm; ++*b0; }
  if (lm != 0xFF) { eb[ne] = *b1; em[ne++] = (uint8_t)lm; --*b1; }
  return ne;
}

alignas(4) static MSX2_DRAM uint8_t cmTmp[264];

CM_NOINLINE static void cmShiftRow(uint8_t *t, const uint8_t *pa, const uint8_t *pb, int n, int sb) {
  const int rb = 8 - sb;
  const uint32_t mh = ((0xFFu << sb) & 0xFFu) * 0x01010101u, ml = (0xFFu >> rb) * 0x01010101u;
  int nw = n >> 2;
  if (nw > 0) {
    int oa = (int)((uintptr_t)pa & 3), ob = (int)((uintptr_t)pb & 3), sa = oa * 8, sbb = ob * 8;
    const cmU32 *va = (const cmU32 *)(pa - oa), *vb = (const cmU32 *)(pb - ob);
    cmU32 *w = (cmU32 *)t;
    uint32_t a = oa ? *va++ : 0, b = ob ? *vb++ : 0;
    for (int k = 0; k < nw; k++) {
      uint32_t A, B;
      if (oa) { uint32_t a2 = *va++; A = (a >> sa) | (a2 << (32 - sa)); a = a2; } else A = *va++;
      if (ob) { uint32_t b2 = *vb++; B = (b >> sbb) | (b2 << (32 - sbb)); b = b2; } else B = *vb++;
      w[k] = ((A << sb) & mh) | ((B >> rb) & ml);
    }
  }
  for (int i = nw * 4; i < n; i++) t[i] = (uint8_t)((pa[i] << sb) | (pb[i] >> rb));
}

CM_NOINLINE static void cmdLmmv(uint8_t clr, int nx, int ny) {
  int dix = C.dix, diy = C.diy;
  int cnt = cmMin(nx, cmAvailP(C.dx, dix));
  uint32_t A, X;
  if (cnt > 0 && cmLopAX(C.lop, clr, A, X)) {
    int xa = dix > 0 ? C.dx : C.dx - cnt + 1, b0, b1, eb[2];
    uint8_t em[2];
    int ne = cmEdges(xa, xa + cnt - 1, &b0, &b1, eb, em);
    uint32_t eo[2];
    for (int k = 0; k < ne; k++) eo[k] = cmOff(eb[k]);
    CmRun r[2];
    int nr = cmRuns(b0, b1, 0, r), y = C.dy;
    uint8_t *vr = vram;
    for (int j = 0; j < ny; j++, y += diy) {
      uint8_t *row = vr + cmRow(y);
      for (int k = 0; k < ne; k++) {
        uint8_t *p = row + eo[k];
        *p = (uint8_t)((*p & (A | (uint8_t)~em[k])) ^ (X & em[k]));
      }
      for (int k = 0; k < nr; k++) {
        if (!A) cmFill(row + r[k].d, r[k].n, (uint8_t)X);
        else    cmApply(row + r[k].d, r[k].n, A, X);
      }
    }
  }
  C.dy += ny * diy;
}

CM_NOINLINE static void cmdLmmm(int nx, int ny) {
  int dix = C.dix, diy = C.diy, lop = C.lop;
  int cnt = cmMin(nx, cmMin(cmAvailP(C.sx, dix), cmAvailP(C.dx, dix)));
  if (cnt > 0 && (lop & 7) <= 4) {
    int delta = C.sx - C.dx;
    int xa = dix > 0 ? C.dx : C.dx - cnt + 1, xb = xa + cnt - 1;
    bool same = ((C.dy - C.sy) & cmYM) == 0;
    int ys = C.sy, yd = C.dy;
    if (same && delta != 0 && delta < cnt && -delta < cnt) {
      for (int j = 0; j < ny; j++, yd += diy) {
        uint32_t rw = cmRow(yd);
        cmLmmmPix(C.sx, C.dx, cnt, dix, rw, rw, lop);
      }
    } else if (!(same && delta == 0 && (lop & 7) <= 2)) {
      int s = cmS, pk = (1 << cmS) - 1;
      CmRunFn fn = kCmLopRun[lop & 7][(lop & 8) ? (cm == 1 ? 2 : (cm == 3 ? 3 : 1)) : 0];
      uint8_t *vr = vram;
      CmRun r[2];
      if (!(delta & pk)) {
        int db = delta >> s, b0, b1, eb[2];
        uint8_t em[2];
        int ne = cmEdges(xa, xb, &b0, &b1, eb, em);
        uint32_t eo[2], es[2];
        for (int k = 0; k < ne; k++) { eo[k] = cmOff(eb[k]); es[k] = cmOff(eb[k] + db); }
        int nr = cmRuns(b0, b1, db, r);
        for (int j = 0; j < ny; j++, ys += diy, yd += diy) {
          uint8_t *rs = vr + cmRow(ys), *rd = vr + cmRow(yd);
          for (int k = 0; k < ne; k++) {
            uint8_t *p = rd + eo[k], t = *p;
            *p = (uint8_t)(t ^ ((t ^ cmLopByte(lop, rs[es[k]], t)) & em[k]));
          }
          for (int k = 0; k < nr; k++) fn(rd + r[k].d, rs + r[k].s, r[k].n);
        }
      } else {
        int D = delta * (1 << cmBp), jd = D >> 3, sb = D & 7;
        int b0, b1, eb[2];
        uint8_t em[2];
        int ne = cmEdges(xa, xb, &b0, &b1, eb, em);
        uint32_t eo[2], ea[2], ec[2];
        for (int k = 0; k < ne; k++) {
          eo[k] = cmOff(eb[k]);
          ea[k] = cmOff((uint32_t)(eb[k] + jd)); ec[k] = cmOff((uint32_t)(eb[k] + jd + 1));
        }
        int nr = cmRuns(b0, b1, jd, r);
        for (int j = 0; j < ny; j++, ys += diy, yd += diy) {
          uint32_t ros = cmRow(ys);
          const uint8_t *rs = vr + ros;
          uint8_t *rd = vr + cmRow(yd), *t = cmTmp;
          for (int k = 0; k < nr; k++) {
            cmShiftRow(t, rs + r[k].s, rs + r[k].s2, r[k].n, sb);
            fn(rd + r[k].d, t, r[k].n);
            t += (r[k].n + 3) & ~3;
          }
          for (int k = 0; k < ne; k++) {
            uint32_t sv = (uint32_t)(vr[(ros + ea[k]) & 0x1FFFF] << sb) | (vr[(ros + ec[k]) & 0x1FFFF] >> (8 - sb));
            uint8_t *p = rd + eo[k], tv = *p;
            *p = (uint8_t)(tv ^ ((tv ^ cmLopByte(lop, sv & 0xFF, tv)) & em[k]));
          }
        }
      }
    }
  }
  C.sy += ny * diy; C.dy += ny * diy;
}

CM_NOINLINE static void cmdHmmv(uint8_t clr, int nx, int ny) {
  int dix = C.dix, diy = C.diy, n = nx >> cmS;
  if (n <= 0) n = 1;
  int cnt = cmMin(n, cmAvailB(C.dx, dix));
  if (cnt > 0) {
    int q = C.dx >> cmS, b0 = dix > 0 ? q : q - cnt + 1, y = C.dy;
    CmRun r[2];
    int nr = cmRuns(b0, b0 + cnt - 1, 0, r);
    uint8_t *vr = vram;
    for (int j = 0; j < ny; j++, y += diy) {
      uint8_t *row = vr + cmRow(y);
      for (int k = 0; k < nr; k++) cmFill(row + r[k].d, r[k].n, clr);
    }
  }
  C.dy += ny * diy;
}

CM_NOINLINE static void cmdHmmm(int nx, int ny) {
  int dix = C.dix, diy = C.diy, n = nx >> cmS;
  if (n <= 0) n = 1;
  int cnt = cmMin(n, cmMin(cmAvailB(C.sx, dix), cmAvailB(C.dx, dix)));
  if (cnt > 0) {
    int qs = C.sx >> cmS, qd = C.dx >> cmS, db = qs - qd;
    int b0 = dix > 0 ? qd : qd - cnt + 1, ys = C.sy, yd = C.dy;
    bool same = ((C.dy - C.sy) & cmYM) == 0;
    uint8_t *vr = vram;
    if (!same || db >= cnt || -db >= cnt) {
      CmRun r[2];
      int nr = cmRuns(b0, b0 + cnt - 1, db, r);
      for (int j = 0; j < ny; j++, ys += diy, yd += diy) {
        const uint8_t *rs = vr + cmRow(ys);
        uint8_t *rd = vr + cmRow(yd);
        for (int k = 0; k < nr; k++) cmCopy(rd + r[k].d, rs + r[k].s, r[k].n);
      }
    } else if (db != 0) {
      if (!cmPl && db * dix > 0) {
        for (int j = 0; j < ny; j++, yd += diy) {
          uint8_t *row = vr + cmRow(yd);
          memmove(row + b0, row + b0 + db, cnt);
        }
      } else {
        for (int j = 0; j < ny; j++, yd += diy) {
          uint8_t *row = vr + cmRow(yd);
          for (int i = 0, xs = qs, xd = qd; i < cnt; i++, xs += dix, xd += dix) row[cmOff(xd)] = row[cmOff(xs)];
        }
      }
    }
  }
  C.sy += ny * diy; C.dy += ny * diy;
}

CM_NOINLINE static void cmdYmmm(int ny) {
  int dix = C.dix, diy = C.diy;
  int cnt = cmAvailB(C.dx, dix);
  if (cnt > 0 && ((C.dy - C.sy) & cmYM) != 0) {
    int q = C.dx >> cmS, b0 = dix > 0 ? q : q - cnt + 1, ys = C.sy, yd = C.dy;
    CmRun r[2];
    int nr = cmRuns(b0, b0 + cnt - 1, 0, r);
    uint8_t *vr = vram;
    for (int j = 0; j < ny; j++, ys += diy, yd += diy) {
      const uint8_t *rs = vr + cmRow(ys);
      uint8_t *rd = vr + cmRow(yd);
      for (int k = 0; k < nr; k++) cmCopy(rd + r[k].d, rs + r[k].s, r[k].n);
    }
  }
  C.sy += ny * diy; C.dy += ny * diy;
}

static void cmdFinish() {
  C.active = false;
  if (!(C.regW & 1)) { S.reg[34] = C.sy & 0xFF; S.reg[35] = (C.sy >> 8) & 3; }
  if (!(C.regW & 2)) { S.reg[38] = C.dy & 0xFF; S.reg[39] = (C.dy >> 8) & 3; }
  C.regW = 0;
}

typedef void (*CmXferW)(uint8_t);
static int cmStepX;
static CmXferW cmXferW;
static void (*cmXferR)();

template<int M> static inline bool cmAdvT() {
  C.x += cmStepX;
  if (++C.i >= C.nx || (unsigned)C.x >= (unsigned)CmK<M>::W) {
    C.i = 0;
    C.x = C.sxc;
    C.dy += C.diy; C.sy += C.diy;
    C.y += C.diy;
    if (--C.rows <= 0) { cmdFinish(); return false; }
  }
  return true;
}
template<int M> static void cmLmmcT(uint8_t v) { psetT<M>(C.x, C.y, v, C.lop); cmAdvT<M>(); }
template<int M> static void cmHmmcT(uint8_t v) {
  uint32_t a = cmAddrT<M>(C.x, C.y);
  vram[a] = v;
  shPoke(a, v);
  cmAdvT<M>();
}
template<int M> static void cmLmcmT() { if (cmAdvT<M>()) S.status[7] = pgetT<M>(C.x, C.y); }
static const CmXferW kCmLmmc[4] = { cmLmmcT<0>, cmLmmcT<1>, cmLmmcT<2>, cmLmmcT<3> };
static const CmXferW kCmHmmc[4] = { cmHmmcT<0>, cmHmmcT<1>, cmHmmcT<2>, cmHmmcT<3> };
static void (*const kCmLmcm[4])() = { cmLmcmT<0>, cmLmcmT<1>, cmLmcmT<2>, cmLmcmT<3> };

template<int M, int MAJ> CM_NOINLINE static int cmLineT(int x, int y, int nx, int ny, int dix, int diy,
                                                        uint32_t A, uint32_t X) {
  uint8_t *vr = vram;
  int asx = ((nx - 1) >> 1) & 1023;
  for (int n = nx;; n--) {
    uint32_t a = cmAddrT<M>(x, y), pm = (uint32_t)CmK<M>::MASK << cmShiftT<M>(x);
    vr[a] = (uint8_t)((vr[a] & (A | ~pm)) ^ (X & pm));
    shPoke(a, vr[a]);
    if (!MAJ) {
      x += dix;
      if (asx < ny) { asx += nx; y += diy; }
    } else {
      y += diy;
      if (asx < ny) { asx += nx; x += dix; }
    }
    asx = (asx - ny) & 1023;
    if (n == 0 || (unsigned)x >= (unsigned)CmK<M>::W) break;
  }
  return y;
}
static void cmdLine(uint8_t clr, int lop, int arg) {
  int nx = C.nx & 1023, ny = C.ny & 1023;
  uint32_t A, X;
  if (!cmLopAX(lop, clr, A, X)) { A = 0xFFFFFFFFu; X = 0; }
  int (*fn)(int, int, int, int, int, int, uint32_t, uint32_t);
  switch (cm * 2 + (arg & 1)) {
    case 0:  fn = cmLineT<0, 0>; break;
    case 1:  fn = cmLineT<0, 1>; break;
    case 2:  fn = cmLineT<1, 0>; break;
    case 3:  fn = cmLineT<1, 1>; break;
    case 4:  fn = cmLineT<2, 0>; break;
    case 5:  fn = cmLineT<2, 1>; break;
    case 6:  fn = cmLineT<3, 0>; break;
    default: fn = cmLineT<3, 1>; break;
  }
  C.dy = fn(C.dx, C.dy, nx, ny, C.dix, C.diy, A, X);
}

template<int M> CM_NOINLINE static int cmSrchT(int x, int y, int dix, uint8_t want, bool eq, bool *found) {
  const uint8_t *vr = vram;
  for (;;) {
    uint8_t c = (vr[cmAddrT<M>(x, y)] >> cmShiftT<M>(x)) & CmK<M>::MASK;
    if (eq ? (c != want) : (c == want)) { *found = true; break; }
    x += dix;
    if (!(x >= 0 && x < CmK<M>::W)) break;
  }
  return x;
}

static int cmTiming(int op) {
  static const uint8_t t[7][4] = {
    { 92, 125,  92,  92 },
    {120, 147, 120, 132 },
    { 98, 137,  98, 124 },
    {129, 197, 129, 132 },
    { 49,  65,  49,  62 },
    { 92, 136,  92,  97 },
    { 65, 125,  65,  68 },
  };
  int k = ((S.reg[1] >> 6) & 1) | (S.reg[8] & 2);
  int r = op == 0x6 ? 0 : op == 0x7 ? 1 : op == 0x8 ? 2 : op == 0x9 ? 3 : op == 0xC ? 4 : op == 0xD ? 5 : 6;
  return t[r][k];
}
static const int kCmCreditPerLine = 1368 * MSX2_VDP_CMD_SPEED / 100;

static void cmdMarkRows(int y0, int n, int diy) {
  if (!shALen && shP0 >= 0x20000) return;
  for (int k = 0; k < n; k++) {
    uint32_t lo = (uint32_t)((y0 + k * diy) & cmYM) << 7;
    shSync(lo, lo + 128);
    if (cmPl) shSync(lo | 0x10000, (lo | 0x10000) + 128);
  }
}

static inline int lineCycleNow() {
  int c = vdpLineCycle();
  return c < 0 ? 0 : (c > MSX2_CYCLES_PER_LINE ? MSX2_CYCLES_PER_LINE : c);
}

static void cmdBusy(int units, int rows) {
  if (units < 1) units = 1;
  C.blk = true;
  C.bleft = rows;
  C.rowCost = units * cmTiming(C.op) * MSX2_CYCLES_PER_LINE;
  C.credit = 0;
  C.syncCyc = lineCycleNow();
}

static void cmdSyncTo(int cyc) {
  if (cyc <= C.syncCyc) return;
#if MSX2_PROFILE
  uint32_t t0 = msx2ProfClock();
#endif
  C.credit += (cyc - C.syncCyc) * kCmCreditPerLine;
  C.syncCyc = cyc;
  if (C.credit >= C.rowCost) {
    int n = C.credit / C.rowCost;
    if (n > C.bleft) n = C.bleft;
    C.credit -= n * C.rowCost;
    int y0 = C.dy;
    switch (C.op) {
      case 0x8: cmdLmmv(C.bclr, C.bnx, n); cmdMarkRows(y0, n, C.diy); break;
      case 0x9: cmdLmmm(C.bnx, n); cmdMarkRows(y0, n, C.diy); break;
      case 0xC: cmdHmmv(C.bclr, C.bnx, n); cmdMarkRows(y0, n, C.diy); break;
      case 0xD: cmdHmmm(C.bnx, n); cmdMarkRows(y0, n, C.diy); break;
      case 0xE: cmdYmmm(n); cmdMarkRows(y0, n, C.diy); break;
      default: break;
    }
    if ((C.bleft -= n) <= 0) { C.blk = false; cmdFinish(); }
  }
#if MSX2_PROFILE
  P.cmdCycles[C.op] += msx2ProfClock() - t0;
#endif
}

static void cmdStart(uint8_t v) {
  cmdSetMode();
  C.op  = v >> 4;
  C.lop = v & 15;
  C.sx = S.reg[32] | ((S.reg[33] & 1) << 8);
  C.sy = S.reg[34] | ((S.reg[35] & 3) << 8);
  C.dx = S.reg[36] | ((S.reg[37] & 1) << 8);
  C.dy = S.reg[38] | ((S.reg[39] & 3) << 8);
  C.nx = S.reg[40] | ((S.reg[41] & 1) << 8);
  C.ny = S.reg[42] | ((S.reg[43] & 3) << 8);
  uint8_t arg = S.reg[45], clr = S.reg[44];
  C.dix = (arg & 4) ? -1 : 1;
  C.diy = (arg & 8) ? -1 : 1;
  int nx = C.nx ? C.nx : 512, ny = C.ny ? C.ny : 1024;
  C.active = false;
  C.blk = false;
  C.regW = 0;
#if MSX2_PROFILE
  switch (C.op) {
    case 0x7: P.cmdDots += (C.nx & 1023) + 1; break;
    case 0x8: case 0x9: case 0xC: case 0xD: P.cmdDots += (uint32_t)nx * ny; break;
    case 0xE: {
      int w = (C.dx >= cmW) ? 0 : (C.dix > 0 ? cmW - C.dx : C.dx + 1);
      P.cmdDots += (uint32_t)w * ny;
      break;
    }
    default: break;
  }
#endif

  switch (C.op) {
    case 0x4:
      S.status[7] = pget(C.sx, C.sy);
      return;
    case 0x5:
      pset(C.dx, C.dy, clr, C.lop);
      return;
    case 0x6: {
      int x = C.sx;
      bool eq = (arg & 2) != 0, found = false;
      uint8_t want = clr & cmMask();
      switch (cm) {
        case 0:  x = cmSrchT<0>(x, C.sy, C.dix, want, eq, &found); break;
        case 1:  x = cmSrchT<1>(x, C.sy, C.dix, want, eq, &found); break;
        case 2:  x = cmSrchT<2>(x, C.sy, C.dix, want, eq, &found); break;
        default: x = cmSrchT<3>(x, C.sy, C.dix, want, eq, &found); break;
      }
      if (found) S.status[2] |= 0x10; else S.status[2] &= ~0x10;
      S.status[8] = x & 0xFF;
      S.status[9] = 0xFE | ((x >> 8) & 1);
      if (kCmCreditPerLine) cmdBusy(x > C.sx ? x - C.sx + 1 : C.sx - x + 1, 1);
      return;
    }
    case 0x7:
      cmdLine(clr, C.lop, arg);
      if (kCmCreditPerLine) cmdBusy((C.nx & 1023) + 1, 1);
      else cmdFinish();
      return;
    case 0x8:
      if (kCmCreditPerLine) { C.bclr = clr; C.bnx = nx; cmdBusy(cmMin(nx, cmAvailP(C.dx, C.dix)), ny); return; }
      { int y0 = C.dy; cmdLmmv(clr, nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0x9:
      if (kCmCreditPerLine) {
        C.bnx = nx;
        cmdBusy(cmMin(nx, cmMin(cmAvailP(C.sx, C.dix), cmAvailP(C.dx, C.dix))), ny);
        return;
      }
      { int y0 = C.dy; cmdLmmm(nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xA:
      C.active = true;
      C.x = C.sxc = C.sx; C.y = C.sy; C.i = 0; C.nx = nx; C.rows = ny;
      cmStepX = C.dix; cmXferR = kCmLmcm[cm];
      S.status[7] = pget(C.x, C.y);
      return;
    case 0xB:
      C.active = true;
      C.x = C.sxc = C.dx; C.y = C.dy; C.i = 0; C.nx = nx; C.rows = ny;
      cmStepX = C.dix; cmXferW = kCmLmmc[cm];
      cmXferW(clr);
      return;
    case 0xC:
      if (kCmCreditPerLine) { C.bclr = clr; C.bnx = nx; cmdBusy(cmMin(nx >> cmS, cmAvailB(C.dx, C.dix)), ny); return; }
      { int y0 = C.dy; cmdHmmv(clr, nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xD:
      if (kCmCreditPerLine) {
        C.bnx = nx;
        cmdBusy(cmMin(nx >> cmS, cmMin(cmAvailB(C.sx, C.dix), cmAvailB(C.dx, C.dix))), ny);
        return;
      }
      { int y0 = C.dy; cmdHmmm(nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xE:
      if (kCmCreditPerLine) { cmdBusy(cmAvailB(C.dx, C.dix), ny); return; }
      { int y0 = C.dy; cmdYmmm(ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xF: {
      int ppb = cmPixPerByte(), n = nx / ppb; if (n <= 0) n = 1;
      C.active = true;
      C.x = C.sxc = C.dx; C.y = C.dy; C.i = 0; C.nx = n; C.rows = ny;
      cmStepX = C.dix * ppb; cmXferW = kCmHmmc[cm];
      cmXferW(clr);
      return;
    }
    default:
      return;
  }
}

static void cmdWriteData(uint8_t v) {
  if (!C.active) return;
#if MSX2_PROFILE
  if (C.op == 0xB || C.op == 0xF) P.cmdData++;
#endif
  if (C.op == 0xB || C.op == 0xF) cmXferW(v);
}

static uint8_t cmdReadData() {
  uint8_t v = S.status[7];
  if (C.active && C.op == 0xA) {
#if MSX2_PROFILE
    P.cmdData++;
#endif
    cmXferR();
  }
  return v;
}

#ifndef ARDUINO
void (*vdpRegHook)(int r, uint8_t v);
#endif
static void writeReg(int r, uint8_t v) {
  if (r >= 47) return;
  if (is9938 && r >= 25 && r <= 27) return;
#ifndef ARDUINO
  if (vdpRegHook) vdpRegHook(r, v);
#endif
  S.reg[r] = v;
  switch (r) {
    case 0: if (!(v & 0x10)) S.status[1] &= ~0x01; break;
    case 14: S.vramAddr = ((uint32_t)(v & 7) << 14) | (S.vramAddr & 0x3FFF); break;
    case 16: S.palLatchFull = false; break;
    case 34: case 35: if (C.active || C.blk) C.regW |= 1; break;
    case 38: case 39: if (C.active || C.blk) C.regW |= 2; break;
#if MSX2_PROFILE
    case 44:
      if (C.active) {
        uint32_t t0 = msx2ProfClock();
        cmdWriteData(v);
        P.cmdCycles[C.op] += msx2ProfClock() - t0;
      }
      break;
    case 46: {
      uint32_t t0 = msx2ProfClock();
      cmdStart(v);
      uint32_t d = msx2ProfClock() - t0;
      P.cmdCycles[v >> 4] += d;
      P.cmdCount[v >> 4]++;
      if (d > P.cmdMax) {
        P.cmdMax = d;
        P.cmdMaxCmd = v;
        P.cmdMaxNx = (uint16_t)(S.reg[40] | ((S.reg[41] & 1) << 8));
        P.cmdMaxNy = (uint16_t)(S.reg[42] | ((S.reg[43] & 3) << 8));
      }
      break;
    }
#else
    case 44: cmdWriteData(v); break;
    case 46: cmdStart(v); break;
#endif
    default: break;
  }
}

static inline void incAddr() {
  uint32_t low = (S.vramAddr + 1) & 0x3FFF;
  uint32_t hi  = S.vramAddr & 0x1C000;
  if (low == 0 && v9938Mode(curMode())) {
    S.reg[14] = (S.reg[14] + 1) & 7;
    hi = (uint32_t)S.reg[14] << 14;
  }
  S.vramAddr = hi | low;
}
static inline uint32_t cpuVramAddr() {
  return planarMode(curMode()) ? planar(S.vramAddr) : S.vramAddr;
}

uint8_t vdpReadPort(int port) {
  vdpSync();
  switch (port & 3) {
    case 0: {
#if MSX2_PROFILE
      P.vramReads++;
#endif
      uint8_t v = S.readAhead;
      S.readAhead = vram[cpuVramAddr()];
      incAddr();
      S.latchFull = false;
      return v;
    }
    case 1: {
      S.latchFull = false;
      switch (S.reg[15] & 15) {
        case 0: {
          uint8_t v = S.status[0];
          S.status[0] &= 0x1F;
          return v;
        }
        case 1: {
          uint8_t v = (S.status[1] & 0x01) | (is9938 ? 0x00 : 0x04);
          S.status[1] &= ~0x01;
          if (!(S.reg[0] & 0x10) && fhMatch) v |= 0x01;
          return v;
        }
        case 2: {
          uint8_t v = 0x0C | (S.status[2] & 0x10);
          if (C.active) v |= 0x81;
          else if (C.blk) v |= 0x01;
          if (vrFlag) v |= 0x40;
          if (vdpLineCycle() * 4 >= MSX2_CYCLES_PER_LINE * 3) v |= 0x20;
          return v;
        }
        case 3: case 5: return 0;
        case 4: return 0xFE;
        case 6: return 0xFC;
#if MSX2_PROFILE
        case 7:
          if (C.active) {
            uint32_t t0 = msx2ProfClock();
            uint8_t v = cmdReadData();
            P.cmdCycles[C.op] += msx2ProfClock() - t0;
            return v;
          }
          return cmdReadData();
#else
        case 7: return cmdReadData();
#endif
        case 8: return S.status[8];
        case 9: return S.status[9] | 0xFE;
        default: return 0xFF;
      }
    }
    default:
      return 0xFF;
  }
}

void vdpWritePort(int port, uint8_t v) {
  switch (port & 3) {
    case 0:
#if MSX2_PROFILE
      P.vramWrites++;
#endif
      vdpSync();
      {
        uint32_t a = cpuVramAddr();
        vram[a] = v;
        shPoke(a, v);
      }
      S.readAhead = v;
      incAddr();
      S.latchFull = false;
      return;
    case 1:
      if (!S.latchFull) {
        S.latch = v;
        S.latchFull = true;
        return;
      }
      S.latchFull = false;
      if (v & 0x80) {
        vdpSync();
        writeReg(v & 0x3F, S.latch);
      } else {
        S.vramAddr = ((uint32_t)(S.reg[14] & 7) << 14) | ((uint32_t)(v & 0x3F) << 8) | S.latch;
        if (!(v & 0x40)) {
          S.readAhead = vram[cpuVramAddr()];
          incAddr();
        }
      }
      return;
    case 2: {
      if (!S.palLatchFull) {
        S.palLatch = v;
        S.palLatchFull = true;
        return;
      }
      S.palLatchFull = false;
      vdpSync();
      int i = S.reg[16] & 15;
      S.palette[i] = (uint16_t)(((S.palLatch & 0x70) << 4) | ((v & 7) << 4) | (S.palLatch & 7));
      updatePalNat(i);
      S.reg[16] = (i + 1) & 15;
      return;
    }
    case 3: {
      vdpSync();
      int r = S.reg[17] & 0x3F;
      if (r != 17) writeReg(r, v);
      if (!(S.reg[17] & 0x80)) S.reg[17] = (S.reg[17] & 0xC0) | ((r + 1) & 0x3F);
      return;
    }
  }
}

#if defined(__GNUC__)
typedef uint32_t __attribute__((may_alias)) u32a;
typedef uint16_t __attribute__((may_alias)) u16a;
#define VDP_NOINLINE __attribute__((noinline))
#else
typedef uint32_t u32a;
typedef uint16_t u16a;
#define VDP_NOINLINE __declspec(noinline)
#endif
#ifndef VDP_HOT
#define VDP_HOT MSX2_FAST
#endif

alignas(4) static MSX2_DRAM uint8_t lbOut[MSX2_SCREEN_W];
static inline u32a *yjkBuf() { return (u32a *)lb; }
static inline u16a *sprPix() { return (u16a *)lb2; }
struct LineTabs {
  uint32_t nib[16][2];
  uint32_t plW[16];
  uint32_t pair[4];
  uint32_t quad[16];
};
static MSX2_DRAM LineTabs LT = {
  { { 0x00000000, 0x00000000 }, { 0x00000000, 0x0000FFFF }, { 0x00000000, 0xFFFF0000 }, { 0x00000000, 0xFFFFFFFF },
    { 0x0000FFFF, 0x00000000 }, { 0x0000FFFF, 0x0000FFFF }, { 0x0000FFFF, 0xFFFF0000 }, { 0x0000FFFF, 0xFFFFFFFF },
    { 0xFFFF0000, 0x00000000 }, { 0xFFFF0000, 0x0000FFFF }, { 0xFFFF0000, 0xFFFF0000 }, { 0xFFFF0000, 0xFFFFFFFF },
    { 0xFFFFFFFF, 0x00000000 }, { 0xFFFFFFFF, 0x0000FFFF }, { 0xFFFFFFFF, 0xFFFF0000 }, { 0xFFFFFFFF, 0xFFFFFFFF } },
  { 0 },
  { 0x00000000, 0x0000FFFF, 0xFFFF0000, 0xFFFFFFFF },
  { 0x00000000, 0x0000FF00, 0x000000FF, 0x0000FFFF, 0xFF000000, 0xFF00FF00, 0xFF0000FF, 0xFF00FFFF,
    0x00FF0000, 0x00FFFF00, 0x00FF00FF, 0x00FFFFFF, 0xFFFF0000, 0xFFFFFF00, 0xFFFF00FF, 0xFFFFFFFF }
};
static MSX2_DRAM uint32_t plKey[4];
static uint32_t plGen = 1;
alignas(4) static MSX2_DRAM uint32_t bmT[256];
static uint32_t bmGen, bmMode;
alignas(4) static MSX2_DRAM uint16_t g6T[256];
static uint32_t g6Gen;
alignas(4) static MSX2_DRAM uint16_t g7T[256];
static int g7Sync = -1;
static MSX2_DRAM uint32_t sprW[16];
static uint32_t sprWGen, sprWMode = ~0u;

static inline uint8_t vr(uint32_t a, bool pl) { return vram[pl ? planar(a & 0x1FFFF) : (a & 0x1FFFF)]; }

static inline void plUpdate() {
  uint32_t k[4];
  for (int i = 0; i < 4; i++)
    k[i] = palNat[4 * i] | ((uint32_t)palNat[4 * i + 1] << 8) | ((uint32_t)palNat[4 * i + 2] << 16) |
           ((uint32_t)palNat[4 * i + 3] << 24);
  if (!(S.reg[8] & 0x20)) k[0] = (k[0] & 0xFFFFFF00u) | palNat[S.reg[7] & 15];
  if (MSX2_LIKELY(k[0] == plKey[0] && k[1] == plKey[1] && k[2] == plKey[2] && k[3] == plKey[3])) return;
  for (int i = 0; i < 4; i++) plKey[i] = k[i];
  const uint8_t *pl = (const uint8_t *)plKey;
  for (int i = 0; i < 16; i++) LT.plW[i] = pl[i] * 0x01010101u;
  plGen++;
}

static VDP_NOINLINE void bgText1(u32a *d, int y, uint32_t bd4) {
  const LineTabs *T = &LT;
  uint32_t name = (uint32_t)(S.reg[2] & 0x7F) << 10;
  const uint8_t *pt = &vram[((uint32_t)(S.reg[4] & 0x3F) << 11) | (y & 7)];
  uint32_t B = T->plW[S.reg[7] & 15], X = T->plW[S.reg[7] >> 4] ^ B;
  int n = (y >> 3) * 40;
  d[0] = d[1] = d[2] = d[3] = bd4;
  d += 4;
  for (int c = 0; c < 40; c++, d += 3) {
    uint32_t p = pt[(uint32_t)vram[name | ((n + c) & 0x3FF)] << 3];
    d[0] = B ^ (X & T->pair[p >> 6]);
    d[1] = B ^ (X & T->pair[(p >> 4) & 3]);
    d[2] = B ^ (X & T->pair[(p >> 2) & 3]);
  }
  d[0] = d[1] = d[2] = d[3] = bd4;
}

static VDP_NOINLINE void bgText2(u32a *d, int y, uint32_t bd4) {
  const LineTabs *T = &LT;
  uint32_t name = (uint32_t)(S.reg[2] & 0x7C) << 10;
  const uint8_t *pt = &vram[((uint32_t)(S.reg[4] & 0x3F) << 11) | (y & 7)];
  uint32_t blk  = ((uint32_t)(S.reg[10] & 7) << 14) | ((uint32_t)(S.reg[3] & 0xF8) << 6);
  uint32_t B0 = T->plW[S.reg[7] & 15], X0 = T->plW[S.reg[7] >> 4] ^ B0;
  uint32_t B1 = T->plW[S.reg[12] & 15], X1 = T->plW[S.reg[12] >> 4] ^ B1;
  bool blink = S.blinkOn;
  int n = (y >> 3) * 80;
  d[0] = d[1] = d[2] = d[3] = bd4;
  d += 4;
  for (int c = 0; c < 40; c++, n += 2, d += 3) {
    uint32_t pa = pt[(uint32_t)vram[name | (n & 0xFFF)] << 3];
    uint32_t pb = pt[(uint32_t)vram[name | ((n + 1) & 0xFFF)] << 3];
    uint32_t Ba = B0, Xa = X0, Bb = B0, Xb = X0;
    if (blink) {
      uint32_t at = vram[blk | ((n >> 3) & 0x1FF)] << (n & 7);
      if (at & 0x80) { Ba = B1; Xa = X1; }
      if (at & 0x40) { Bb = B1; Xb = X1; }
    }
    d[0] = Ba ^ (Xa & T->quad[pa >> 4]);
    d[1] = ((Ba ^ (Xa & T->quad[pa & 0x0C])) & 0xFFFF0000u) | ((Bb ^ (Xb & T->quad[pb >> 6])) & 0x0000FFFFu);
    d[2] = Bb ^ (Xb & T->quad[(pb >> 2) & 15]);
  }
  d[0] = d[1] = d[2] = d[3] = bd4;
}

static inline void patWords(const LineTabs *T, uint32_t p, uint32_t cc,
                            uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) {
  const uint8_t *pw = (const uint8_t *)T->plW, *nb = (const uint8_t *)T->nib;
  uint32_t B = *(const uint32_t *)(pw + ((cc & 15) << 2));
  uint32_t X = *(const uint32_t *)(pw + ((cc >> 4) << 2)) ^ B;
  const uint32_t *a = (const uint32_t *)(nb + ((p >> 4) << 3));
  const uint32_t *b = (const uint32_t *)(nb + ((p & 15) << 3));
  w0 = B ^ (X & a[0]); w1 = B ^ (X & a[1]); w2 = B ^ (X & b[0]); w3 = B ^ (X & b[1]);
}
struct PatG1 {
  const uint8_t *pt, *ct;
  const LineTabs *T;
  inline void gen(uint32_t ch, uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) const {
    patWords(T, pt[ch << 3], ct[ch >> 3], w0, w1, w2, w3);
  }
};
struct PatG2 {
  const uint8_t *pt, *ct;
  const LineTabs *T;
  uint32_t cm;
  inline void gen(uint32_t ch, uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) const {
    ch <<= 3;
    patWords(T, pt[ch], ct[ch & cm], w0, w1, w2, w3);
  }
};
struct PatG3 {
  const uint8_t *pt, *ct;
  const LineTabs *T;
  inline void gen(uint32_t ch, uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) const {
    ch <<= 3;
    patWords(T, pt[ch], ct[ch], w0, w1, w2, w3);
  }
};
struct PatMC {
  const uint8_t *pt;
  const LineTabs *T;
  inline void gen(uint32_t ch, uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) const {
    uint32_t p = pt[ch << 3];
    w0 = w1 = T->plW[p >> 4];
    w2 = w3 = T->plW[p & 15];
  }
};

template <class G, bool Odd>
static VDP_NOINLINE u32a *patRun(u32a *o, const G g, const uint8_t *nm, int n, uint32_t &prev) {
  uint32_t pv = prev;
  for (int i = 0; i < n; i++, o += 4) {
    uint32_t w0, w1, w2, w3;
    g.gen(nm[i], w0, w1, w2, w3);
    if (Odd) {
      uint32_t t = w3;
      w3 = (w2 << 16) | (w3 >> 16); w2 = (w1 << 16) | (w2 >> 16);
      w1 = (w0 << 16) | (w1 >> 16); w0 = (pv << 16) | (w0 >> 16);
      pv = t;
    }
    o[0] = w0; o[1] = w1; o[2] = w2; o[3] = w3;
  }
  prev = pv;
  return o;
}
template <class G, bool Odd>
static inline void patLine(u32a *d, const G &g, const uint8_t *nm, int k) {
  int c = k >> 2, j0 = k & 3;
  uint32_t prev = 0, w0, w1, w2, w3;
  if (Odd) { g.gen(nm[(c - 1) & 31], w0, w1, w2, w3); prev = w3; }
  u32a *o = d;
  if (j0) {
    g.gen(nm[c], w0, w1, w2, w3);
    if (Odd) {
      uint32_t t = w3;
      w3 = (w2 << 16) | (w3 >> 16); w2 = (w1 << 16) | (w2 >> 16);
      w1 = (w0 << 16) | (w1 >> 16); w0 = (prev << 16) | (w0 >> 16);
      prev = t;
    }
    if (j0 == 1)      { d[0] = w1; d[1] = w2; d[2] = w3; d[127] = w0; }
    else if (j0 == 2) { d[0] = w2; d[1] = w3; d[126] = w0; d[127] = w1; }
    else              { d[0] = w3; d[125] = w0; d[126] = w1; d[127] = w2; }
    o = d + 4 - j0;
    c = (c + 1) & 31;
  }
  int total = j0 ? 31 : 32, n1 = 32 - c;
  if (n1 > total) n1 = total;
  o = patRun<G, Odd>(o, g, nm + c, n1, prev);
  patRun<G, Odd>(o, g, nm, total - n1, prev);
}
template <class G>
static inline void patScroll(u32a *d, const G &g, const uint8_t *nm, int hs) {
  if (hs & 1) patLine<G, true>(d, g, nm, ((hs >> 1) + 1) & 127);
  else        patLine<G, false>(d, g, nm, hs >> 1);
}

static VDP_NOINLINE void bgPattern(int m, u32a *d, int y) {
  int hs = (((S.reg[26] & 0x3F) << 3) - (S.reg[27] & 7)) & 255;
  int row = y >> 3, yy = y & 7;
  const uint8_t *nm = &vram[((uint32_t)(S.reg[2] & 0x7F) << 10) | ((uint32_t)row << 5)];
  if (m == MODE_G1) {
    PatG1 g;
    g.pt = &vram[((uint32_t)(S.reg[4] & 0x3F) << 11) | yy];
    g.ct = &vram[((uint32_t)(S.reg[10] & 7) << 14) | ((uint32_t)S.reg[3] << 6)];
    g.T = &LT;
    patScroll(d, g, nm, hs);
  } else if (m == MODE_MC) {
    PatMC g;
    g.pt = &vram[((uint32_t)(S.reg[4] & 0x3F) << 11) | ((row & 3) << 1) | ((y >> 2) & 1)];
    g.T = &LT;
    patScroll(d, g, nm, hs);
  } else {
    uint32_t off   = ((uint32_t)(row & 0x18) << 8) | yy;
    uint32_t pmask = ((uint32_t)(S.reg[4] & 3) << 11) | 0x7FF;
    uint32_t cmask = ((uint32_t)(S.reg[3] & 0x7F) << 6) | 0x3F;
    const uint8_t *pt = &vram[((uint32_t)(S.reg[4] & 0x3C) << 11) | (off & pmask)];
    const uint8_t *ct = &vram[((uint32_t)(S.reg[10] & 7) << 14) | ((uint32_t)(S.reg[3] & 0x80) << 6) | (off & cmask)];
    if ((cmask & 0x7F8) == 0x7F8) {
      PatG3 g; g.pt = pt; g.ct = ct; g.T = &LT;
      patScroll(d, g, nm, hs);
    } else {
      PatG2 g; g.pt = pt; g.ct = ct; g.T = &LT; g.cm = cmask & 0x7F8;
      patScroll(d, g, nm, hs);
    }
  }
}

struct BmSrc { const uint8_t *e, *o; };
struct BmT32 {
  enum { kStep = 1 };
  const uint32_t *t;
  inline uint32_t operator()(const uint8_t *e, const uint8_t *, int i) const { return t[e[i]]; }
};
struct BmT16 {
  enum { kStep = 1 };
  const uint16_t *t;
  inline uint32_t operator()(const uint8_t *e, const uint8_t *o, int i) const { return t[o[i]] | ((uint32_t)t[e[i]] << 16); }
};
struct BmWord {
  enum { kStep = 4 };
  inline uint32_t operator()(const uint8_t *e, const uint8_t *, int i) const { return ((const u32a *)e)[i]; }
};
template <class G>
static VDP_NOINLINE u32a *bmRun(u32a *d, const G g, const uint8_t *e, const uint8_t *o, int n) {
  for (int i = 0; i < n; i++) d[i] = g(e, o, i);
  return d + n;
}
static VDP_NOINLINE VDP_HOT u32a *bmRun(u32a *d, const BmT32 g, const uint8_t *e, const uint8_t *, int n) {
  const uint32_t *t = g.t;
  int i = 0;
  if (!((uintptr_t)e & 3))
    for (; i + 4 <= n; i += 4) {
      uint32_t w = *(const u32a *)(e + i);
      d[i] = t[w & 0xFF]; d[i + 1] = t[(w >> 8) & 0xFF]; d[i + 2] = t[(w >> 16) & 0xFF]; d[i + 3] = t[w >> 24];
    }
  for (; i < n; i++) d[i] = t[e[i]];
  return d + n;
}
template <class G>
static VDP_NOINLINE u32a *bmRunOdd(u32a *d, const G g, const uint8_t *e, const uint8_t *o, int n, uint32_t &prev) {
  uint32_t p = prev;
  for (int i = 0; i < n; i++) { uint32_t c = g(e, o, i); d[i] = (p << 16) | (c >> 16); p = c; }
  prev = p;
  return d + n;
}
template <class G>
static inline void bmLine(u32a *d, const G g, const BmSrc &A, const BmSrc &B, int k0, bool odd) {
  BmSrc s1 = (k0 & 128) ? B : A, s2 = (k0 & 128) ? A : B;
  int a = k0 & 127;
  if (MSX2_LIKELY(!odd)) {
    d = bmRun(d, g, s1.e + a * G::kStep, s1.o + a * G::kStep, 128 - a);
    bmRun(d, g, s2.e, s2.o, a);
  } else {
    uint32_t p = g(s1.e, s1.o, a);
    d = bmRunOdd(d, g, s1.e + (a + 1) * G::kStep, s1.o + (a + 1) * G::kStep, 127 - a, p);
    bmRunOdd(d, g, s2.e, s2.o, a + 1, p);
  }
}

static void bmBuild(int m) {
  const uint8_t *pl = (const uint8_t *)plKey;
  uint32_t hi[16], lo[16];
  for (int i = 0; i < 16; i++) {
    if (m == MODE_G4) { hi[i] = LT.plW[i] & 0xFFFF0000u; lo[i] = LT.plW[i] & 0x0000FFFFu; }
    else { uint32_t a = pl[i >> 2], b = pl[i & 3]; hi[i] = (a << 16) | (b << 24); lo[i] = a | (b << 8); }
  }
  for (int h = 0; h < 16; h++)
    for (int l = 0; l < 16; l++) bmT[h * 16 + l] = hi[h] | lo[l];
  bmGen = plGen; bmMode = (uint32_t)m;
}
static void g6Build() {
  const uint8_t *pl = (const uint8_t *)plKey;
  u32a *t = (u32a *)g6T;
  for (int h = 0; h < 16; h++)
    for (int l = 0; l < 16; l += 2) t[(h * 16 + l) >> 1] = pl[h] | ((uint32_t)pl[l] << 8) | ((uint32_t)pl[h] << 16) | ((uint32_t)pl[l + 1] << 24);
  g6Gen = plGen;
}

static MSX2_DRAM uint8_t yjkC[96] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
};
template <bool Yae>
static VDP_NOINLINE void yjkWords(u32a *w, const uint8_t *e, const uint8_t *o) {
  const uint8_t *pl = (const uint8_t *)plKey, *cv = yjkC + 32;
  uint32_t sync = syncBits;
  for (int g = 0; g < 64; g++, w += 2) {
    uint32_t q0 = e[2 * g], q1 = o[2 * g], q2 = e[2 * g + 1], q3 = o[2 * g + 1];
    int k = (int)(((q0 & 7) | ((q1 & 7) << 3)) ^ 0x20) - 0x20;
    int j = (int)(((q2 & 7) | ((q3 & 7) << 3)) ^ 0x20) - 0x20;
    const uint8_t *cj = cv + j, *ck = cv + k;
    int c = -2 * j - k;
#define VDP_YJK_PX(b) ((Yae && ((b) & 0x08)) ? (uint32_t)pl[(b) >> 4]                                       \
                       : (sync | ((uint32_t)cv[(5 * (int)((b) >> 3) + c) >> 2] << 4) |                      \
                          ((uint32_t)ck[(b) >> 3] << 2) | cj[(b) >> 3]))
    uint32_t p0 = VDP_YJK_PX(q0), p1 = VDP_YJK_PX(q1), p2 = VDP_YJK_PX(q2), p3 = VDP_YJK_PX(q3);
#undef VDP_YJK_PX
    w[0] = (p1 | (p1 << 8)) | ((p0 | (p0 << 8)) << 16);
    w[1] = (p3 | (p3 << 8)) | ((p2 | (p2 << 8)) << 16);
  }
}
static inline void yjkPage(u32a *w, const uint8_t *e, const uint8_t *o, bool yae) {
  if (yae) yjkWords<true>(w, e, o); else yjkWords<false>(w, e, o);
}

static VDP_NOINLINE void bgBitmap(int m, u32a *d, int y) {
  bool g45 = (m == MODE_G4 || m == MODE_G5);
  uint32_t page = g45 ? ((uint32_t)(S.reg[2] & 0x60) << 10) : ((uint32_t)(S.reg[2] & 0x20) << 11);
  int hs = ((S.reg[26] & 0x3F) << 3) - (S.reg[27] & 7);
  bool sp2 = (S.reg[25] & 0x01) != 0;
  uint32_t pbit = g45 ? 0x8000 : 0x10000;
  uint32_t pa = sp2 ? (page & ~pbit) : page, pb = sp2 ? (page | pbit) : page;
  int hsm = hs & (sp2 ? 511 : 255);
  int k0 = hsm >> 1;
  bool odd = (hsm & 1) != 0;
  BmSrc A, B;
  if (g45) {
    A.e = &vram[(pa + ((uint32_t)y << 7)) & 0x1FFFF]; A.o = A.e;
    B.e = &vram[(pb + ((uint32_t)y << 7)) & 0x1FFFF]; B.o = B.e;
    if (bmGen != plGen || bmMode != (uint32_t)m) bmBuild(m);
    BmT32 g; g.t = bmT;
    bmLine(d, g, A, B, k0, odd);
    return;
  }
  uint32_t p = ((pa + ((uint32_t)y << 8)) >> 1) & 0xFFFF, q = ((pb + ((uint32_t)y << 8)) >> 1) & 0xFFFF;
  A.e = &vram[p]; A.o = &vram[0x10000 + p];
  B.e = &vram[q]; B.o = &vram[0x10000 + q];
  if (m == MODE_G6) {
    if (g6Gen != plGen) g6Build();
    BmT16 g; g.t = g6T;
    bmLine(d, g, A, B, k0, odd);
    return;
  }
  uint8_t r25 = S.reg[25];
  if (!(r25 & 0x08)) {
    if (g7Sync != (int)syncBits) {
      for (int i = 0; i < 256; i++) g7T[i] = (uint16_t)(g7Nat[i] * 0x0101u);
      g7Sync = syncBits;
    }
    uint8_t z = (S.reg[8] & 0x20) ? g7Nat[0] : g7Nat[S.reg[7]];
    g7T[0] = (uint16_t)(z * 0x0101u);
    BmT16 g; g.t = g7T;
    bmLine(d, g, A, B, k0, odd);
  } else {
    bool yae = (r25 & 0x10) != 0;
    if (hsm == 0) { yjkPage(d, A.e, A.o, yae); return; }
    u32a *w = yjkBuf();
    yjkPage(w, A.e, A.o, yae);
    BmSrc WA, WB;
    WA.e = WA.o = (const uint8_t *)w;
    WB = WA;
    if (sp2) { yjkPage(w + 128, B.e, B.o, yae); WB.e = WB.o = (const uint8_t *)(w + 128); }
    BmWord g;
    bmLine(d, g, WA, WB, k0, odd);
  }
}

#define SPR_MAX 32
static bool sprNoLimit = false;
void vdpSetSpriteNoLimit(bool on) { sprNoLimit = on; }
bool vdpSpriteNoLimit() { return sprNoLimit; }
static inline uint32_t sprBits(uint32_t bits, bool mag) {
  if (!mag) return bits << 16;
  bits = (bits | (bits << 8)) & 0x00FF00FFu;
  bits = (bits | (bits << 4)) & 0x0F0F0F0Fu;
  bits = (bits | (bits << 2)) & 0x33333333u;
  bits = (bits | (bits << 1)) & 0x55555555u;
  return bits | (bits << 1);
}
static inline uint32_t sprClip(uint32_t m, int sx, int &x0) {
  if (sx < 0) { x0 = 0; return (sx <= -32) ? 0 : (m << -sx); }
  x0 = sx;
  if (sx > 224) m &= ~(0xFFFFFFFFu >> (256 - sx));
  return m;
}
static inline uint32_t sprBlocks(int x0, int sx, int h) {
  int x1 = sx + h - 1; if (x1 > 255) x1 = 255;
  return ((2u << (x1 >> 3)) - 1) & ~((1u << (x0 >> 3)) - 1);
}

static VDP_NOINLINE VDP_HOT uint32_t sprDraw1(u16a *p, uint32_t m, uint32_t v) {
  uint32_t r = 0;
  for (; m; m <<= 1, p++) {
    if (!(m & 0x80000000u)) continue;
    if (*p) { r |= 0x20; continue; }
    *p = (uint16_t)v; r |= 1;
  }
  return r;
}
static VDP_NOINLINE VDP_HOT uint32_t sprDrawN(u16a *p, uint32_t m, uint32_t v, uint32_t cbit, bool draw) {
  uint32_t r = 0;
  for (; m; m <<= 1, p++) {
    if (!(m & 0x80000000u)) continue;
    if (*p) { r |= cbit; continue; }
    if (draw) { *p = (uint16_t)v; r |= 1; }
  }
  return r;
}
static VDP_NOINLINE VDP_HOT uint32_t sprDrawOr(u16a *p, uint32_t m, uint32_t v, uint32_t group, bool draw) {
  uint32_t r = 0, cb = v & 0xFF00;
  for (; m; m <<= 1, p++) {
    if (!(m & 0x80000000u)) continue;
    uint32_t o = *p;
    if (o) {
      if ((o & 0xFF) == group) *p = (uint16_t)(o | cb);
      continue;
    }
    if (draw) { *p = (uint16_t)v; r |= 1; }
  }
  return r;
}

static VDP_NOINLINE VDP_HOT bool spritesMode1(int y, uint32_t &used) {
  uint32_t sat = ((uint32_t)(S.reg[11] & 3) << 15) | ((uint32_t)S.reg[5] << 7);
  uint32_t pgen = (uint32_t)(S.reg[6] & 0x3F) << 11;
  bool big = (S.reg[1] & 2) != 0, mag = (S.reg[1] & 1) != 0;
  int h = (big ? 16 : 8) << (mag ? 1 : 0);
  u16a *sp = sprPix();
  bool any = false;
  int count = 0;
  uint32_t coll = 0, blk = 0;
  shSetup(sat, 128, pgen);
  const uint8_t *a = shA;
  const uint8_t *pg = shP;
  uint32_t y1 = (uint32_t)(y - 1);
  for (int i = 0; i < 32; i++, a += 4) {
    if (a[0] == 208) break;
    uint32_t dy = (y1 - a[0]) & 0xFF;
    if (dy >= (uint32_t)h) continue;
    if (++count > 4 && !sprNoLimit) {
      if (!(S.status[0] & 0x40)) S.status[0] = (S.status[0] & 0xA0) | 0x40 | i;
      break;
    }
#if MSX2_PROFILE
    P.sprVis++;
#endif
    int sx = a[1] - ((a[3] & 0x80) ? 32 : 0);
    uint32_t color = a[3] & 15;
    uint32_t pat = big ? (a[2] & 0xFC) : a[2];
    uint32_t pa = pat * 8 + (mag ? (dy >> 1) : dy);
    uint32_t bits = (uint32_t)pg[pa] << 8;
    if (big) bits |= pg[pa + 16];
    int x;
    uint32_t m = sprClip(sprBits(bits, mag), sx, x);
    if (!m) continue;
    blk |= sprBlocks(x, sx, h);
    uint32_t r = sprDraw1(sp + x, m, 1 | (color << 8));
    coll |= r;
    if ((r & 1) && color) any = true;
  }
  S.status[0] |= coll & 0x20;
  used = blk;
  return any;
}

static VDP_NOINLINE VDP_HOT uint32_t sprScan2(const uint8_t *ys, int st, uint32_t y1, uint32_t h, uint32_t *vis) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < 32; i++, ys += st) {
    uint32_t sy = *ys;
    if (sy == 216) break;
    uint32_t dy = (y1 - sy) & 0xFF;
    if (dy >= h) continue;
    if (n >= 8 && !sprNoLimit) return n | 0x80 | (i << 8);
    vis[n++] = i | (dy << 8);
  }
  return n;
}

static VDP_NOINLINE VDP_HOT bool spritesMode2(int y, bool pl, uint32_t &used) {
  uint32_t ctab = ((uint32_t)(S.reg[11] & 3) << 15) | ((uint32_t)(S.reg[5] & 0xF8) << 7);
  uint32_t sat = ctab + 0x200;
  uint32_t pgen = (uint32_t)(S.reg[6] & 0x3F) << 11;
  bool big = (S.reg[1] & 2) != 0, mag = (S.reg[1] & 1) != 0;
  int h = (big ? 16 : 8) << (mag ? 1 : 0);
  bool tp = (S.reg[8] & 0x20) != 0;
  uint32_t vis[SPR_MAX];
  const uint8_t *tabA = nullptr, *tabP = nullptr;
  if (!pl) { shSetup(ctab, 0x280, pgen); tabA = shA; tabP = shP; }
  const uint8_t *ys = pl ? &vram[sat >> 1] : tabA + 0x200;
  uint32_t r9 = sprScan2(ys, pl ? 2 : 4, (uint32_t)(y - 1), (uint32_t)h, vis);
  int n = (int)(r9 & 0x7F);
  if ((r9 & 0x80) && !(S.status[0] & 0x40)) S.status[0] = (uint8_t)((S.status[0] & 0xA0) | 0x40 | (r9 >> 8));
#if MSX2_PROFILE
  P.sprVis += n;
#endif
  if (!n) return false;

  u16a *sp = sprPix();
  uint32_t flags = 0, blk = 0;
  int group = 0;
  for (int k = 0; k < n; k++) {
    int i = vis[k] & 0xFF, row = (int)(vis[k] >> (mag ? 9 : 8));
    int sx;
    uint32_t pat, cc;
    if (tabA) {
      const uint8_t *e = tabA + 0x200 + i * 4;
      sx = e[1]; pat = e[2]; cc = tabA[i * 16 + row];
    } else {
      uint32_t a = sat + i * 4;
      sx = vr(a + 1, true); pat = vr(a + 2, true); cc = vr(ctab + i * 16 + row, true);
    }
    if (big) pat &= 0xFC;
    if (cc & 0x80) sx -= 32;
    bool orMode = (cc & 0x40) != 0;
    bool noColl = (cc & 0x20) != 0;
    uint32_t color = cc & 15;
    if (orMode) {
      if (!group) continue;
    } else {
      group = i + 1;
    }
    uint32_t bits;
    if (tabP) {
      uint32_t pa = pat * 8 + row;
      bits = (uint32_t)tabP[pa] << 8;
      if (big) bits |= tabP[pa + 16];
    } else {
      uint32_t pa = pgen + pat * 8 + row;
      bits = (uint32_t)vr(pa, true) << 8;
      if (big) bits |= vr(pa + 16, true);
    }
    int x;
    uint32_t m = sprClip(sprBits(bits, mag), sx, x);
    if (!m) continue;
    blk |= sprBlocks(x, sx, h);
    bool draw = color || tp;
    uint32_t v = group | (color << 8);
    uint32_t r = orMode ? sprDrawOr(sp + x, m, v, group, draw) : sprDrawN(sp + x, m, v, noColl ? 0 : 0x20, draw);
    flags |= r;
  }
  S.status[0] |= flags & 0x20;
  used = blk;
  return (flags & 1) != 0;
}

struct SprE {
  uint32_t m;
  uint8_t  x, col, draw, ic;
};
static MSX2_DRAM SprE sprE[SPR_MAX];

#if defined(__GNUC__)
static inline uint32_t sprClz(uint32_t v) { return (uint32_t)__builtin_clz(v); }
#else
static inline uint32_t sprClz(uint32_t v) { unsigned long i; _BitScanReverse(&i, v); return 31 - i; }
#endif

static VDP_NOINLINE VDP_HOT int sprPrep2(int y, bool pl) {
  uint32_t ctab = ((uint32_t)(S.reg[11] & 3) << 15) | ((uint32_t)(S.reg[5] & 0xF8) << 7);
  uint32_t sat = ctab + 0x200;
  uint32_t pgen = (uint32_t)(S.reg[6] & 0x3F) << 11;
  bool big = (S.reg[1] & 2) != 0, mag = (S.reg[1] & 1) != 0;
  int h = (big ? 16 : 8) << (mag ? 1 : 0);
  bool tp = (S.reg[8] & 0x20) != 0;
  uint32_t vis[SPR_MAX];
  const uint8_t *tabA = nullptr, *tabP = nullptr;
  if (!pl) { shSetup(ctab, 0x280, pgen); tabA = shA; tabP = shP; }
  const uint8_t *ys = pl ? &vram[sat >> 1] : tabA + 0x200;
  uint32_t r9 = sprScan2(ys, pl ? 2 : 4, (uint32_t)(y - 1), (uint32_t)h, vis);
  int n = (int)(r9 & 0x7F);
  if ((r9 & 0x80) && !(S.status[0] & 0x40)) S.status[0] = (uint8_t)((S.status[0] & 0xA0) | 0x40 | (r9 >> 8));
#if MSX2_PROFILE
  P.sprVis += n;
#endif
  int cnt = 0;
  bool anyDraw = false;
  for (int k = 0; k < n; k++) {
    int i = vis[k] & 0xFF, row = (int)(vis[k] >> (mag ? 9 : 8));
    int sx;
    uint32_t pat, cc, bits;
    if (tabA) {
      const uint8_t *e = tabA + 0x200 + i * 4;
      sx = e[1]; pat = e[2]; cc = tabA[i * 16 + row];
      if (big) pat &= 0xFC;
      uint32_t pa = pat * 8 + row;
      bits = (uint32_t)tabP[pa] << 8;
      if (big) bits |= tabP[pa + 16];
    } else {
      uint32_t a = sat + i * 4;
      sx = vr(a + 1, true); pat = vr(a + 2, true); cc = vr(ctab + i * 16 + row, true);
      if (big) pat &= 0xFC;
      uint32_t pa = pgen + pat * 8 + row;
      bits = (uint32_t)vr(pa, true) << 8;
      if (big) bits |= vr(pa + 16, true);
    }
    if (cc & 0x40) return -1;
    if (cc & 0x80) sx -= 32;
    int x;
    uint32_t m = sprClip(sprBits(bits, mag), sx, x);
    if (!m) continue;
    SprE &e = sprE[cnt++];
    e.m = m; e.x = (uint8_t)x; e.col = (uint8_t)(cc & 15);
    e.draw = (cc & 15) || tp;
    e.ic = (cc & 0x20) != 0;
    anyDraw |= e.draw != 0;
  }
  return anyDraw ? cnt : 0;
}

static inline void sprPut(u32a *o, uint32_t nw, uint32_t c32) {
  while (nw) {
    uint32_t z = sprClz(nw) & ~3u;
    uint32_t q = (nw << z) >> 28;
    u32a *p = o + (z >> 1);
    if (q == 15) {
      p[0] = c32; p[1] = c32;
    } else {
      uint32_t m0 = LT.nib[q][0], m1 = LT.nib[q][1];
      p[0] = (p[0] & ~m0) | (c32 & m0);
      p[1] = (p[1] & ~m1) | (c32 & m1);
    }
    nw &= ~(0xF0000000u >> z);
  }
}

static VDP_NOINLINE VDP_HOT void sprDraw2(u32a *d, int n) {
  uint32_t occ[9] = { 0 };
  uint32_t coll = 0;
  for (int k = 0; k < n; k++) {
    const SprE &e = sprE[k];
    uint32_t w = e.x >> 5, s = e.x & 31;
    uint32_t p0 = e.m >> s, p1 = s ? e.m << (32 - s) : 0;
    if (!e.ic) coll |= (p0 & occ[w]) | (p1 & occ[w + 1]);
    if (!e.draw) continue;
    uint32_t n0 = p0 & ~occ[w], n1 = p1 & ~occ[w + 1];
    occ[w] |= p0; occ[w + 1] |= p1;
    if (!d) continue;
    uint32_t c32 = sprW[e.col] | (sprW[e.col] << 16);
    if (n0) sprPut(d + w * 16, n0, c32);
    if (n1) sprPut(d + (w + 1) * 16, n1, c32);
  }
  if (coll) S.status[0] |= 0x20;
}

static VDP_NOINLINE VDP_HOT void sprFinish(u32a *d, uint32_t used, uint32_t cond) {
  u32a *s = (u32a *)sprPix();
  if (d) {
    const uint32_t *cw = sprW;
    uint32_t c2 = cond << 16;
    for (; used; s += 4, d += 4, used >>= 1) {
      if (!(used & 1)) continue;
      for (int i = 0; i < 4; i++) {
        uint32_t v = s[i];
        if (!v) continue;
        uint32_t o = d[i];
        if (v & cond) o = (o & 0x0000FFFFu) | (cw[(v >> 8) & 15] << 16);
        if (v & c2)   o = (o & 0xFFFF0000u) | cw[v >> 24];
        d[i] = o;
        s[i] = 0;
      }
    }
  } else {
    for (; used; s += 4, used >>= 1)
      if (used & 1) { s[0] = 0; s[1] = 0; s[2] = 0; s[3] = 0; }
  }
}
static void sprBuildW(uint32_t cls) {
  const uint8_t *pl = (const uint8_t *)plKey;
  for (int c = 0; c < 16; c++) {
    uint32_t v;
    if (cls == 2) v = g7SprNat[c] * 0x0101u;
    else if (cls == 1) v = pl[(c >> 2) & 3] | ((uint32_t)pl[c & 3] << 8);
    else v = pl[c] * 0x0101u;
    sprW[c] = v;
  }
  sprWGen = plGen; sprWMode = cls | ((uint32_t)syncBits << 8);
}

static void fillRow(int row, uint8_t c) {
  if (row >= 0 && row < MSX2_SCREEN_H) memset(outRows[row], c, MSX2_SCREEN_W);
}

static uint8_t borderColor(int m) {
  if (m == MODE_G7) return g7Nat[S.reg[7]];
  if (m == MODE_G5) return palNat[S.reg[7] & 3];
  return palNat[S.reg[7] & 15];
}

static VDP_NOINLINE void adjustLine(u32a *d, int dx, uint32_t bd4) {
  const int W = MSX2_SCREEN_W / 4;
  int q = (dx >= 0 ? dx : -dx) >> 1;
  bool odd = (dx & 1) != 0;
  auto at = [&](int i) -> uint32_t { return (i >= 0 && i < W) ? d[i] : bd4; };
  if (dx > 0) {
    for (int w = W - 1; w >= 0; w--)
      d[w] = odd ? ((at(w - q - 1) << 16) | (at(w - q) >> 16)) : at(w - q);
  } else {
    for (int w = 0; w < W; w++)
      d[w] = odd ? ((at(w + q) << 16) | (at(w + q + 1) >> 16)) : at(w + q);
  }
}

static void renderLine(int line, int row) {
  int m = curMode();
  uint8_t bd = borderColor(m);

  if (!(S.reg[1] & 0x40)) { fillRow(row, bd); return; }
#if MSX2_PROFILE
  P.lines++;
  P.modeLines[m & 31]++;
  if (S.reg[26] | S.reg[27]) P.hsLines++;
  if (S.reg[18]) P.adjLines++;
#endif
  plUpdate();
  int y = (line + S.reg[23]) & 0xFF;
  uint32_t bd4 = bd * 0x01010101u;
  u32a *out = (row >= 0 && row < MSX2_SCREEN_H) ? (u32a *)outRows[row] : 0;

  bool sprOn = m != MODE_T1 && m != MODE_T2 && !(S.reg[8] & 0x02);
  bool mode2 = (m != MODE_G1 && m != MODE_G2 && m != MODE_MC);
  bool sprites = false;
  uint32_t used = 0;
  int nSpr = 0;
  if (sprOn) {
#if MSX2_PROFILE
    uint32_t t0 = msx2ProfClock();
#endif
    if (!mode2) sprites = spritesMode1(y, used);
    else if ((nSpr = sprPrep2(y, planarMode(m))) < 0) { nSpr = 0; sprites = spritesMode2(y, planarMode(m), used); }
#if MSX2_PROFILE
    P.sprCycles += msx2ProfClock() - t0;
#endif
  }
  bool msk = (S.reg[25] & 0x02) != 0, adj = (S.reg[18] & 0x0F) != 0;
  u32a *d = (out && (sprites || nSpr || msk || adj)) ? (u32a *)lbOut : out;

  if (d) {
#if MSX2_PROFILE
    uint32_t t1 = msx2ProfClock();
#endif
    switch (m) {
      case MODE_T1: bgText1(d, y, bd4); break;
      case MODE_T2: bgText2(d, y, bd4); break;
      case MODE_G1: case MODE_G2: case MODE_G3: case MODE_MC: bgPattern(m, d, y); break;
      case MODE_G4: case MODE_G5: case MODE_G6: case MODE_G7: bgBitmap(m, d, y); break;
      default:
        for (int i = 0; i < MSX2_SCREEN_W / 4; i++) d[i] = bd4;
        break;
    }
#if MSX2_PROFILE
    P.outCycles += msx2ProfClock() - t1;
#endif
  }

  if (sprites || used || nSpr) {
#if MSX2_PROFILE
    uint32_t t0 = msx2ProfClock();
#endif
    if (sprites || nSpr) {
#if MSX2_PROFILE
      P.sprLines++;
#endif
      uint32_t cls = (m == MODE_G7) ? 2 : (m == MODE_G5) ? 1 : 0;
      if (sprWGen != plGen || sprWMode != (cls | ((uint32_t)syncBits << 8))) sprBuildW(cls);
      if (nSpr) sprDraw2(d, nSpr);
      else sprFinish(d, used, mode2 ? 0x00FF : 0xFF00);
    } else {
      sprFinish(0, used, 0);
    }
#if MSX2_PROFILE
    P.sprCycles += msx2ProfClock() - t0;
#endif
  }

  if (d && msk) d[0] = d[1] = d[2] = d[3] = bd4;

  if (d && adj) adjustLine(d, adjustOffset(S.reg[18] & 0x0F), bd4);

  if (d && d != out) memcpy(out, d, MSX2_SCREEN_W);
}

static void renderPending() {
  if (C.blk) cmdSyncTo(kRenderCycle);
  int l = pendLine;
  pendLine = -1;
  renderLine(l, pendRow);
}
static void vdpSyncSlow() {
  int now = lineCycleNow();
  if (pendLine >= 0 && now >= kRenderCycle) renderPending();
  if (C.blk) cmdSyncTo(now);
}

void vdpLine(int line) {
  if (pendLine >= 0) renderPending();
  if (C.blk) cmdSyncTo(MSX2_CYCLES_PER_LINE);
  C.syncCyc = 0;
  int h = vdpDisplayLines();
  if (line == 0) adjY = adjustOffset(S.reg[18] >> 4);
  int top = (MSX2_SCREEN_H - h) / 2 + adjY;

  if (line == 0) {
    vrFlag = false;
    int on = S.reg[13] >> 4, off = S.reg[13] & 15;
    if (on == 0) S.blinkOn = false;
    else if (off == 0) S.blinkOn = true;
    else if (--S.blinkCounter <= 0) {
      S.blinkOn = !S.blinkOn;
      S.blinkCounter = (S.blinkOn ? on : off) * 10;
    }
    if (top > 0) {
      int m = curMode();
      uint8_t bd = borderColor(m);
      for (int r = 0; r < top; r++) fillRow(r, bd);
    }
  }

  if (line < h) {
    pendLine = line;
    pendRow = line + top;
  } else if (line == h) {
    S.status[0] |= 0x80;
    vrFlag = true;
    if (h + top < MSX2_SCREEN_H) {
      uint8_t bd = borderColor(curMode());
      for (int r = h + top; r < MSX2_SCREEN_H; r++) fillRow(r, bd);
    }
  }

  fhMatch = (line - 1 == ((S.reg[19] - S.reg[23]) & 0xFF));
  if (fhMatch && (S.reg[0] & 0x10)) S.status[1] |= 0x01;
}

#ifndef ARDUINO
int vdpSpriteSelfTest(uint32_t seed, int iters, int *fastLines, int *collLines) {
  uint32_t r = seed ? seed : 1;
  auto rnd = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; };
  static const int modes[5][2] = { { MODE_G3, 0x04 }, { MODE_G4, 0x06 }, { MODE_G5, 0x08 }, { MODE_G6, 0x0A }, { MODE_G7, 0x0E } };
  alignas(4) static uint32_t A[128], B[128], bg[128];
  int fails = 0, fast = 0, coll = 0;
  VdpState save = S;
  for (int it = 0; it < iters; it++) {
    const int *md = modes[rnd() % 5];
    S.reg[0] = (uint8_t)md[1];
    S.reg[1] = (uint8_t)(0x40 | (rnd() & 3));
    S.reg[5] = (uint8_t)(rnd() | 0x07); S.reg[11] = (uint8_t)(rnd() & 3); S.reg[6] = (uint8_t)(rnd() & 0x3F);
    S.reg[8] = (uint8_t)(rnd() & 0x20);
    for (int i = 0; i < 16; i++) { S.palette[i] = (uint16_t)(rnd() & 0x777); updatePalNat(i); }
    for (uint32_t a = 0; a < VDP_VRAM_SIZE; a += 4) { uint32_t v = rnd(); memcpy(vram + a, &v, 4); }
    int m = md[0];
    bool pl = planarMode(m);
    uint32_t ctab = ((uint32_t)(S.reg[11] & 3) << 15) | ((uint32_t)(S.reg[5] & 0xF8) << 7), sat = ctab + 0x200;
    auto put = [&](uint32_t a, uint8_t v) { vram[pl ? planar(a & 0x1FFFF) : (a & 0x1FFFF)] = v; };
    int band = (int)(rnd() % 200), spread = 8 + (int)(rnd() % 64), ccRate = 1 + (int)(rnd() % 3) * 15;
    for (int i = 0; i < 32; i++) {
      put(sat + i * 4, (uint8_t)(rnd() % 97 == 0 ? 216 : (band + rnd() % spread) & 0xFF));
      if (rnd() % 3 == 0) put(sat + i * 4 + 1, (uint8_t)(rnd() % 40 + (rnd() & 1) * 216));
      for (int k = 0; k < 16; k++) {
        uint8_t c = (uint8_t)rnd();
        if (rnd() % ccRate) c &= ~0x40;
        put(ctab + i * 16 + k, c);
      }
    }
    shInvalidate();
    plUpdate();
    uint32_t cls = (m == MODE_G7) ? 2 : (m == MODE_G5) ? 1 : 0;
    sprBuildW(cls);
    for (int y = 0; y < 256; y++) {
      for (int i = 0; i < 128; i++) bg[i] = rnd();
      memcpy(A, bg, sizeof(A)); memcpy(B, bg, sizeof(B));
      uint32_t used = 0;
      S.status[0] = 0;
      bool spr = spritesMode2(y, pl, used);
      if (spr) sprFinish((u32a *)A, used, 0x00FF); else if (used) sprFinish(0, used, 0);
      uint8_t stA = S.status[0];
      S.status[0] = 0;
      int n = sprPrep2(y, pl);
      if (n < 0) {
        used = 0;
        spr = spritesMode2(y, pl, used);
        if (spr) sprFinish((u32a *)B, used, 0x00FF); else if (used) sprFinish(0, used, 0);
      } else if (n) {
        sprDraw2((u32a *)B, n);
        fast++;
      }
      uint8_t stB = S.status[0];
      if (stA & 0x20) coll++;
      if (memcmp(A, B, sizeof(A)) || stA != stB) fails++;
    }
  }
  S = save;
  shInvalidate();
  if (fastLines) *fastLines = fast;
  if (collLines) *collLines = coll;
  return fails;
}
#endif
