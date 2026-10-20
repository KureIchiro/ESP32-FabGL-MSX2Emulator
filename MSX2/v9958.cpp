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

// 画面モード (M5 M4 M3 M2 M1)
enum {
  MODE_G1 = 0x00, MODE_T1 = 0x01, MODE_MC = 0x02, MODE_G2 = 0x04, MODE_G3 = 0x08,
  MODE_T2 = 0x09, MODE_G4 = 0x0C, MODE_G5 = 0x10, MODE_G6 = 0x14, MODE_G7 = 0x1C
};

static VdpState S;
static uint8_t *vram;
static uint8_t **outRows;
static uint8_t syncBits;
static bool    vrFlag;               // S#2 VR (垂直帰線中)
static bool    fhMatch;              // いまのラインが R#19 の一致ライン(IE1=0 のときの S#1 FH)
static bool    is9938;               // V9938 として動く(ID=0、R#25〜27 なし)
static int     adjY;                 // R#18 の縦の位置調整(フレームの頭で決める。+ が下)

// ネイティブ色(同期ビット | BBGGRR)
static MSX2_DRAM uint8_t palNat[16];
static MSX2_DRAM uint8_t g7Nat[256];         // G7 の GRB332 → ネイティブ
static MSX2_DRAM uint8_t g7SprNat[16];       // G7 のスプライト固定色

// 1ラインぶんの作業領域(ネイティブ色、左から順)。横スクロール用に2ページぶん持つ。
alignas(4) static MSX2_DRAM uint8_t lb[1024];
alignas(4) static MSX2_DRAM uint8_t lb2[512];

static inline int  curMode() { return ((S.reg[1] >> 4) & 1) | ((S.reg[1] >> 2) & 2) | ((S.reg[0] << 1) & 0x1C); }
static inline bool planarMode(int m) { return m == MODE_G6 || m == MODE_G7; }
static inline bool v9938Mode(int m) { return (m & 0x18) != 0; }
static inline uint32_t planar(uint32_t a) { return ((a >> 1) | ((a & 1) << 16)) & 0x1FFFF; }

// ---------------------------------------------------------------------------
//  スプライトの表の写し(内部RAM)
//  VRAM は PSRAM にあり、スプライトの表(Y座標・色・パターン)を毎ライン読むと
//  キャッシュの入れ替えで遅い(Xak3 の16x16拡大スプライトでスプライトだけ1フレーム約8ms)。
//  表の範囲だけ内部RAMに写しを置き、VRAM に書く経路(CPU の書き込み・VDPコマンド)で一緒に直す。
//  G6/G7(2バンクのインターリーブ)のスプライトは写しを使わない(VRAM をそのまま読む)。
//  番地はすべて VRAM の物理番地。0x100000 は「写していない」。
// ---------------------------------------------------------------------------
static MSX2_DRAM uint8_t shA[0x280];      // モード2: 色表(512)+属性表(128)、モード1: 属性表(128)
static MSX2_DRAM uint8_t shP[0x800];      // パターン
static uint32_t shA0 = 0x100000, shALen = 0, shP0 = 0x100000;

static inline void shPoke(uint32_t a, uint8_t v) {
  if (a - shA0 < shALen) shA[a - shA0] = v;
  if (a - shP0 < 0x800u) shP[a - shP0] = v;
}
// VRAM の [lo, hi) を写しに反映する
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
// 写す範囲を決める(変わったら全部写し直す)
static inline void shSetup(uint32_t a0, uint32_t alen, uint32_t p0) {
  if (MSX2_LIKELY(a0 == shA0 && alen == shALen && p0 == shP0)) return;
  shA0 = a0; shALen = alen; shP0 = p0;
  memcpy(shA, vram + a0, alen);
  memcpy(shP, vram + p0, 0x800);
}
static inline void shInvalidate() { shA0 = shP0 = 0x100000; shALen = 0; }

#ifndef ARDUINO
// PC のテスト用: 写しと VRAM の違うバイト数
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

// ---------------------------------------------------------------------------
//  初期化
// ---------------------------------------------------------------------------
static const uint16_t kDefaultPalette[16] = {
  0x000, 0x000, 0x161, 0x373, 0x117, 0x237, 0x511, 0x267,
  0x711, 0x733, 0x661, 0x664, 0x141, 0x625, 0x555, 0x777
};
// G7 のスプライト色 (GRB 各3bit, 0xGRB)
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
  int  x, y, sxc, i, rows;         // 転送型コマンドの進行位置
  bool blk;                        // ブロック系/LINE/SRCH の実行中 (CE=1)
  int  bnx, bleft, rowCost, credit; // ブロック系: NX、残りの行数、1行の時間、たまった時間 (cmdBusy 参照)
  int  syncCyc;                    // このラインのどこまで時間を進めたか (Z80 のサイクル)
  uint8_t bclr;
  uint8_t regW;                    // 実行中に CPU が書いた SY(1) / DY(2) (cmdFinish 参照)
};
static VdpCmd C;

// ---- ラインを描く時刻 ----
// 表示ラインは、Z80 がそのラインを走るあいだ(1ライン = MSX2_CYCLES_PER_LINE)の表示期間のまんなか
// (kRenderCycle。表示期間はおよそ 43〜214)の状態で描く。vdpLine はラインの頭で描くラインを予約する
// だけで、CPU が VDP を書き換えるとき(レジスタ・パレット・VRAM・コマンドの進行)に kRenderCycle を
// 過ぎていれば、書き換える前に描く(vdpSync)。誰も書き換えなければ次の vdpLine の頭で描く。
// (ラインの頭で描くと、ライン割込みで画面を上下に分けるソフトの切り替えが実機より1ライン早く見え、
//  境目にスクロールしていないラインが出た。KG1/KG2.DSK(ｷｮｸｹﾞﾝ)のスコア表示の下)
static int pendLine = -1, pendRow;                    // 予約中の表示ライン(-1: なし)と出力行
static const int kRenderCycle = MSX2_CYCLES_PER_LINE * 9 / 16;   // 128
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

// R#18 の4ビットの値 → 画面を動かすドット数(ライン数)。+ が右(下)。
// 0111=-7(左/上) … 0001=-1, 0000=0, 1111=+1 … 1000=+8(右/下)
static inline int adjustOffset(int nib) { return nib < 8 ? -nib : 16 - nib; }

VdpState *vdpState() { return &S; }
uint8_t  *vdpVram()  { return vram; }
int vdpDisplayMode() { return curMode(); }
int vdpDisplayLines() { return (S.reg[9] & 0x80) ? 212 : 192; }

bool vdpIrq() {
  return ((S.status[0] & 0x80) && (S.reg[1] & 0x20)) ||
         ((S.status[1] & 0x01) && (S.reg[0] & 0x10));
}

// ---------------------------------------------------------------------------
//  コマンドエンジン (G4〜G7。V9958のCMDビット時や他モードでは G7 と同じ番地計算)
// ---------------------------------------------------------------------------
//  番地 = 行の先頭 ((y & cmYM) << 7) + 行内の番地。行内の番地は論理バイト番号 bx (G4/G5 は 0〜127、
//  G6/G7 は 0〜255) から決まり、G4/G5 は bx、G6/G7 は (bx >> 1) + (bx & 1) * 64KB (2バンク)。
//  ブロック系は1行ずつ、行内を番地の連続する区間にまとめて memset/memcpy や 32bit 単位で処理する。
//  結果は元の「1ドット(1バイト)ずつ順に」と同じ: 行どうしは元と同じ順に処理し、1つの行の中で
//  転送元と転送先が重なって順番で結果が変わりうるときだけ、元と同じ順に1ドット(1バイト)ずつ処理する。
static int cm;      // 0:G4 1:G5 2:G6 3:G7
static int cmW;     // 横ドット数
static int cmS;     // 1バイトのドット数の log2 (G4/G6:1 G5:2 G7:0)
static int cmBp;    // 1ドットのビット数の log2 (G4/G6:2 G5:1 G7:3)
static int cmPl;    // 1: G6/G7 (2バンクのインターリーブ)
static int cmYM;    // Y のマスク (G4/G5:1023 G6/G7:511)
static uint8_t  cmM;     // 1ドットのマスク
static uint32_t cmRep;   // 1ドットの値を32bit の全ドットに並べる係数

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
// モード別の定数 (M = cm)
template<int M> struct CmK {
  enum { YM = M < 2 ? 1023 : 511, RS = M < 2 ? 7 : 8, XM = (M == 1 || M == 2) ? 511 : 255, W = XM + 1,
         S = M == 1 ? 2 : (M == 3 ? 0 : 1), BP = 3 - S, PK = (1 << S) - 1, PL = M >= 2, MASK = (1 << (1 << BP)) - 1 };
};
template<int M> static inline uint32_t cmAddrT(int x, int y) {
  uint32_t la = ((uint32_t)(y & CmK<M>::YM) << CmK<M>::RS) | ((uint32_t)(x & CmK<M>::XM) >> CmK<M>::S);
  return CmK<M>::PL ? ((la >> 1) | ((la & 1) << 16)) : la;        // G6/G7 はインターリーブ
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
  if ((lop & 8) && c == 0) return;           // T付き論理演算: 0は書かない
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
// 1ドットずつの処理はモードで分けたもの(定数で計算する)を呼ぶ
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

// ---- ブロック系の下請け ----
#if defined(__GNUC__)
typedef uint32_t __attribute__((may_alias)) cmU32;
#define CM_NOINLINE __attribute__((noinline))   // writeReg に展開させない(ループの変数がスタックに溢れる)
#else
typedef uint32_t cmU32;
#define CM_NOINLINE
#endif
// 論理番地 → VRAM の番地 (G6/G7 はインターリーブ)。行内の論理バイト番号に使う
static inline uint32_t cmOff(uint32_t a) { return (a >> cmPl) | ((a & (uint32_t)cmPl) << 16); }
static inline int cmMin(int a, int b) { return a < b ? a : b; }
static inline uint32_t cmRow(int y) { return (uint32_t)(y & cmYM) << 7; }   // 行の先頭の番地

// 1行で処理するドット数(X が 0〜cmW-1 から出たらその行は終わり)
static inline int cmAvailP(int x, int dix) { return x >= cmW ? 0 : (dix > 0 ? cmW - x : x + 1); }
// 同じくバイト数 (HMMV/HMMM/YMMM: X は1バイトぶんずつ進む)
static inline int cmAvailB(int x, int dix) {
  if (x >= cmW) return 0;
  int q = x >> cmS;
  return dix > 0 ? (cmW >> cmS) - q : q + 1;
}

// 行内で番地が連続する区間。d=書き込み先、s=読み出し元、s2=読み出し元の次の論理バイト(行の先頭から)
struct CmRun { uint32_t d, s, s2; int n; };
// 書き込み先の論理バイト [b0, b1] (読み出し元はそれぞれ +db) を区間に分ける。G6/G7 は偶数と奇数の2つ
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
static inline void cmCopy(uint8_t *d, const uint8_t *s, int n) {    // 重ならないこと
  if (n >= 8) memcpy(d, s, n); else while (n-- > 0) *d++ = *s++;
}
// p[0..n) = (p & A) ^ X  (A/X は4バイトとも同じ値)
static void cmApply(uint8_t *p, int n, uint32_t A, uint32_t X) {
  for (; n > 0 && ((uintptr_t)p & 3); n--, p++) *p = (uint8_t)((*p & A) ^ X);
  cmU32 *w = (cmU32 *)p;
  for (; n >= 4; n -= 4, w++) *w = (*w & A) ^ X;
  for (p = (uint8_t *)w; n > 0; n--, p++) *p = (uint8_t)((*p & A) ^ X);
}

// 定数色 c の論理演算 (LMMV/LINE) を、ドットのビットごとに new = (old & A) ^ X の形にする。
// A/X は32bit の全ドットぶん並べた値。書き込まない場合 (lop 5〜7、T付きで c=0) は false
static bool cmLopAX(int lop, uint8_t c, uint32_t &A, uint32_t &X) {
  uint32_t m = cmM, a, x;
  c &= m;
  if ((lop & 8) && c == 0) return false;
  switch (lop & 7) {
    case 0:  a = 0;       x = c; break;           // IMP
    case 1:  a = c;       x = 0; break;           // AND
    case 2:  a = ~c & m;  x = c; break;           // OR  : (d & ~c) ^ c = d | c
    case 3:  a = m;       x = c; break;           // XOR
    case 4:  a = 0;       x = ~c & m; break;      // NOT
    default: return false;
  }
  A = a * cmRep; X = x * cmRep;
  return true;
}

// 0 でないドットのビットを全部1にしたマスク (T=1:4bit 2:2bit 3:8bit のドット)
template<int T> static inline uint32_t cmNzMask(uint32_t s) {
  if (T == 1) { uint32_t x = s | (s >> 2); x |= x >> 1; x &= 0x11111111u; return (x << 4) - x; }
  if (T == 2) { uint32_t x = (s | (s >> 1)) & 0x55555555u; return x | (x << 1); }
  uint32_t x = s | (s >> 4); x |= x >> 2; x |= x >> 1; x &= 0x01010101u; return (x << 8) - x;
}
// LMMM の論理演算をバイト(32bit)単位で。s=読み出し元 t=書き込み先。OP: 0 IMP 1 AND 2 OR 3 XOR 4 NOT
template<int OP, int T> static inline uint32_t cmLop(uint32_t s, uint32_t t) {
  uint32_t r = OP == 0 ? s : OP == 1 ? (s & t) : OP == 2 ? (s | t) : OP == 3 ? (s ^ t) : ~s;
  if (T) r = t ^ ((t ^ r) & cmNzMask<T>(s));      // T付き: 0 のドットは書かない
  return r;
}
// 書き込み先 d[0..n) に読み出し元 s[0..n) を論理演算で重ねる(全ビットがその区間のドット)
template<int OP, int T> static void cmLopRun(uint8_t *d, const uint8_t *s, int n) {
  for (; n > 0 && ((uintptr_t)d & 3); n--, d++, s++) *d = (uint8_t)cmLop<OP, T>(*s, *d);
  if (n >= 4) {
    cmU32 *w = (cmU32 *)d;
    int o = (int)((uintptr_t)s & 3);
    if (!o) {
      const cmU32 *v = (const cmU32 *)s;
      for (; n >= 4; n -= 4, w++, v++) *w = cmLop<OP, T>(*v, *w);
      s = (const uint8_t *)v;
    } else {                                     // 読み出し元の4バイト境界がずれている(リトルエンディアン)
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
static const CmRunFn kCmLopRun[5][4] = {        // [lop & 7][T付きのドット: 0=Tなし 1=4bit 2=2bit 3=8bit]
  { cmCopyRun,       cmLopRun<0, 1>, cmLopRun<0, 2>, cmLopRun<0, 3> },
  { cmLopRun<1, 0>,  cmLopRun<1, 1>, cmLopRun<1, 2>, cmLopRun<1, 3> },
  { cmLopRun<2, 0>,  cmLopRun<2, 0>, cmLopRun<2, 0>, cmLopRun<2, 0> },   // TOR = OR (0 を重ねても同じ)
  { cmLopRun<3, 0>,  cmLopRun<3, 0>, cmLopRun<3, 0>, cmLopRun<3, 0> },   // TXOR = XOR
  { cmLopRun<4, 0>,  cmLopRun<4, 1>, cmLopRun<4, 2>, cmLopRun<4, 3> },
};
// 1バイトぶんの論理演算 (lop & 7 は 0〜4)。行の両端のバイト用
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
// LMMM の1行の一部を1ドットずつ、元と同じ順で (M = cm、rs/rd は行の先頭の番地、lop & 7 は 0〜4)
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
// 書き込み先の論理バイト [*b0, *b1] (ドット xa〜xb) の両端が一部のドットだけなら、端として取り出す
static int cmEdges(int xa, int xb, int *b0, int *b1, int *eb, uint8_t *em) {
  int pk = (1 << cmS) - 1, ne = 0;
  uint32_t fm = 0xFFu >> ((xa & pk) << cmBp), lm = (0xFFu << ((pk - (xb & pk)) << cmBp)) & 0xFF;
  *b0 = xa >> cmS; *b1 = xb >> cmS;
  if (*b0 == *b1) fm &= lm, lm = 0xFF;
  if (fm != 0xFF) { eb[ne] = *b0; em[ne++] = (uint8_t)fm; ++*b0; }
  if (lm != 0xFF) { eb[ne] = *b1; em[ne++] = (uint8_t)lm; --*b1; }
  return ne;
}

alignas(4) static MSX2_DRAM uint8_t cmTmp[264];   // LMMM でずらした読み出し元の1行ぶん(区間ごとに4バイト境界から)

// t[i] = (pa[i] << sb) | (pb[i] >> (8 - sb))  (i = 0..n-1、sb = 1〜7)。t は4バイト境界。
// 32bit ずつ読む(pa/pb の4バイト境界のずれは2語をつなぐ)。読むのは pa/pb[0..n) を含む語だけ
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
  if (cnt > 0 && (lop & 7) <= 4) {                // lop 5〜7 は何も書かない
    int delta = C.sx - C.dx;
    int xa = dix > 0 ? C.dx : C.dx - cnt + 1, xb = xa + cnt - 1;   // 書き込み先(左から)
    bool same = ((C.dy - C.sy) & cmYM) == 0;      // 転送元と転送先が同じ行
    int ys = C.sy, yd = C.dy;
    if (same && delta != 0 && delta < cnt && -delta < cnt) {
      // 同じ行で重なる: 元と同じ順で1ドットずつ
      for (int j = 0; j < ny; j++, yd += diy) {
        uint32_t rw = cmRow(yd);
        cmLmmmPix(C.sx, C.dx, cnt, dix, rw, rw, lop);
      }
    } else if (!(same && delta == 0 && (lop & 7) <= 2)) {   // 自分自身への IMP/AND/OR は変化なし
      int s = cmS, pk = (1 << cmS) - 1;
      CmRunFn fn = kCmLopRun[lop & 7][(lop & 8) ? (cm == 1 ? 2 : (cm == 3 ? 3 : 1)) : 0];
      uint8_t *vr = vram;
      CmRun r[2];
      if (!(delta & pk)) {
        // 読み出し元と書き込み先でバイト内の位置が同じ: バイトどうしで処理する
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
        // バイト内の位置がずれる: 書き込み先の1バイトぶんのドットは、読み出し元の2バイト(論理)を
        // sb ビットずらしてつないだもの。全ドットが書き込み先のバイトは、ずらした1行を cmTmp に作ってから重ねる
        int D = delta * (1 << cmBp), jd = D >> 3, sb = D & 7;   // ビット単位のずれ (sb は 0 でない)
        int b0, b1, eb[2];
        uint8_t em[2];
        int ne = cmEdges(xa, xb, &b0, &b1, eb, em);
        uint32_t eo[2], ea[2], ec[2];
        for (int k = 0; k < ne; k++) {                 // 端のバイトは読み出し元の範囲外の2バイトも読むことがある
          eo[k] = cmOff(eb[k]);                        // (そのドットはマスクで捨てる。番地は VRAM 内に丸める)
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
    if (!same || db >= cnt || -db >= cnt) {      // 重ならない
      CmRun r[2];
      int nr = cmRuns(b0, b0 + cnt - 1, db, r);
      for (int j = 0; j < ny; j++, ys += diy, yd += diy) {
        const uint8_t *rs = vr + cmRow(ys);
        uint8_t *rd = vr + cmRow(yd);
        for (int k = 0; k < nr; k++) cmCopy(rd + r[k].d, rs + r[k].s, r[k].n);
      }
    } else if (db != 0) {                         // 同じ行で重なる
      if (!cmPl && db * dix > 0) {
        // 読み出し元が進む向きの先: どれも書き換わる前に読むので memmove と同じ
        for (int j = 0; j < ny; j++, yd += diy) {
          uint8_t *row = vr + cmRow(yd);
          memmove(row + b0, row + b0 + db, cnt);
        }
      } else {                                     // 元と同じ順で1バイトずつ
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
  if (cnt > 0 && ((C.dy - C.sy) & cmYM) != 0) {   // 同じ行どうしなら変化なし
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
  // 実機同様、終了後の SY/DY を書き戻す(続けて同じ向きに転送するソフト向け)。
  // ただし実行中に CPU が書いたレジスタは書き戻さない。実機の SY/DY は実行中もレジスタそのものが
  // 進むので、前のコマンドの値で CPU の書いた値が置き換わることはない(openMSX も同じ)。
  // (置き換えると、前のコマンドの終わりを待たずに次のコマンドの R#32〜 を書くソフトで、新しい SY が
  //  前のコマンドの終わりの SY になった。バサロアタッカーで POW ゲージが満タン(HMMM 72x4)のあとの
  //  弾の HMMM が SY=298(ゲージの絵)になり、弾がゲージの絵になって画面に残る)
  // 書いたあとに実機で残りの行のぶん +1 されるかは行の中の時刻しだいで、行単位の時間では決められないので足さない。
  if (!(C.regW & 1)) { S.reg[34] = C.sy & 0xFF; S.reg[35] = (C.sy >> 8) & 3; }
  if (!(C.regW & 2)) { S.reg[38] = C.dy & 0xFF; S.reg[39] = (C.dy >> 8) & 3; }
  C.regW = 0;
}

// ---- 転送型 (LMCM/LMMC/HMMC) ----
// 1単位(LMCM/LMMC は1ドット、HMMC は1バイト)ずつ CPU とやりとりする。モード別の処理をコマンド開始時に選んでおく
typedef void (*CmXferW)(uint8_t);
static int cmStepX;                   // 1単位で X が進むドット数(向き込み)
static CmXferW cmXferW;               // LMMC/HMMC: R#44 に書かれた1単位
static void (*cmXferR)();             // LMCM: S#7 を読んだあと次の1ドットへ

// 1単位を進める。終わったら false (M = cm)
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

// LINE の本体 (M = cm、MAJ = ARG の MAJ)。終点の Y を返す。
// 書かない演算 (lop 5〜7、T付きで色0) は A=全1 X=0 で呼ぶ(同じ値を書き戻すだけで、Y の終点は同じに求まる)
template<int M, int MAJ> CM_NOINLINE static int cmLineT(int x, int y, int nx, int ny, int dix, int diy,
                                                        uint32_t A, uint32_t X) {
  uint8_t *vr = vram;
  int asx = ((nx - 1) >> 1) & 1023;
  for (int n = nx;; n--) {               // 元の「n == nx で終わり」を残り数で数える
    uint32_t a = cmAddrT<M>(x, y), pm = (uint32_t)CmK<M>::MASK << cmShiftT<M>(x);
    vr[a] = (uint8_t)((vr[a] & (A | ~pm)) ^ (X & pm));
    shPoke(a, vr[a]);
    if (!MAJ) {                          // MAJ=0: 長辺がX
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

// SRCH の本体 (M = cm)。止まった X を返す
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

// ---- 実行時間 (MSX2_VDP_CMD_SPEED) ----
// 1単位(HMMV/HMMM/YMMM は1バイト、LMMV/LMMM/LINE/SRCH は1ドット)にかかる VDPクロック(21.48MHz)。
// 列は 画面OFF+スプライトON / 画面ON+スプライトON / 画面OFF+スプライトOFF / 画面ON+スプライトOFF (blueMSX と同じ値)
static int cmTiming(int op) {
  static const uint8_t t[7][4] = {
    { 92, 125,  92,  92 },   // SRCH
    {120, 147, 120, 132 },   // LINE
    { 98, 137,  98, 124 },   // LMMV
    {129, 197, 129, 132 },   // LMMM
    { 49,  65,  49,  62 },   // HMMV
    { 92, 136,  92,  97 },   // HMMM
    { 65, 125,  65,  68 },   // YMMM
  };
  int k = ((S.reg[1] >> 6) & 1) | (S.reg[8] & 2);       // BL(画面表示) / SPD(スプライト禁止)
  int r = op == 0x6 ? 0 : op == 0x7 ? 1 : op == 0x8 ? 2 : op == 0x9 ? 3 : op == 0xC ? 4 : op == 0xD ? 5 : 6;
  return t[r][k];
}
static const int kCmCreditPerLine = 1368 * MSX2_VDP_CMD_SPEED / 100;   // 1ラインの VDPクロック

// ブロック系コマンドが書いた行(y0 から diy 向きに n 行)を、スプライト表の写しに反映する
// (1行 = G4/G5 は128バイト、G6/G7 は両バンクに128バイトずつ)
static void cmdMarkRows(int y0, int n, int diy) {
  if (!shALen && shP0 >= 0x20000) return;                 // 写していない
  for (int k = 0; k < n; k++) {
    uint32_t lo = (uint32_t)((y0 + k * diy) & cmYM) << 7;
    shSync(lo, lo + 128);
    if (cmPl) shSync(lo | 0x10000, (lo | 0x10000) + 128);
  }
}

// いまのライン内の位置 (Z80 のサイクル、0〜MSX2_CYCLES_PER_LINE)
static inline int lineCycleNow() {
  int c = vdpLineCycle();
  return c < 0 ? 0 : (c > MSX2_CYCLES_PER_LINE ? MSX2_CYCLES_PER_LINE : c);
}

// 実行中にする。rows 行を1行 units 単位ずつ処理する(LINE/SRCH は rows=1 で処理は済んでいる)。
// 時間はライン内の位置で数える(C.credit の単位は VDPクロック × MSX2_CYCLES_PER_LINE)。
// ラインの頭だけでなく、CPU が VDP を触るたびにその位置まで進める(cmdSyncTo)ので、
// 終わる時刻(CE が 0 になり SY/DY が書き戻される時刻)がライン単位に丸められない。
// (ライン単位だと最大1ライン遅れ、CE を見ずに時間待ちで次のコマンドの R#32〜 を書くソフトで
//  前のコマンドの SY の書き戻しが新しい SY を上書きした。バサロアタッカーの得点表示)
static void cmdBusy(int units, int rows) {
  if (units < 1) units = 1;
  C.blk = true;
  C.bleft = rows;
  C.rowCost = units * cmTiming(C.op) * MSX2_CYCLES_PER_LINE;
  C.credit = 0;
  C.syncCyc = lineCycleNow();
}

// ライン内の位置 cyc まで時間を進め、たまった時間で終わる行まで処理する
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
      default: break;                           // LINE / SRCH: 時間を待つだけ
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
  C.blk = false;                               // 実行中のコマンドは打ち切り
  C.regW = 0;
#if MSX2_PROFILE
  switch (C.op) {                              // 名目ドット数(画面端での打ち切りは考えない)
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
    case 0x4:  // POINT
      S.status[7] = pget(C.sx, C.sy);
      return;
    case 0x5:  // PSET
      pset(C.dx, C.dy, clr, C.lop);
      return;
    case 0x6: {  // SRCH
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
    case 0x7:  // LINE
      cmdLine(clr, C.lop, arg);
      if (kCmCreditPerLine) cmdBusy((C.nx & 1023) + 1, 1);
      else cmdFinish();
      return;
    case 0x8:  // LMMV
      if (kCmCreditPerLine) { C.bclr = clr; C.bnx = nx; cmdBusy(cmMin(nx, cmAvailP(C.dx, C.dix)), ny); return; }
      { int y0 = C.dy; cmdLmmv(clr, nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0x9:  // LMMM
      if (kCmCreditPerLine) {
        C.bnx = nx;
        cmdBusy(cmMin(nx, cmMin(cmAvailP(C.sx, C.dix), cmAvailP(C.dx, C.dix))), ny);
        return;
      }
      { int y0 = C.dy; cmdLmmm(nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xA:  // LMCM (VRAM→CPU, S#7 で読み出す)
      C.active = true;
      C.x = C.sxc = C.sx; C.y = C.sy; C.i = 0; C.nx = nx; C.rows = ny;
      cmStepX = C.dix; cmXferR = kCmLmcm[cm];
      S.status[7] = pget(C.x, C.y);
      return;
    case 0xB:  // LMMC (CPU→VRAM, R#44 へ書く。1つめは今の CLR)
      C.active = true;
      C.x = C.sxc = C.dx; C.y = C.dy; C.i = 0; C.nx = nx; C.rows = ny;
      cmStepX = C.dix; cmXferW = kCmLmmc[cm];
      cmXferW(clr);
      return;
    case 0xC:  // HMMV
      if (kCmCreditPerLine) { C.bclr = clr; C.bnx = nx; cmdBusy(cmMin(nx >> cmS, cmAvailB(C.dx, C.dix)), ny); return; }
      { int y0 = C.dy; cmdHmmv(clr, nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xD:  // HMMM
      if (kCmCreditPerLine) {
        C.bnx = nx;
        cmdBusy(cmMin(nx >> cmS, cmMin(cmAvailB(C.sx, C.dix), cmAvailB(C.dx, C.dix))), ny);
        return;
      }
      { int y0 = C.dy; cmdHmmm(nx, ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xE:  // YMMM (X は DX から画面端まで)
      if (kCmCreditPerLine) { cmdBusy(cmAvailB(C.dx, C.dix), ny); return; }
      { int y0 = C.dy; cmdYmmm(ny); cmdMarkRows(y0, ny, C.diy); }
      cmdFinish();
      return;
    case 0xF: {  // HMMC (CPU→VRAM バイト単位)
      int ppb = cmPixPerByte(), n = nx / ppb; if (n <= 0) n = 1;
      C.active = true;
      C.x = C.sxc = C.dx; C.y = C.dy; C.i = 0; C.nx = n; C.rows = ny;
      cmStepX = C.dix * ppb; cmXferW = kCmHmmc[cm];
      cmXferW(clr);
      return;
    }
    default:   // STOP ほか
      return;
  }
}

static void cmdWriteData(uint8_t v) {          // R#44 への書き込み(LMMC/HMMC)
  if (!C.active) return;
#if MSX2_PROFILE
  if (C.op == 0xB || C.op == 0xF) P.cmdData++;
#endif
  if (C.op == 0xB || C.op == 0xF) cmXferW(v);   // cmdStart で選んだ LMMC/HMMC の処理
}

static uint8_t cmdReadData() {                 // S#7 の読み出し(LMCM)
  uint8_t v = S.status[7];
  if (C.active && C.op == 0xA) {
#if MSX2_PROFILE
    P.cmdData++;
#endif
    cmXferR();                                 // 次の1ドットを S#7 に用意する
  }
  return v;
}

// ---------------------------------------------------------------------------
//  レジスタ / ポート
// ---------------------------------------------------------------------------
#ifndef ARDUINO
void (*vdpRegHook)(int r, uint8_t v);                // PC のテスト用: レジスタへの書き込みを知らせる
#endif
static void writeReg(int r, uint8_t v) {
  if (r >= 47) return;
  if (is9938 && r >= 25 && r <= 27) return;          // V9938 には無いレジスタ(YJK・横スクロールなど)
#ifndef ARDUINO
  if (vdpRegHook) vdpRegHook(r, v);
#endif
  S.reg[r] = v;
  switch (r) {
    // IE1 を切ると走査線割込みの要求(FH)も消える(openMSX と同じ)。S#1 を読まずに
    // IE1 の OFF→ON で割込みを受け付けたことにするソフトがある(レイドック2)
    case 0: if (!(v & 0x10)) S.status[1] &= ~0x01; break;
    case 14: S.vramAddr = ((uint32_t)(v & 7) << 14) | (S.vramAddr & 0x3FFF); break;
    case 16: S.palLatchFull = false; break;
    case 34: case 35: if (C.active || C.blk) C.regW |= 1; break;   // 実行中に書いた SY / DY (cmdFinish 参照)
    case 38: case 39: if (C.active || C.blk) C.regW |= 2; break;
#if MSX2_PROFILE
    case 44:                                   // 時間は実行中の転送(HMMC/LMMC)だけ数える
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
  vdpSync();                                 // 予約中のラインを描き、コマンドをいまの位置まで進める
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
          S.status[0] &= 0x1F;               // F / 5S / C は読むと消える
          return v;
        }
        case 1: {
          uint8_t v = (S.status[1] & 0x01) | (is9938 ? 0x00 : 0x04);   // ID: V9938=0 / V9958=2
          S.status[1] &= ~0x01;              // FH は読むと消える
          // IE1=0 のときの FH は、一致したラインの右の枠から次のラインの左の枠までだけ 1 になる
          // (読んでも消えない。openMSX と同じ)。ここでは一致したラインの次のラインのあいだ 1 にする。
          // 割込みを止めて FH を読み続けてラインを待つソフトがある(Sonyc)
          if (!(S.reg[0] & 0x10) && fhMatch) v |= 0x01;
          return v;
        }
        case 2: {
          uint8_t v = 0x0C | (S.status[2] & 0x10);
          if (C.active) v |= 0x81;           // TR / CE
          else if (C.blk) v |= 0x01;         // CE
          if (vrFlag) v |= 0x40;
          if (vdpLineCycle() * 4 >= MSX2_CYCLES_PER_LINE * 3) v |= 0x20;   // HR
          return v;
        }
        case 3: case 5: return 0;
        case 4: return 0xFE;
        case 6: return 0xFC;
#if MSX2_PROFILE
        case 7:
          if (C.active) {                    // 時間は実行中の転送(LMCM)だけ数える
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
      vdpSync();                             // 書く前に、予約中のラインを描きコマンドを進める
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
        if (!(v & 0x40)) {                   // 読み出しの準備: 先読みしておく
          S.readAhead = vram[cpuVramAddr()];
          incAddr();
        }
      }
      return;
    case 2: {                                 // パレット
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
    case 3: {                                 // 間接レジスタ(R#17)
      vdpSync();
      int r = S.reg[17] & 0x3F;
      if (r != 17) writeReg(r, v);
      if (!(S.reg[17] & 0x80)) S.reg[17] = (S.reg[17] & 0xC0) | ((r + 1) & 0x3F);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
//  描画
// ---------------------------------------------------------------------------
// 背景は lb を通さず、ビューポート行 outRows[row] へ 32bit 単位で直接書く。
// 32bit語の中の並びは px2,px3,px0,px1 (FabGL の DMA 順)なので
//   256ドットのモード(横2倍): 1語 = 元の2ドット a,b → b | b<<8 | a<<16 | a<<24
//   512ドットのモード       : 1語 = 4ドット q0..q3  → q2 | q3<<8 | q0<<16 | q1<<24
// どちらも上位16bitが左半分、下位16bitが右半分。横スクロールで半語(256ドット系は1ドット、
// 512ドット系は2ドット)ずれるときは、隣り合う2語から (w[k] << 16) | (w[k+1] >> 16) で作る。
// スプライトは描いた画素だけを16bitで上書きする(256ドット座標 x の画素は行のバイト位置 (2x)^2)。
#if defined(__GNUC__)
typedef uint32_t __attribute__((may_alias)) u32a;    // 出力行などを 8/16/32bit で読み書きするため
typedef uint16_t __attribute__((may_alias)) u16a;
#define VDP_NOINLINE __attribute__((noinline))
#else
typedef uint32_t u32a;
typedef uint16_t u16a;
#define VDP_NOINLINE __declspec(noinline)
#endif
// いちばん内側のループ(スプライトの1枚、ビットマップの1ライン)に付ける。既定は IRAM(ESP32 で約0.9KB)。
// (フラッシュのキャッシュは PSRAM と共用で、VRAM を読むたびに描画コードが追い出される。
//  2026-10-08 から既定を IRAM にした。IRAM が足りなくなったら -DVDP_HOT= でフラッシュに戻せる)
// ※GCC 8 はテンプレート関数の section 属性を無視するので、patRun などはどちらでもフラッシュ。
#ifndef VDP_HOT
#define VDP_HOT MSX2_FAST
#endif

// lb は YJK の作業用(1ページ128語 × 2ページ)、lb2 はスプライトの画素状態 (16bit × 256) に使う。
// lbOut: スプライト・MSK・R#18 を重ねるラインはここで仕上げてから表示中の行へ写す
// (表示中の行に背景だけを書いた状態で VGA の走査が来ると、スプライトで隠すはずの絵が一瞬映る)
alignas(4) static MSX2_DRAM uint8_t lbOut[MSX2_SCREEN_W];
static inline u32a *yjkBuf() { return (u32a *)lb; }
static inline u16a *sprPix() { return (u16a *)lb2; }   // 下位8bit: 描いた印(モード1は1、モード2はグループ番号)
                                                        // 上位8bit: 色。合成のあと使ったブロックだけ0に戻す
// パターン描画の表(1つのベースレジスタで引けるように1つにまとめる)
struct LineTabs {
  uint32_t nib[16][2];                                  // 4ドット(上位ビットが左) → 2語ぶんのマスク
  uint32_t plW[16];                                     // pl[i] × 0x01010101 (pl はこのラインのパレット)
  uint32_t pair[4];                                     // 2ドット → 1語のマスク
  uint32_t quad[16];                                    // 512ドット系: 4ドット q0..q3 (bit3 = q0) → 1語のマスク
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
static MSX2_DRAM uint32_t plKey[4];                     // このラインの pl[16](TP=0 なら [0] は背景色)
static uint32_t plGen = 1;                              // plKey が変わるたびに増やす(下の表の作り直し判定)
alignas(4) static MSX2_DRAM uint32_t bmT[256];          // G4/G5: VRAM 1バイト → 出力1語
static uint32_t bmGen, bmMode;
alignas(4) static MSX2_DRAM uint16_t g6T[256];          // G6: VRAM 1バイト → 2ドット(下位バイトが左)
static uint32_t g6Gen;
alignas(4) static MSX2_DRAM uint16_t g7T[256];          // G7: g7Nat × 0x0101([0] はラインごとに色0の色)
static int g7Sync = -1;
static MSX2_DRAM uint32_t sprW[16];                     // スプライト色 → 出力16bit
static uint32_t sprWGen, sprWMode = ~0u;

static inline uint8_t vr(uint32_t a, bool pl) { return vram[pl ? planar(a & 0x1FFFF) : (a & 0x1FFFF)]; }

// このラインのパレット。変わったときだけ LT.plW を作り直す
static inline void plUpdate() {
  uint32_t k[4];
  for (int i = 0; i < 4; i++)
    k[i] = palNat[4 * i] | ((uint32_t)palNat[4 * i + 1] << 8) | ((uint32_t)palNat[4 * i + 2] << 16) |
           ((uint32_t)palNat[4 * i + 3] << 24);
  if (!(S.reg[8] & 0x20)) k[0] = (k[0] & 0xFFFFFF00u) | palNat[S.reg[7] & 15];   // TP=0: 色0は背景色
  if (MSX2_LIKELY(k[0] == plKey[0] && k[1] == plKey[1] && k[2] == plKey[2] && k[3] == plKey[3])) return;
  for (int i = 0; i < 4; i++) plKey[i] = k[i];
  const uint8_t *pl = (const uint8_t *)plKey;
  for (int i = 0; i < 16; i++) LT.plW[i] = pl[i] * 0x01010101u;
  plGen++;
}

// ---- 文字モード(横スクロール・スプライトなし) ----
static VDP_NOINLINE void bgText1(u32a *d, int y, uint32_t bd4) {
  const LineTabs *T = &LT;
  uint32_t name = (uint32_t)(S.reg[2] & 0x7F) << 10;
  const uint8_t *pt = &vram[((uint32_t)(S.reg[4] & 0x3F) << 11) | (y & 7)];
  uint32_t B = T->plW[S.reg[7] & 15], X = T->plW[S.reg[7] >> 4] ^ B;
  int n = (y >> 3) * 40;
  d[0] = d[1] = d[2] = d[3] = bd4;                      // 左右8ドットは枠
  d += 4;
  for (int c = 0; c < 40; c++, d += 3) {                // 1文字6ドット = 3語
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
  d[0] = d[1] = d[2] = d[3] = bd4;                      // 左右16ドットは枠
  d += 4;
  for (int c = 0; c < 40; c++, n += 2, d += 3) {        // 2文字(12ドット) = 3語
    uint32_t pa = pt[(uint32_t)vram[name | (n & 0xFFF)] << 3];
    uint32_t pb = pt[(uint32_t)vram[name | ((n + 1) & 0xFFF)] << 3];
    uint32_t Ba = B0, Xa = X0, Bb = B0, Xb = X0;
    if (blink) {                                        // n は偶数なので2文字の属性は同じバイト
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

// ---- パターンモード (G1/G2/G3/MC)。1文字8ドット = 4語。gen(ch) = 名前 ch の文字の4語 ----
// 色 cc と パターン p から4語を作る。表はバイトオフセットで引く(Xtensa でアドレス計算を1命令減らすため)
static inline void patWords(const LineTabs *T, uint32_t p, uint32_t cc,
                            uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) {
  const uint8_t *pw = (const uint8_t *)T->plW, *nb = (const uint8_t *)T->nib;
  uint32_t B = *(const uint32_t *)(pw + ((cc & 15) << 2));                 // 背景色(下位4bit)
  uint32_t X = *(const uint32_t *)(pw + ((cc >> 4) << 2)) ^ B;              // 前景色 ^ 背景色
  const uint32_t *a = (const uint32_t *)(nb + ((p >> 4) << 3));             // 左4ドット
  const uint32_t *b = (const uint32_t *)(nb + ((p & 15) << 3));             // 右4ドット
  w0 = B ^ (X & a[0]); w1 = B ^ (X & a[1]); w2 = B ^ (X & b[0]); w3 = B ^ (X & b[1]);
}
struct PatG1 {
  const uint8_t *pt, *ct;
  const LineTabs *T;
  inline void gen(uint32_t ch, uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) const {
    patWords(T, pt[ch << 3], ct[ch >> 3], w0, w1, w2, w3);
  }
};
struct PatG2 {                                          // G2/G3 (色テーブルのマスクあり)
  const uint8_t *pt, *ct;
  const LineTabs *T;
  uint32_t cm;
  inline void gen(uint32_t ch, uint32_t &w0, uint32_t &w1, uint32_t &w2, uint32_t &w3) const {
    ch <<= 3;
    patWords(T, pt[ch], ct[ch & cm], w0, w1, w2, w3);
  }
};
struct PatG3 {                                          // G2/G3 (R#3 の下位7bitが全部1: マスク不要)
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

// 名前 nm[0..n) の文字を順に4語ずつ出す(内側のループ。レジスタを使えるよう別関数にする)。
// Odd は半語(1ドット)ずらした語 (W[k-1] << 16) | (W[k] >> 16) を出す。prev = 直前の語
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
// k = 行の先頭に来る語の番号 (0..127)。先頭の文字の語 0..j0-1 は行末へ回す(スクロールの折り返し)
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
  o = patRun<G, Odd>(o, g, nm + c, n1, prev);             // 文字 c..31
  patRun<G, Odd>(o, g, nm, total - n1, prev);             // 折り返して 0..
}
template <class G>
static inline void patScroll(u32a *d, const G &g, const uint8_t *nm, int hs) {   // hs = 0..255 (左へずらすドット数)
  if (hs & 1) patLine<G, true>(d, g, nm, ((hs >> 1) + 1) & 127);
  else        patLine<G, false>(d, g, nm, hs >> 1);
}

static VDP_NOINLINE void bgPattern(int m, u32a *d, int y) {
  int hs = (((S.reg[26] & 0x3F) << 3) - (S.reg[27] & 7)) & 255;   // V9958 横スクロール
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
  } else {                                              // G2 / G3
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

// ---- ビットマップモード。1語 = VRAM の1バイト(G4/G5)または偶数/奇数バンクの1バイトずつ(G6/G7) ----
struct BmSrc { const uint8_t *e, *o; };
struct BmT32 {                                          // G4/G5
  enum { kStep = 1 };                                   // 1語あたりのソースのバイト数
  const uint32_t *t;
  inline uint32_t operator()(const uint8_t *e, const uint8_t *, int i) const { return t[e[i]]; }
};
struct BmT16 {                                          // G6/G7
  enum { kStep = 1 };
  const uint16_t *t;
  inline uint32_t operator()(const uint8_t *e, const uint8_t *o, int i) const { return t[o[i]] | ((uint32_t)t[e[i]] << 16); }
};
struct BmWord {                                         // YJK (作業用の語)
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
  if (!((uintptr_t)e & 3))                              // 4バイトずつ読む(リトルエンディアン)
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
// 仮想的な1ラインの語 0..127 は A、128..255 は B(SP2 のとき。そうでなければ A と同じ)。
// k0 番目の語から128語を出す(odd なら半語ずらす)。
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

static void bmBuild(int m) {                            // G4/G5 の表
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

// YJK: 4ドット(4バイト)ごとに色差 J,K を共有。1ページ128語を w に出す。
// 各成分の clamp(v, 0, 31) >> 3 は表で引く(v は R,G が Y+J/Y+K で -32〜62、B が (5Y-2J-K)>>2 で -24〜62)
static MSX2_DRAM uint8_t yjkC[96] = {                   // [v + 32]: v<8 → 0, 8〜15 → 1, 16〜23 → 2, 24以上 → 3
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
};
template <bool Yae>
static VDP_NOINLINE void yjkWords(u32a *w, const uint8_t *e, const uint8_t *o) {
  const uint8_t *pl = (const uint8_t *)plKey, *cv = yjkC + 32;   // cv[v] = clamp(v, 0, 31) >> 3
  uint32_t sync = syncBits;
  for (int g = 0; g < 64; g++, w += 2) {
    uint32_t q0 = e[2 * g], q1 = o[2 * g], q2 = e[2 * g + 1], q3 = o[2 * g + 1];
    int k = (int)(((q0 & 7) | ((q1 & 7) << 3)) ^ 0x20) - 0x20;       // 6bit 符号付き
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
  int hs = ((S.reg[26] & 0x3F) << 3) - (S.reg[27] & 7);  // V9958 横スクロール(256ドット単位)
  bool sp2 = (S.reg[25] & 0x01) != 0;                    // SP2: 2ページを横に並べてスクロール
  uint32_t pbit = g45 ? 0x8000 : 0x10000;
  uint32_t pa = sp2 ? (page & ~pbit) : page, pb = sp2 ? (page | pbit) : page;
  int hsm = hs & (sp2 ? 511 : 255);                      // 512ドット系も語単位では同じ(1語=2ドット分)
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
  uint8_t r25 = S.reg[25];                               // G7 (YJK を含む)
  if (!(r25 & 0x08)) {
    if (g7Sync != (int)syncBits) {
      for (int i = 0; i < 256; i++) g7T[i] = (uint16_t)(g7Nat[i] * 0x0101u);
      g7Sync = syncBits;
    }
    uint8_t z = (S.reg[8] & 0x20) ? g7Nat[0] : g7Nat[S.reg[7]];   // 色0の透明
    g7T[0] = (uint16_t)(z * 0x0101u);
    BmT16 g; g.t = g7T;
    bmLine(d, g, A, B, k0, odd);
  } else {
    bool yae = (r25 & 0x10) != 0;
    if (hsm == 0) { yjkPage(d, A.e, A.o, yae); return; }   // スクロールなし: 出力行へ直接
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

// ---- スプライト ----
// 1ラインのスプライトの枚数の制限(モード1 = 4枚、モード2 = 8枚)をなくす(Shift+F8。MSX1 版と同じ)。
// モード1の5枚目のステータス(S#0)は制限なしでも立てる。モード2の9枚目の検出は制限なしなら行わない。
#define SPR_MAX 32
static bool sprNoLimit = false;
void vdpSetSpriteNoLimit(bool on) { sprNoLimit = on; }
bool vdpSpriteNoLimit() { return sprNoLimit; }
// 結果は sprPix() に入れ、触った8ドットのブロックを used に返す(合成のあと sprFinish で0に戻す)。
static inline uint32_t sprBits(uint32_t bits, bool mag) {   // 16bit(上位が左) → 32bit(bit31 が左端)
  if (!mag) return bits << 16;
  bits = (bits | (bits << 8)) & 0x00FF00FFu;                // 拡大: 各ビットを2つにする
  bits = (bits | (bits << 4)) & 0x0F0F0F0Fu;
  bits = (bits | (bits << 2)) & 0x33333333u;
  bits = (bits | (bits << 1)) & 0x55555555u;
  return bits | (bits << 1);
}
static inline uint32_t sprClip(uint32_t m, int sx, int &x0) {   // 画面外(x<0, x>255)のドットを落とす
  if (sx < 0) { x0 = 0; return (sx <= -32) ? 0 : (m << -sx); }
  x0 = sx;
  if (sx > 224) m &= ~(0xFFFFFFFFu >> (256 - sx));
  return m;
}
static inline uint32_t sprBlocks(int x0, int sx, int h) {       // x0..(sx+h-1) を覆うブロック
  int x1 = sx + h - 1; if (x1 > 255) x1 = 255;
  return ((2u << (x1 >> 3)) - 1) & ~((1u << (x0 >> 3)) - 1);
}

// 1枚のスプライトの1ライン。p = sprPix の描き始め、m = ドットのマスク(bit31 が p[0])、v = 描く値。
// 戻り値: bit0 = 描いた画素がある、0x20 = 衝突
static VDP_NOINLINE VDP_HOT uint32_t sprDraw1(u16a *p, uint32_t m, uint32_t v) {      // モード1
  uint32_t r = 0;
  for (; m; m <<= 1, p++) {
    if (!(m & 0x80000000u)) continue;
    if (*p) { r |= 0x20; continue; }                     // 衝突(色0の画素も数える)
    *p = (uint16_t)v; r |= 1;
  }
  return r;
}
static VDP_NOINLINE VDP_HOT uint32_t sprDrawN(u16a *p, uint32_t m, uint32_t v, uint32_t cbit, bool draw) {   // モード2 CC=0
  uint32_t r = 0;
  for (; m; m <<= 1, p++) {
    if (!(m & 0x80000000u)) continue;
    if (*p) { r |= cbit; continue; }                     // 衝突(IC=1 なら cbit=0)
    if (draw) { *p = (uint16_t)v; r |= 1; }
  }
  return r;
}
static VDP_NOINLINE VDP_HOT uint32_t sprDrawOr(u16a *p, uint32_t m, uint32_t v, uint32_t group, bool draw) {  // モード2 CC=1
  uint32_t r = 0, cb = v & 0xFF00;
  for (; m; m <<= 1, p++) {
    if (!(m & 0x80000000u)) continue;
    uint32_t o = *p;
    if (o) {                                             // 同じグループの上なら色を OR
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
  shSetup(sat, 128, pgen);                               // 表は内部RAMの写しから読む(sat+127 は 0x1FFFF を越えない)
  const uint8_t *a = shA;
  const uint8_t *pg = shP;
  uint32_t y1 = (uint32_t)(y - 1);
  for (int i = 0; i < 32; i++, a += 4) {
    if (a[0] == 208) break;
    uint32_t dy = (y1 - a[0]) & 0xFF;
    if (dy >= (uint32_t)h) continue;
    if (++count > 4) {
      if (!(S.status[0] & 0x40)) S.status[0] = (S.status[0] & 0xA0) | 0x40 | i;
      if (!sprNoLimit) break;                             // 制限なし(Shift+F8)なら5枚目以降も描く
    }
#if MSX2_PROFILE
    P.sprVis++;
#endif
    int sx = a[1] - ((a[3] & 0x80) ? 32 : 0);
    uint32_t color = a[3] & 15;
    uint32_t pat = big ? (a[2] & 0xFC) : a[2];
    uint32_t pa = pat * 8 + (mag ? (dy >> 1) : dy);       // パターン表の中の位置(2KB を越えない)
    uint32_t bits = (uint32_t)pg[pa] << 8;
    if (big) bits |= pg[pa + 16];
    int x;
    uint32_t m = sprClip(sprBits(bits, mag), sx, x);
    if (!m) continue;
    blk |= sprBlocks(x, sx, h);
    uint32_t r = sprDraw1(sp + x, m, 1 | (color << 8));
    coll |= r;
    if ((r & 1) && color) any = true;                    // 色0の画素は描かない
  }
  S.status[0] |= coll & 0x20;
  used = blk;
  return any;
}

// モード2: このラインに出るスプライトを探す(Y=216 で終わり、制限ありなら9枚目で打ち切り)。
// vis[] = 番号 | (dy << 8)(SPR_MAX 個まで)。戻り値 = 枚数 | (制限ありで9枚目があればその番号 << 8 | 0x80)
static VDP_NOINLINE VDP_HOT uint32_t sprScan2(const uint8_t *ys, int st, uint32_t y1, uint32_t h, uint32_t *vis) {
  uint32_t n = 0, ninth = 0;
  for (uint32_t i = 0; i < 32; i++, ys += st) {
    uint32_t sy = *ys;
    if (sy == 216) break;
    uint32_t dy = (y1 - sy) & 0xFF;
    if (dy >= h) continue;
    if (n >= 8) {
      if (!sprNoLimit) {
        if (!ninth) ninth = 0x80 | (i << 8);
        return n | ninth;
      }
    }
    vis[n++] = i | (dy << 8);
  }
  return n | ninth;
}

static VDP_NOINLINE VDP_HOT bool spritesMode2(int y, bool pl, uint32_t &used) {
  uint32_t ctab = ((uint32_t)(S.reg[11] & 3) << 15) | ((uint32_t)(S.reg[5] & 0xF8) << 7);
  uint32_t sat = ctab + 0x200;
  uint32_t pgen = (uint32_t)(S.reg[6] & 0x3F) << 11;
  bool big = (S.reg[1] & 2) != 0, mag = (S.reg[1] & 1) != 0;
  int h = (big ? 16 : 8) << (mag ? 1 : 0);
  bool tp = (S.reg[8] & 0x20) != 0;
  uint32_t vis[SPR_MAX];                                  // 表示するスプライト: 番号 | (dy << 8)
  // G6/G7 以外は表を内部RAMの写しから読む(tabA = 色表+属性表、tabP = パターン)
  const uint8_t *tabA = nullptr, *tabP = nullptr;
  if (!pl) { shSetup(ctab, 0x280, pgen); tabA = shA; tabP = shP; }
  const uint8_t *ys = pl ? &vram[sat >> 1] : tabA + 0x200;  // Y座標(G6/G7 は偶数バンクに2バイトおき)
  uint32_t r9 = sprScan2(ys, pl ? 2 : 4, (uint32_t)(y - 1), (uint32_t)h, vis);
  int n = (int)(r9 & 0x7F);
  if ((r9 & 0x80) && !(S.status[0] & 0x40)) S.status[0] = (uint8_t)((S.status[0] & 0xA0) | 0x40 | (r9 >> 8));   // 9枚目
#if MSX2_PROFILE
  P.sprVis += n;
#endif
  if (!n) return false;

  u16a *sp = sprPix();
  uint32_t flags = 0, blk = 0;                            // flags: bit0 = 描いた、0x20 = 衝突
  int group = 0;                                          // 直前の CC=0 スプライト(+1)
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
    if (cc & 0x80) sx -= 32;                              // EC
    bool orMode = (cc & 0x40) != 0;                       // CC
    bool noColl = (cc & 0x20) != 0;                       // IC
    uint32_t color = cc & 15;
    if (orMode) {
      if (!group) continue;                               // 上位に CC=0 が無い CC=1 は表示されない
    } else {
      group = i + 1;
    }
    uint32_t bits;
    if (tabP) {
      uint32_t pa = pat * 8 + row;                        // パターン表の中の位置(2KB を越えない)
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

// ---- モード2の速い経路 ----
// 1ドットずつ sprPix に描いてから重ねる代わりに、どのドットが描かれたかを 32ドット単位の
// ビットマスク(占有)で持ち、背景を描いた行へ 4ドットずつマスク合成で直接書く。
// CC=1(色の OR)のスプライトがあるラインは今までの経路(spritesMode2 + sprFinish)で描く。
struct SprE {
  uint32_t m;                                             // ドットのマスク(bit31 が x)
  uint8_t  x, col, draw, ic;                              // 描き始め / 色 / 描く(色≠0 か TP=1) / IC
};
static MSX2_DRAM SprE sprE[SPR_MAX];

#if defined(__GNUC__)
static inline uint32_t sprClz(uint32_t v) { return (uint32_t)__builtin_clz(v); }   // ESP32 は NSAU 1命令
#else
static inline uint32_t sprClz(uint32_t v) { unsigned long i; _BitScanReverse(&i, v); return 31 - i; }
#endif

// このラインのスプライトを sprE に並べる。戻り値: 枚数(描くものが無ければ 0)、CC=1 があれば -1
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
  if ((r9 & 0x80) && !(S.status[0] & 0x40)) S.status[0] = (uint8_t)((S.status[0] & 0xA0) | 0x40 | (r9 >> 8));   // 9枚目
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
    if (cc & 0x40) return -1;                             // CC=1 は今までの経路で
    if (cc & 0x80) sx -= 32;                              // EC
    int x;
    uint32_t m = sprClip(sprBits(bits, mag), sx, x);
    if (!m) continue;
    SprE &e = sprE[cnt++];
    e.m = m; e.x = (uint8_t)x; e.col = (uint8_t)(cc & 15);
    e.draw = (cc & 15) || tp;
    e.ic = (cc & 0x20) != 0;
    anyDraw |= e.draw != 0;
  }
  return anyDraw ? cnt : 0;                               // 描くドットが無ければ衝突も起きない
}

// o = 出力行の32ドットぶん(16語)の先頭、nw = 描くドット(bit31 が左端)、c32 = 色(16bit × 2)
static inline void sprPut(u32a *o, uint32_t nw, uint32_t c32) {
  while (nw) {
    uint32_t z = sprClz(nw) & ~3u;                        // 4ドット単位の位置
    uint32_t q = (nw << z) >> 28;
    u32a *p = o + (z >> 1);
    if (q == 15) {                                        // 4ドットとも描く(よくある)
      p[0] = c32; p[1] = c32;
    } else {
      uint32_t m0 = LT.nib[q][0], m1 = LT.nib[q][1];
      p[0] = (p[0] & ~m0) | (c32 & m0);
      p[1] = (p[1] & ~m1) | (c32 & m1);
    }
    nw &= ~(0xF0000000u >> z);
  }
}

// sprE の n 枚を、背景を描いた行 d に優先順位の高い順で重ねる(d が NULL なら衝突だけ調べる)
static VDP_NOINLINE VDP_HOT void sprDraw2(u32a *d, int n) {
  uint32_t occ[9] = { 0 };                                // 描いたドット(9語目は画面の外)
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
    if (n1) sprPut(d + (w + 1) * 16, n1, c32);            // x>224 は sprClip で画面外を落としてある
  }
  if (coll) S.status[0] |= 0x20;
}

// スプライトを出力行に重ね(d が NULL なら重ねない)、使ったブロックの sprPix を0に戻す。
// sprPix の1語 = 画素 x (下位16bit) と x+1 (上位)。出力行の1語は x が上位16bit、x+1 が下位16bit。
// cond: 描く画素の条件(モード1は色≠0 → 0xFF00、モード2は印≠0 → 0x00FF)
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
static void sprBuildW(uint32_t cls) {                   // 0: pl[c]、1: G5 (2ドット)、2: G7 固定色
  const uint8_t *pl = (const uint8_t *)plKey;
  for (int c = 0; c < 16; c++) {
    uint32_t v;
    if (cls == 2) v = g7SprNat[c] * 0x0101u;
    else if (cls == 1) v = pl[(c >> 2) & 3] | ((uint32_t)pl[c & 3] << 8);
    else v = pl[c] * 0x0101u;
    sprW[c] = v;
  }
  sprWGen = plGen; sprWMode = cls | ((uint32_t)syncBits << 8);   // G7 の固定色 g7SprNat は syncBits で変わる
}

static void fillRow(int row, uint8_t c) {
  if (row >= 0 && row < MSX2_SCREEN_H) memset(outRows[row], c, MSX2_SCREEN_W);
}

static uint8_t borderColor(int m) {
  if (m == MODE_G7) return g7Nat[S.reg[7]];
  if (m == MODE_G5) return palNat[S.reg[7] & 3];
  return palNat[S.reg[7] & 15];
}

// 出力行を dx ドット(256ドット単位 = 2画素)ずらす。+ が右。空いたところは bd4 で埋める。
// 出力行の1語(32bit)には左から2画素ずつの組が2つ入り、上位16bitが左の組、下位16bitが右の組
// (px2,px3,px0,px1 の並び)。1ドット = この16bitの組1つなので、偶数ドットは語ごと、
// 奇数ドットは隣り合う2語の半分ずつを組み合わせてずらす。
static VDP_NOINLINE void adjustLine(u32a *d, int dx, uint32_t bd4) {
  const int W = MSX2_SCREEN_W / 4;                            // 128語
  int q = (dx >= 0 ? dx : -dx) >> 1;
  bool odd = (dx & 1) != 0;
  auto at = [&](int i) -> uint32_t { return (i >= 0 && i < W) ? d[i] : bd4; };
  if (dx > 0) {                                               // 右へ: 後ろから詰める(読む語は書く語より前)
    for (int w = W - 1; w >= 0; w--)
      d[w] = odd ? ((at(w - q - 1) << 16) | (at(w - q) >> 16)) : at(w - q);
  } else {                                                    // 左へ: 前から詰める(読む語は書く語より後ろ)
    for (int w = 0; w < W; w++)
      d[w] = odd ? ((at(w + q) << 16) | (at(w + q + 1) >> 16)) : at(w + q);
  }
}

static void renderLine(int line, int row) {
  int m = curMode();
  uint8_t bd = borderColor(m);

  if (!(S.reg[1] & 0x40)) { fillRow(row, bd); return; }      // BL=0: 表示オフ
#if MSX2_PROFILE
  P.lines++;
  P.modeLines[m & 31]++;
  if (S.reg[26] | S.reg[27]) P.hsLines++;                    // V9958 横スクロールが効いているライン
  if (S.reg[18]) P.adjLines++;                               // R#18(画面位置調整)が 0 でないライン
#endif
  plUpdate();
  int y = (line + S.reg[23]) & 0xFF;                          // 縦スクロール
  uint32_t bd4 = bd * 0x01010101u;
  u32a *out = (row >= 0 && row < MSX2_SCREEN_H) ? (u32a *)outRows[row] : 0;

  // スプライトは背景より先に調べておく(背景のあとは重ねるだけにする)。
  // モード2は sprE に並べて背景の上へ直接描く(nSpr 枚)。モード1と CC=1 のあるラインは sprPix に描いて sprFinish で重ねる
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
  // 重ねるものがあるラインは lbOut で仕上げてから写す。無いラインは表示中の行へ直接書く
  bool msk = (S.reg[25] & 0x02) != 0, adj = (S.reg[18] & 0x0F) != 0;
  u32a *d = (out && (sprites || nSpr || msk || adj)) ? (u32a *)lbOut : out;

  // 背景。outCycles はこの背景+出力の時間(以前の out256/out512 は背景生成と一体になった)
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

  // スプライトを重ねる
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

  // V9958 MSK: 左8ドット(512ドット系は16ドット)を枠の色で隠す
  if (d && msk) d[0] = d[1] = d[2] = d[3] = bd4;

  // R#18 横の位置調整: ライン全体を左右にずらし、空いたところは枠の色
  if (d && adj) adjustLine(d, adjustOffset(S.reg[18] & 0x0F), bd4);

  if (d && d != out) memcpy(out, d, MSX2_SCREEN_W);           // 仕上がったラインを表示中の行へ
}

// ---------------------------------------------------------------------------
//  ライン処理
// ---------------------------------------------------------------------------
// 予約中のラインを描く(コマンドは描く時刻 kRenderCycle まで進めてから)
static void renderPending() {
  if (C.blk) cmdSyncTo(kRenderCycle);
  int l = pendLine;
  pendLine = -1;
  renderLine(l, pendRow);
}
// CPU が VDP を触るとき: kRenderCycle を過ぎていれば予約中のラインを描き、コマンドをいまの位置まで進める
static void vdpSyncSlow() {
  int now = lineCycleNow();
  if (pendLine >= 0 && now >= kRenderCycle) renderPending();
  if (C.blk) cmdSyncTo(now);
}

void vdpLine(int line) {
  // 前のラインの残り: 予約中のラインを描き、VDPコマンドをラインの終わりまで進める
  if (pendLine >= 0) renderPending();
  if (C.blk) cmdSyncTo(MSX2_CYCLES_PER_LINE);
  C.syncCyc = 0;
  int h = vdpDisplayLines();
  if (line == 0) adjY = adjustOffset(S.reg[18] >> 4);         // R#18 縦の位置調整はフレーム単位で効かせる
  int top = (MSX2_SCREEN_H - h) / 2 + adjY;                    // 192ライン時は上下10ライン枠(+縦の位置調整)

  if (line == 0) {
    vrFlag = false;
    // TEXT2 の点滅(R#13 上位=点灯時間、下位=消灯時間、単位は約10フレーム)
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
    pendLine = line;                                           // 表示期間のまんなかで描く(上の説明)
    pendRow = line + top;
  } else if (line == h) {
    S.status[0] |= 0x80;                                       // 垂直帰線割込み
    vrFlag = true;
    if (h + top < MSX2_SCREEN_H) {                             // 下の枠(上へずらしたときに空いた行も)
      uint8_t bd = borderColor(curMode());
      for (int r = h + top; r < MSX2_SCREEN_H; r++) fillRow(r, bd);
    }
  }

  // 走査線割込み(R#19、縦スクロール R#23 を考慮)。実機は一致したラインの表示が終わったところ
  // (右の枠)で FH が立つので、次のラインの頭で立てる(一致したラインの頭で立てると約1ライン早い)
  fhMatch = (line - 1 == ((S.reg[19] - S.reg[23]) & 0xFF));
  if (fhMatch && (S.reg[0] & 0x10)) S.status[1] |= 0x01;
}

#ifndef ARDUINO
// PC のテスト用: 乱数で作ったスプライト表で、モード2の速い経路(sprPrep2 + sprDraw2)と
// 今までの経路(spritesMode2 + sprFinish)の出力行と S#0 を全ラインで比べる。戻り値 = 食い違ったライン数
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
    S.reg[1] = (uint8_t)(0x40 | (rnd() & 3));                         // BL=1、SI/MAG は乱数
    S.reg[5] = (uint8_t)(rnd() | 0x07); S.reg[11] = (uint8_t)(rnd() & 3); S.reg[6] = (uint8_t)(rnd() & 0x3F);
    S.reg[8] = (uint8_t)(rnd() & 0x20);                               // TP は乱数、SPD=0
    for (int i = 0; i < 16; i++) { S.palette[i] = (uint16_t)(rnd() & 0x777); updatePalNat(i); }
    for (uint32_t a = 0; a < VDP_VRAM_SIZE; a += 4) { uint32_t v = rnd(); memcpy(vram + a, &v, 4); }
    int m = md[0];
    bool pl = planarMode(m);
    uint32_t ctab = ((uint32_t)(S.reg[11] & 3) << 15) | ((uint32_t)(S.reg[5] & 0xF8) << 7), sat = ctab + 0x200;
    auto put = [&](uint32_t a, uint8_t v) { vram[pl ? planar(a & 0x1FFFF) : (a & 0x1FFFF)] = v; };
    int band = (int)(rnd() % 200), spread = 8 + (int)(rnd() % 64), ccRate = 1 + (int)(rnd() % 3) * 15;
    for (int i = 0; i < 32; i++) {
      put(sat + i * 4, (uint8_t)(rnd() % 97 == 0 ? 216 : (band + rnd() % spread) & 0xFF));   // Y: たくさん重なるように集める
      if (rnd() % 3 == 0) put(sat + i * 4 + 1, (uint8_t)(rnd() % 40 + (rnd() & 1) * 216)); // X: 端に寄せて切り落としも試す
      for (int k = 0; k < 16; k++) {                                   // 色: CC=1 は少なめ
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
