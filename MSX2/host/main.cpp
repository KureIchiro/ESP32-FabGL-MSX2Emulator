// ============================================================================
//  MSX2+ コアの PC 用テストハーネス
//    msx2host selftest <outdir>            VDP の各モードを描かせて PNG に出す
//    msx2host boot <romdir> <frames> <outdir> [every]
//                                          ROM で起動し、every フレームごとに PNG に出す
// ============================================================================
#include "../msx2.h"
#include "../v9958.h"
#include "../wd2793.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "../Z80.h"
extern "C" Z80 msx2_cpu;
#if MSX2_PROFILE
#include <chrono>
// msx2ProfClock()(PCでは TSC)の 1us あたりのカウントを steady_clock と比べて求める
static uint32_t profClockPerUs() {
  using namespace std::chrono;
  auto t0 = steady_clock::now();
  uint32_t c0 = msx2ProfClock();
  for (;;) {
    auto t = steady_clock::now();
    uint32_t c = msx2ProfClock();
    double us = duration<double, std::micro>(t - t0).count();
    if (us >= 50000) return (uint32_t)((c - c0) / us + 0.5);
  }
}
#endif

// ---- 命令トレース (Z80.c を DEBUG 付きでビルドしたときだけ呼ばれる) ----
struct TraceEnt { uint16_t pc, sp, af, hl, bc, de; uint8_t psr, ssr3, op, r15; };
static TraceEnt traceBuf[1 << 20];
static unsigned traceN;
static bool traceDumped;
static void dumpTrace(const char *why, int count) {
  printf("---- trace (%s), last %d ----\n", why, count);
  for (int i = count; i > 0; i--) {
    const TraceEnt &t = traceBuf[(traceN - i) & ((1 << 20) - 1)];
    printf("PC=%04X op=%02X SP=%04X AF=%04X HL=%04X BC=%04X DE=%04X A8=%02X FFFF(3)=%02X R15=%X\n", t.pc, t.op, t.sp, t.af, t.hl, t.bc, t.de, t.psr, t.ssr3, t.r15);
  }
}
static FILE *sndLog();
int vdpShadowMismatch();   // v9958.cpp (PC only): sprite-table shadow vs VRAM
extern "C" byte DebugZ80(Z80 *R) {
  TraceEnt &t = traceBuf[traceN++ & ((1 << 20) - 1)];
  t.pc = R->PC.W; t.sp = R->SPtr.W; t.af = R->AF.W; t.hl = R->HL.W; t.bc = R->BC.W; t.de = R->DE.W; t.r15 = vdpState()->reg[15];
  t.psr = msx2PrimarySlots(); t.ssr3 = msx2SubSlots(3); t.op = RdZ80(R->PC.W);
  if (t.pc == 0x0038) if (FILE *f = sndLog()) {
    VdpState *v = vdpState();
    fprintf(f, "%u c%d I R0=%02X R1=%02X R19=%02X R23=%02X S0=%02X S1=%02X SP=%04X RET=%04X\n", msx2LineClock(), vdpLineCycle(), v->reg[0],
            v->reg[1], v->reg[19], v->reg[23], v->status[0], v->status[1], R->SPtr.W,
            RdZ80(R->SPtr.W) | (RdZ80((uint16_t)(R->SPtr.W + 1)) << 8));
  }
  static int trapPc = getenv("TRAP") ? (int)strtol(getenv("TRAP"), nullptr, 16) : 0x0038;
  // TRAPSKIP=n: n回目までの到達は無視する / TRAPN=n: 表示する命令数(最大1M)
  static int trapSkip = getenv("TRAPSKIP") ? atoi(getenv("TRAPSKIP")) : 0;
  static int trapN = getenv("TRAPN") ? atoi(getenv("TRAPN")) : 40;
  // TRAPFRAME=n: n フレーム目より前の到達は数えない
  static uint32_t trapLine = getenv("TRAPFRAME") ? (uint32_t)atoi(getenv("TRAPFRAME")) * MSX2_LINES_PER_FRAME : 0;
  // SPWATCH=lo-hi (16進): SP が初めてこの範囲に入ったところでトレースを出す
  static int spLo = -1, spHi = -1;
  static bool spInit = false;
  if (!spInit) {
    spInit = true;
    if (const char *w = getenv("SPWATCH")) sscanf(w, "%x-%x", &spLo, &spHi);
  }
  if (!traceDumped && spLo >= 0 && t.sp >= spLo && t.sp <= spHi) {
    traceDumped = true;
    dumpTrace("SP entered SPWATCH range", trapN > (1 << 20) ? (1 << 20) : trapN);
  }
  // TRAPA8=xx: スロット選択(A8h)がこの値のときだけ数える
  static int trapA8 = getenv("TRAPA8") ? (int)strtol(getenv("TRAPA8"), nullptr, 16) : -1;
  if (!traceDumped && t.pc == trapPc && msx2LineClock() >= trapLine && (trapA8 < 0 || t.psr == trapA8) &&
      trapSkip-- <= 0) {
    traceDumped = true;
    dumpTrace("arrival at TRAP", trapN > (1 << 20) ? (1 << 20) : trapN);
  }
  return 1;
}

// ---- フック ----
// SNDLOG=<file>: PSG/OPLL の書き込みと 0038h への到達(割込み)を「ライン番号 種類 レジスタ 値」で書き出す
extern void (*vdpRegHook)(int r, uint8_t v);
int vdpSpriteSelfTest(uint32_t seed, int iters, int *fastLines, int *collLines);
static FILE *sndLog() {
  static FILE *f = getenv("SNDLOG") ? fopen(getenv("SNDLOG"), "w") : nullptr;
  return f;
}
void    msx2PsgWrite(int r, uint8_t v) { if (FILE *f = sndLog()) fprintf(f, "%u P %X %02X\n", msx2LineClock(), r, v); }
static int g_fmWrites;                               // OPLL への書き込み回数(FM-PAC の確認用)
void    msx2FmWrite(int port, uint8_t v) {
  static uint8_t addr;
  g_fmWrites++;
  if (!(port & 1)) { addr = v; return; }
  if (FILE *f = sndLog()) fprintf(f, "%u F %02X %02X\n", msx2LineClock(), addr, v);
}
void    msx2SccWrite(uint16_t, uint8_t) {}

static uint8_t *vram;
static uint8_t *rows[MSX2_SCREEN_H];
static const uint8_t kSync = 0xC0;

// ---------------------------------------------------------------------------
//  PNG (無圧縮 deflate)
// ---------------------------------------------------------------------------
static uint32_t crcTable[256];
static void crcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
static void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> b;
  put32(b, (uint32_t)data.size());
  b.insert(b.end(), type, type + 4);
  b.insert(b.end(), data.begin(), data.end());
  uint32_t c = crc(b.data() + 4, b.size() - 4) ^ 0xFFFFFFFFu;
  put32(b, c);
  fwrite(b.data(), 1, b.size(), f);
}
static bool writePng(const char *path, int w, int h, const uint8_t *rgb) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, w); put32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
  }
  std::vector<uint8_t> z;
  z.push_back(0x78); z.push_back(0x01);
  size_t pos = 0;
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  while (pos < raw.size()) {
    size_t n = raw.size() - pos; if (n > 65535) n = 65535;
    z.push_back(pos + n >= raw.size() ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8);
    z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  put32(z, (b << 16) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", std::vector<uint8_t>());
  fclose(f);
  return true;
}

// ビューポート(FabGL の並び px2,px3,px0,px1)を RGB に戻して 512x424 で保存
static void dumpScreen(const std::string &path) {
  const int W = MSX2_SCREEN_W, H = MSX2_SCREEN_H;
  std::vector<uint8_t> rgb((size_t)W * H * 2 * 3);
  static const int order[4] = { 2, 3, 0, 1 };
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint8_t v = rows[y][(x & ~3) + order[x & 3]];
      uint8_t r = (v & 3) * 85, g = ((v >> 2) & 3) * 85, bl = ((v >> 4) & 3) * 85;
      for (int k = 0; k < 2; k++) {
        uint8_t *p = &rgb[(((size_t)(y * 2 + k) * W) + x) * 3];
        p[0] = r; p[1] = g; p[2] = bl;
      }
    }
  }
  if (writePng(path.c_str(), W, H * 2, rgb.data())) printf("wrote %s\n", path.c_str());
}

// ---------------------------------------------------------------------------
//  VDP をポート経由で操作するヘルパ
// ---------------------------------------------------------------------------
static void wreg(int r, uint8_t v) { vdpWritePort(1, v); vdpWritePort(1, 0x80 | r); }
static void setWriteAddr(uint32_t a) {
  wreg(14, (a >> 14) & 7);
  vdpWritePort(1, a & 0xFF);
  vdpWritePort(1, 0x40 | ((a >> 8) & 0x3F));
}
static void vpoke(uint32_t a, uint8_t v) { setWriteAddr(a); vdpWritePort(0, v); }
static void setPalette(int i, int r, int g, int b) {
  wreg(16, i);
  vdpWritePort(2, (r << 4) | b);
  vdpWritePort(2, g);
}
static void cmd(int sx, int sy, int dx, int dy, int nx, int ny, uint8_t clr, uint8_t arg, uint8_t op) {
  wreg(17, 32);                                   // R#32 から自動インクリメント
  uint8_t v[15] = { (uint8_t)sx, (uint8_t)(sx >> 8), (uint8_t)sy, (uint8_t)(sy >> 8),
                    (uint8_t)dx, (uint8_t)(dx >> 8), (uint8_t)dy, (uint8_t)(dy >> 8),
                    (uint8_t)nx, (uint8_t)(nx >> 8), (uint8_t)ny, (uint8_t)(ny >> 8),
                    clr, arg, op };
  for (int i = 0; i < 15; i++) vdpWritePort(3, v[i]);
}
static void clearVdp() { vdpReset(); memset(vram, 0, VDP_VRAM_SIZE); }
static void renderFrame() { for (int l = 0; l < MSX2_LINES_PER_FRAME; l++) vdpLine(l); }

// SCREEN 5 相当(G4, 212ライン)
static void screen5() {
  wreg(0, 0x06); wreg(1, 0x40); wreg(2, 0x1F); wreg(5, 0xEF); wreg(11, 0);
  wreg(6, 0x0F); wreg(7, 0x04); wreg(8, 0x08); wreg(9, 0x80);
}

static void testScreen5() {
  clearVdp();
  screen5();
  cmd(0, 0, 0, 0, 256, 212, 0x44, 0, 0xC0);                        // HMMV: 全面を色4
  for (int i = 0; i < 16; i++) cmd(0, 0, i * 16, 0, 16, 40, i, 0, 0x80);   // LMMV: 16色の帯
  cmd(0, 0, 10, 50, 200, 100, 15, 0, 0x70);                        // LINE (長辺X)
  cmd(0, 0, 10, 50, 150, 60, 10, 0x01, 0x70);                      // LINE (長辺Y)
  cmd(0, 0, 128, 60, 100, 40, 8, 0x0C, 0x70);                      // LINE 左上向き
  cmd(0, 0, 40, 120, 60, 40, 2, 0, 0x80);
  cmd(40, 120, 120, 120, 60, 40, 0, 0, 0x90);                      // LMMM コピー
  cmd(40, 120, 190, 120, 60, 40, 0, 0, 0xD0);                      // HMMM コピー
  cmd(0, 0, 0, 0, 16, 40, 0, 0, 0xE0);                             // (YMMM は下で)
  // 16x16 スプライトを2枚(モード2、1ラインごとに色を変える)
  wreg(1, 0x42);                                                   // 16x16
  for (int i = 0; i < 32; i++) vpoke(0x7800 + i, (i & 1) ? 0xAA : 0xFF);   // パターン0
  vpoke(0x7600 + 0, 150); vpoke(0x7600 + 1, 30); vpoke(0x7600 + 2, 0); vpoke(0x7600 + 3, 0);
  vpoke(0x7604 + 0, 160); vpoke(0x7604 + 1, 40); vpoke(0x7604 + 2, 0); vpoke(0x7604 + 3, 0);
  vpoke(0x7608, 216);                                              // 終端
  for (int i = 0; i < 16; i++) { vpoke(0x7400 + i, 1 + (i & 7) + 8); vpoke(0x7410 + i, 0x40 | 2); }
  setPalette(1, 7, 0, 0);
  renderFrame();
  dumpScreen("s5.png");
}

static void testScreen8() {
  clearVdp();
  wreg(0, 0x0E); wreg(1, 0x40); wreg(2, 0x1F); wreg(5, 0xF7); wreg(11, 1);
  wreg(6, 0x1E); wreg(7, 0x00); wreg(8, 0x08); wreg(9, 0x80);
  // CPU から直接書く(G7 はインターリーブ配置)。色 = GRB332
  for (int y = 0; y < 212; y++) {
    setWriteAddr((uint32_t)y * 256);
    for (int x = 0; x < 256; x++) {
      uint8_t g = (x >> 5) & 7, r = (y >> 5) & 7, b = (x >> 3) & 3;
      vdpWritePort(0, (g << 5) | (r << 2) | b);
    }
  }
  cmd(0, 0, 60, 60, 136, 90, 0xFF, 0, 0xC0);                       // HMMV 白
  cmd(0, 0, 70, 70, 116, 70, 0x1C, 0, 0x80);                       // LMMV 赤
  renderFrame();
  dumpScreen("s8.png");
}

static void testScreen12() {
  clearVdp();
  wreg(0, 0x0E); wreg(1, 0x40); wreg(2, 0x1F); wreg(8, 0x0A); wreg(9, 0x80);   // スプライト無効
  wreg(25, 0x08);                                                  // YJK
  for (int y = 0; y < 212; y++) {
    setWriteAddr((uint32_t)y * 256);
    int j = (y / 7) % 64 - 32, k = ((y * 3) / 7) % 64 - 32;
    for (int g = 0; g < 64; g++) {
      int Y = g >> 1;
      uint8_t kk = k & 63, jj = j & 63;
      uint8_t q[4] = { (uint8_t)((Y << 3) | (kk & 7)), (uint8_t)((Y << 3) | (kk >> 3)),
                       (uint8_t)((Y << 3) | (jj & 7)), (uint8_t)((Y << 3) | (jj >> 3)) };
      for (int t = 0; t < 4; t++) vdpWritePort(0, q[t]);
    }
  }
  renderFrame();
  dumpScreen("s12.png");
}

static void testText2() {
  clearVdp();
  wreg(0, 0x04); wreg(1, 0x50); wreg(2, 0x03); wreg(3, 0x2F); wreg(10, 0); wreg(4, 0x02);
  wreg(7, 0xF4); wreg(8, 0x08); wreg(9, 0x80); wreg(12, 0x1F); wreg(13, 0xF0);
  // 仮フォント: 文字コードの各ビットを縦縞にする
  for (int c = 0; c < 256; c++)
    for (int r = 0; r < 8; r++) vpoke(0x1000 + c * 8 + r, (r == 7) ? 0 : (uint8_t)((c << 2) | ((r & 1) ? 0x84 : 0x00)));
  for (int i = 0; i < 80 * 27; i++) vpoke(i, (uint8_t)i);
  for (int i = 0; i < 20; i++) vpoke(0x0A00 + i, 0xF0);            // 先頭数行の一部を点滅属性に
  renderFrame();
  dumpScreen("t2.png");
}

static void testScreen2Sprites() {
  clearVdp();
  wreg(0, 0x02); wreg(1, 0x60 & ~0x20); wreg(1, 0x40); wreg(2, 0x06); wreg(3, 0xFF); wreg(4, 0x03);
  wreg(5, 0x36); wreg(6, 0x07); wreg(7, 0x05); wreg(9, 0x00);
  for (int i = 0; i < 768; i++) vpoke(0x1800 + i, (uint8_t)i);
  for (int i = 0; i < 0x1800; i++) { vpoke(i, (i & 8) ? 0xF0 : 0x3C); vpoke(0x2000 + i, (uint8_t)(((i >> 3) & 15) << 4 | 1)); }
  for (int i = 0; i < 8; i++) vpoke(0x3800 + i, 0xFF);
  for (int s = 0; s < 6; s++) {                                    // 同じ行に6枚 → 5枚目以降は消える
    vpoke(0x1B00 + s * 4 + 0, 80); vpoke(0x1B00 + s * 4 + 1, 20 + s * 30);
    vpoke(0x1B00 + s * 4 + 2, 0);  vpoke(0x1B00 + s * 4 + 3, 8 + s);
  }
  vpoke(0x1B00 + 24, 208);
  renderFrame();
  dumpScreen("s2.png");
  printf("S#0 after frame = %02X (expect 5S set, sprite 4)\n", vdpState()->status[0]);
}

// ---------------------------------------------------------------------------
//  ROM 起動
// ---------------------------------------------------------------------------
static uint8_t *loadFile(const std::string &path, uint32_t *size) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *p = (uint8_t *)malloc(n);
  fread(p, 1, n, f);
  fclose(f);
  *size = (uint32_t)n;
  printf("loaded %s (%ld bytes)\n", path.c_str(), n);
  return p;
}
static uint8_t *loadAny(const std::string &dir, std::initializer_list<const char *> names, uint32_t *size) {
  for (const char *n : names) {
    uint8_t *p = loadFile(dir + "/" + n, size);
    if (p) return p;
  }
  return nullptr;
}

// CART=<file> [MAPPER=NONE|KONAMI|ASCII8|ASCII16|RTYPE] でスロット1にカートリッジを挿す
// (サイズは MSX2.ino の loadCartridge と同じく2のべき乗に切り上げ、余りは FFh)
static void insertCartFromEnv() {
  if (const char *ss = getenv("SCCSLOT")) {          // SCCSLOT=1|2: ROM の無い SCC カートリッジ
    msx2SetCartridge(atoi(ss), nullptr, 0, MSX2_MAP_SCC);
    printf("SCC cartridge in slot %d\n", atoi(ss));
  }
  if (const char *ms = getenv("MOUSE")) msx2SetMousePort(atoi(ms));   // MOUSE=1|2
  if (getenv("SPRNOLIMIT")) vdpSetSpriteNoLimit(true);               // SPRNOLIMIT=1: スプライトの枚数制限なし(Shift+F8)
  const char *path = getenv("CART");
  if (!path || !*path) return;
  uint32_t size;
  uint8_t *file = loadFile(path, &size);
  if (!file) { printf("CART %s not found\n", path); return; }
  uint32_t rs = 0x2000;
  while (rs < size) rs <<= 1;
  uint8_t *rom = (uint8_t *)malloc(rs);
  memset(rom, 0xFF, rs);
  memcpy(rom, file, size);
  free(file);
  static const char *names[] = { "NONE", "KONAMI", "ASCII8", "ASCII16", "RTYPE", "KONAMI_SCC", "SCC" };
  Msx2Mapper m = MSX2_MAP_NONE;
  if (const char *ms = getenv("MAPPER"))
    for (int i = 0; i < 7; i++) if (!strcmp(ms, names[i])) m = (Msx2Mapper)i;
  printf("Cartridge: %s (%u bytes, %u allocated) mapper=%s\n", path, size, rs, names[m]);
  msx2SetCartridge(1, rom, rs, m);
}

// FM-PAC のレジスタ動作を CPU を通さずに確認する(openMSX の MSXFmPac と同じ動作か)
static int fmpacTest() {
  static uint8_t rom[0x10000], sram[0x2000], bios[0x8000];
  memset(rom, 0xFF, sizeof(rom));
  for (int b = 0; b < 4; b++) { rom[b * 0x4000 + 0x100] = (uint8_t)(0xB0 + b); rom[b * 0x4000 + 0x2100] = (uint8_t)(0xC0 + b); }
  memcpy(rom + 0x18, "PAC2OPLL", 8);
  memset(sram, 0xFF, sizeof(sram));
  memset(bios, 0xFF, sizeof(bios));
  msx2SetBios(bios);
  msx2SetFmPac(rom, sizeof(rom), sram);
  msx2Reset();
  int fails = 0;
  auto expect = [&](const char *what, int got, int want) {
    printf("%s %-28s got %02X expect %02X\n", got == want ? "ok" : "NG", what, got, want);
    if (got != want) fails++;
  };
  OutZ80(0xA8, 0x08);                                // ページ1 = スロット2
  char sig[9] = { 0 };
  for (int i = 0; i < 8; i++) sig[i] = (char)RdZ80((zword)(0x4018 + i));
  expect("signature PAC2OPLL", strcmp(sig, "PAC2OPLL") == 0, 1);
  expect("bank0 4100h", RdZ80(0x4100), 0xB0);
  expect("bank0 6100h", RdZ80(0x6100), 0xC0);
  expect("7FF6h after reset", RdZ80(0x7FF6), 0x00);
  expect("7FF7h after reset", RdZ80(0x7FF7), 0x00);
  WrZ80(0x7FF7, 0x02);
  expect("bank2 4100h", RdZ80(0x4100), 0xB2);
  expect("bank2 6100h", RdZ80(0x6100), 0xC2);
  expect("7FF7h read", RdZ80(0x7FF7), 0x02);
  WrZ80(0x7FF7, 0x07);
  expect("7FF7h masked to 2 bits", RdZ80(0x7FF7), 0x03);
  expect("bank3 4100h", RdZ80(0x4100), 0xB3);
  int w0 = g_fmWrites;
  OutZ80(0x7C, 0x10); OutZ80(0x7D, 0x20);
  expect("I/O 7Ch/7Dh disabled", g_fmWrites - w0, 0);
  WrZ80(0x7FF4, 0x10); WrZ80(0x7FF5, 0x20);
  expect("7FF4h/7FF5h always write", g_fmWrites - w0, 2);
  WrZ80(0x7FF6, 0x01);
  expect("7FF6h read", RdZ80(0x7FF6), 0x01);
  OutZ80(0x7C, 0x10); OutZ80(0x7D, 0x20);
  expect("I/O 7Ch/7Dh enabled", g_fmWrites - w0, 4);
  WrZ80(0x7FF6, 0xFF);
  expect("7FF6h masked to 11h", RdZ80(0x7FF6), 0x11);
  WrZ80(0x5FFE, 0x4D); WrZ80(0x5FFF, 0x69);
  expect("SRAM key locked by bit4", RdZ80(0x4100), 0xB3);
  WrZ80(0x7FF6, 0x01);
  WrZ80(0x5FFE, 0x4D); WrZ80(0x5FFF, 0x69);
  expect("SRAM on, 4100h", RdZ80(0x4100), 0xFF);
  WrZ80(0x4100, 0x5A);
  expect("SRAM write/read", RdZ80(0x4100), 0x5A);
  expect("SRAM buffer", sram[0x100], 0x5A);
  expect("5FFEh reads key", RdZ80(0x5FFE), 0x4D);
  expect("5FFFh reads key", RdZ80(0x5FFF), 0x69);
  expect("6100h FFh while SRAM", RdZ80(0x6100), 0xFF);
  expect("7FF7h while SRAM", RdZ80(0x7FF7), 0x03);
  WrZ80(0x5FFE, 0x00);
  expect("SRAM off -> ROM bank3", RdZ80(0x4100), 0xB3);
  WrZ80(0x4100, 0x77);
  expect("ROM write ignored", RdZ80(0x4100), 0xB3);
  expect("SRAM untouched", sram[0x100], 0x5A);
  msx2Reset();
  OutZ80(0xA8, 0x08);
  expect("reset -> bank0", RdZ80(0x4100), 0xB0);
  expect("reset -> 7FF6h=0", RdZ80(0x7FF6), 0x00);
  OutZ80(0xA8, 0x00);                                // ページ1 = スロット0(BIOS側)
  expect("slot0: 7FF6h is not FM-PAC", RdZ80(0x7FF6), 0xFF);
  WrZ80(0x7FF7, 0x02);
  OutZ80(0xA8, 0x08);
  expect("slot0 write did not switch", RdZ80(0x7FF7), 0x00);
  printf("FMPAC TEST %s (%d failures)\n", fails ? "NG" : "OK", fails);
  return fails ? 1 : 0;
}

// ROM の無い SCC カートリッジ・(KS) の SCC 読み出し・マウス・FM-PAC の SRAM 書き込み回数・CPU の速さ
static int devTest() {
  static uint8_t bios[0x8000], rom[0x20000], fprom[0x4000], sram[0x2000];
  memset(bios, 0xFF, sizeof(bios));
  for (int i = 0; i < (int)sizeof(rom); i++) rom[i] = (uint8_t)(i >> 13);   // 各 8KB バンクの番号
  msx2SetBios(bios);
  int fails = 0;
  auto expect = [&](const char *what, int got, int want) {
    printf("%s %-40s got %02X expect %02X\n", got == want ? "ok" : "NG", what, got, want);
    if (got != want) fails++;
  };
  // ---- SCC だけのカートリッジ(スロット1)----
  msx2SetCartridge(1, nullptr, 0, MSX2_MAP_SCC);
  msx2Reset();
  OutZ80(0xA8, 0x10);                                // ページ2 = スロット1
  expect("SCC cart: 8000h empty", RdZ80(0x8000), 0xFF);
  WrZ80(0x9800, 0x12);
  expect("SCC off: 9800h not writable", RdZ80(0x9800), 0xFF);
  WrZ80(0x9000, 0x3F);
  for (int i = 0; i < 0x80; i++) WrZ80((zword)(0x9800 + i), (uint8_t)(i ^ 0x5A));
  int bad = 0;
  for (int i = 0; i < 0x80; i++) if (RdZ80((zword)(0x9800 + i)) != (uint8_t)(i ^ 0x5A)) bad++;
  expect("SCC on: wave RAM read back", bad, 0);
  expect("SCC mirror 9905h", RdZ80(0x9905), 0x05 ^ 0x5A);
  expect("SCC 9880h (freq) reads FFh", RdZ80(0x9880), 0xFF);
  expect("SCC 8000h still empty", RdZ80(0x8000), 0xFF);
  WrZ80(0x9000, 0x00);
  expect("SCC off again: 9800h", RdZ80(0x9800), 0xFF);
  OutZ80(0xA8, 0x00);
  WrZ80(0x9000, 0x3F);
  OutZ80(0xA8, 0x10);
  expect("write via other slot ignored", RdZ80(0x9800), 0xFF);
  msx2SetCartridge(1, nullptr, 0, MSX2_MAP_NONE);
  // ---- (KS) の SCC 読み出し(スロット2)----
  msx2SetCartridge(2, rom, sizeof(rom), MSX2_MAP_KONAMI_SCC);
  msx2Reset();
  OutZ80(0xA8, 0x20);                                // ページ2 = スロット2
  expect("KS: 8000h bank2", RdZ80(0x8000), 0x02);
  WrZ80(0x9000, 0x3F);
  WrZ80(0x9801, 0xA5);
  expect("KS: SCC wave read", RdZ80(0x9801), 0xA5);
  expect("KS: 8000h = bank 3Fh&mask", RdZ80(0x8000), 0x0F);
  WrZ80(0x9000, 0x05);
  expect("KS: SCC off -> ROM bank5", RdZ80(0x9801), 0x05);
  msx2SetCartridge(2, nullptr, 0, MSX2_MAP_NONE);
  // ---- マウス(ポート1)----
  msx2SetMousePort(1);
  msx2Reset();
  msx2_mouse_x += 10;                                // 右へ 10
  msx2_mouse_y += 5;                                 // 上へ 5
  msx2_mouse_buttons = 1;                            // 左ボタン
  uint8_t r15 = 0x00, nib[4];
  for (int k = 0; k < 4; k++) {
    r15 ^= 0x10;                                     // 8 番ピンを切り替える
    OutZ80(0xA0, 15); OutZ80(0xA1, r15);
    OutZ80(0xA0, 14); nib[k] = InZ80(0xA2);
  }
  expect("mouse X high (-10 = F6h)", nib[0] & 0x0F, 0x0F);
  expect("mouse X low", nib[1] & 0x0F, 0x06);
  expect("mouse Y high (+5)", nib[2] & 0x0F, 0x00);
  expect("mouse Y low", nib[3] & 0x0F, 0x05);
  expect("mouse left button = trigger A", nib[0] & 0x30, 0x20);
  for (int k = 0; k < 4; k++) { r15 ^= 0x10; OutZ80(0xA0, 15); OutZ80(0xA1, r15); OutZ80(0xA0, 14); nib[k] = InZ80(0xA2); }
  expect("mouse no move -> 0", (nib[0] & 15) | (nib[1] & 15) | (nib[2] & 15) | (nib[3] & 15), 0);
  OutZ80(0xA0, 15); OutZ80(0xA1, r15 | 0x40);        // ポート2(ジョイスティック)を読む
  OutZ80(0xA0, 14);
  expect("port2 is joystick", InZ80(0xA2), msx2_joy[1]);
  msx2SetMousePort(0);
  msx2_mouse_buttons = 0;
  // ---- FM-PAC の SRAM 書き込み回数 ----
  memset(fprom, 0xFF, sizeof(fprom));
  msx2SetFmPac(fprom, sizeof(fprom), sram);
  msx2Reset();
  OutZ80(0xA8, 0x08);
  uint32_t w0 = msx2FmPacSramWrites();
  WrZ80(0x7FF6, 0x01); WrZ80(0x5FFE, 0x4D); WrZ80(0x5FFF, 0x69);
  WrZ80(0x4000, 0x11); WrZ80(0x4001, 0x22); WrZ80(0x4001, 0x22);
  expect("FM-PAC SRAM writes counted (same value not)", (int)(msx2FmPacSramWrites() - w0), 2);
  msx2SetFmPac(nullptr, 0, nullptr);
  // ---- CPU の速さ ----
  msx2SetCpuCycles(300);
  expect("cpu cycles 300", msx2GetCpuCycles(), 300);
  msx2SetCpuCycles(372);
  expect("cpu cycles 372", msx2GetCpuCycles(), 372);
  msx2SetCpuCycles(228);
  expect("cpu cycles 228", msx2GetCpuCycles(), 228);
  printf("DEV TEST %s (%d failures)\n", fails ? "NG" : "OK", fails);
  return fails ? 1 : 0;
}

// R#18(画面位置調整)と V9938 モードの確認
static uint8_t px(int y, int x) { static const int order[4] = { 2, 3, 0, 1 }; return rows[y][(x & ~3) + order[x & 3]]; }
static int adjTest() {
  int fails = 0;
  auto expect = [&](const char *what, int got, int want) {
    printf("%s %-44s got %d expect %d\n", got == want ? "ok" : "NG", what, got, want);
    if (got != want) fails++;
  };
  static uint8_t ref[MSX2_SCREEN_H][MSX2_SCREEN_W];
  for (int lines = 212; lines >= 192; lines -= 20) {
    clearVdp();
    screen5();
    if (lines == 192) wreg(9, 0x00);
    wreg(7, 0x04);                                                   // 枠の色 = 4
    for (int y = 0; y < 256; y++)
      for (int x = 0; x < 128; x++) vpoke(y * 128 + x, (uint8_t)((x * 7 + y * 3) ^ (y >> 2)));
    setPalette(4, 0, 0, 7); setPalette(15, 7, 7, 7);
    wreg(18, 0); renderFrame();
    for (int y = 0; y < MSX2_SCREEN_H; y++) for (int x = 0; x < MSX2_SCREEN_W; x++) ref[y][x] = px(y, x);
    uint8_t bd = ref[0][0];                                          // 192ラインのときは上の枠
    if (lines == 212) { renderFrame(); wreg(9, 0x00); renderFrame(); bd = px(0, 0); wreg(9, 0x80); }
    for (int nib = 0; nib < 16; nib++) {
      int dx = nib < 8 ? -nib : 16 - nib;
      wreg(18, (uint8_t)nib); renderFrame();
      int bad = 0;
      for (int y = 0; y < MSX2_SCREEN_H; y++)
        for (int x = 0; x < MSX2_SCREEN_W; x++) {
          int sx = x - 2 * dx;
          uint8_t want = (sx >= 0 && sx < MSX2_SCREEN_W) ? ref[y][sx] : ((lines == 192 && (y < 10 || y >= 202)) ? ref[y][0] : bd);
          if (px(y, x) != want) bad++;
        }
      char what[64]; snprintf(what, sizeof(what), "%d lines: H adjust %X (dx=%d) mismatches", lines, nib, dx);
      expect(what, bad, 0);
    }
    for (int nib = 0; nib < 16; nib++) {
      int dy = nib < 8 ? -nib : 16 - nib;
      wreg(18, (uint8_t)(nib << 4)); renderFrame();
      int bad = 0;
      int top = (MSX2_SCREEN_H - lines) / 2;
      for (int y = 0; y < MSX2_SCREEN_H; y++) {
        int sy = y - dy;                                             // 元の画面のこの行が来る
        bool inDisp = sy >= top && sy < top + lines;
        for (int x = 0; x < MSX2_SCREEN_W; x++) {
          uint8_t want = inDisp ? ref[sy][x] : bd;
          if (px(y, x) != want) bad++;
        }
      }
      char what[64]; snprintf(what, sizeof(what), "%d lines: V adjust %X (dy=%d) mismatches", lines, nib, dy);
      expect(what, bad, 0);
    }
  }
  // V9938 モード: ID=0、R#25〜27 は書けない。V9958 は ID=2
  clearVdp();
  vdpSetV9938(true);
  wreg(15, 1); expect("V9938: S#1 ID bits", vdpReadPort(1) & 0x3E, 0x00);
  wreg(25, 0x18); wreg(26, 5); wreg(27, 3);
  expect("V9938: R#25 ignored", vdpState()->reg[25], 0);
  expect("V9938: R#26 ignored", vdpState()->reg[26], 0);
  expect("V9938: R#27 ignored", vdpState()->reg[27], 0);
  vdpSetV9938(false);
  wreg(15, 1); expect("V9958: S#1 ID bits", vdpReadPort(1) & 0x3E, 0x04);
  wreg(26, 5); expect("V9958: R#26 written", vdpState()->reg[26], 5);
  wreg(15, 0);
  printf("ADJUST TEST %s (%d failures)\n", fails ? "NG" : "OK", fails);
  return fails ? 1 : 0;
}

// FDC(wd2793.cpp)の動作を CPU を通さずに確認する(openMSX の PhilipsFDC / WD2793 と同じ動作か)
static int fdcTest() {
  static uint8_t bios[0x8000], img[737280], img2[368640];
  memset(bios, 0xFF, sizeof(bios));
  msx2SetBios(bios);
  msx2Reset();
  for (uint32_t s = 0; s < sizeof(img) / 512; s++) { memset(img + s * 512, (uint8_t)s, 512); img[s * 512 + 1] = (uint8_t)(s >> 8); }
  for (uint32_t s = 0; s < sizeof(img2) / 512; s++) memset(img2 + s * 512, (uint8_t)(0x80 ^ s), 512);
  int fails = 0;
  auto expect = [&](const char *what, int got, int want) {
    printf("%s %-40s got %02X expect %02X\n", got == want ? "ok" : "NG", what, got, want);
    if (got != want) fails++;
  };
  auto cmd = [](uint8_t c) { msx2FdcWrite(0, c); };
  auto st = []() { return (int)msx2FdcRead(0); };
  auto runLines = [](int frames) { for (int i = 0; i < frames; i++) msx2RunFrame(); };
  wd2793Reset();
  wd2793SetDisk(0, img, sizeof(img));               // 起動前に入れる(入れ替え中の空き時間なし)
  msx2FdcWrite(5, 0x80);                             // ドライブA、モーターON
  expect("7FFD bit2=0 once after insert", msx2FdcRead(5) & 4, 0);
  expect("7FFD bit2=1 afterwards", msx2FdcRead(5) & 4, 4);
  cmd(0x08);                                         // Restore
  expect("7FFF INTRQ low after restore", msx2FdcRead(7) & 0x40, 0x00);
  expect("restore: TRACK00, ready", st() & 0x84, 0x04);
  expect("7FFF INTRQ cleared by status read", msx2FdcRead(7) & 0x40, 0x40);
  // (インデックスパルスはライン単位で変わるので、フレーム単位でしか進められないここでは確かめない)
  msx2FdcWrite(3, 5); cmd(0x18);                     // Seek 5
  expect("seek: track reg", msx2FdcRead(1), 5);
  expect("seek: not TRACK00, no seek error", st() & 0x14, 0x00);
  expect("seek: BUSY while stepping (5x6ms)", st() & 0x01, 0x01);
  expect("seek: no INTRQ while stepping", msx2FdcRead(7) & 0x40, 0x40);
  runLines(2);                                       // 2フレーム = 524ライン > 5ステップ(470ライン)
  expect("seek: INTRQ after stepping", msx2FdcRead(7) & 0x40, 0x00);
  expect("seek: BUSY off after stepping", st() & 0x01, 0x00);
  msx2FdcWrite(4, 1); msx2FdcWrite(2, 3); cmd(0x80); // サイド1 セクタ3 を読む
  int lba = (5 * 2 + 1) * 9 + 2;
  expect("read: DRQ", msx2FdcRead(7) & 0x80, 0x00);
  int b0 = msx2FdcRead(3), b1 = msx2FdcRead(3);
  for (int i = 2; i < 512; i++) msx2FdcRead(3);
  expect("read: sector data (LBA low)", b0, lba & 0xFF);
  expect("read: sector data (LBA high)", b1, lba >> 8);
  expect("read: done, no error", st() & 0x13, 0x00);
  msx2FdcWrite(1, 7); msx2FdcWrite(2, 1); cmd(0x80); // トラックレジスタがヘッドの位置と違う
  expect("track reg != head -> RNF", st() & 0x10, 0x10);
  msx2FdcWrite(1, 5);
  cmd(0x5C);                                         // Step In + update + verify
  expect("step in: track reg 6", msx2FdcRead(1), 6);
  expect("step in verify ok", st() & 0x10, 0x00);
  cmd(0x48);                                         // Step In (update なし): ヘッドだけ動く
  expect("step w/o update keeps track reg", msx2FdcRead(1), 6);
  msx2FdcWrite(2, 1); cmd(0x80);
  expect("head moved -> RNF on read", st() & 0x10, 0x10);
  msx2FdcWrite(3, 2); cmd(0x18);                     // Seek 2(track reg 6 → 2: ヘッドは 7 → 3)
  msx2FdcWrite(1, 3); msx2FdcWrite(4, 0); msx2FdcWrite(2, 1); cmd(0x80);
  b0 = msx2FdcRead(3); b1 = msx2FdcRead(3);
  expect("seek moves head relative (track 3)", b0 | (b1 << 8), 3 * 2 * 9);
  cmd(0xD0);
  expect("D0: no INTRQ", msx2FdcRead(7) & 0x40, 0x40);
  cmd(0x08);
  msx2FdcWrite(4, 0); cmd(0xC0);                     // Read Address
  int id[6]; for (int i = 0; i < 6; i++) id[i] = msx2FdcRead(3);
  expect("read address: track", id[0], 0);
  expect("read address: size code", id[3], 2);
  expect("read address: sector reg = track", msx2FdcRead(2), 0);
  msx2FdcWrite(2, 8); cmd(0x90);                     // マルチセクタ: 8,9 を読んで 10 で RNF
  for (int i = 0; i < 1024; i++) msx2FdcRead(3);
  expect("multi-sector read ends with RNF", st() & 0x11, 0x10);
  msx2FdcWrite(2, 2); cmd(0xA0);                     // 書き込み
  for (int i = 0; i < 512; i++) msx2FdcWrite(3, 0x5A);
  expect("write: done", st() & 0x13, 0x00);
  expect("write: image updated", img[512 * 1], 0x5A);
  // ドライブB(つながっていない): TRACK00 にならず Restore は Seek Error
  msx2FdcWrite(5, 0x81); cmd(0x08);
  expect("drive B absent: not ready, seek error, no TR00", st() & 0x94, 0x90);
  msx2FdcWrite(5, 0x83); cmd(0x08);
  expect("drive 3 (none): seek error", st() & 0x10, 0x10);
  // 実行中にドライブAを入れ替え: 約0.3秒はディスクなし、そのあと読める。7FFD bit2 で1回だけ知らせる
  msx2FdcWrite(5, 0x80);
  runLines(1);                                       // 実行中(ライン時計が進んでいる)の入れ替え
  wd2793SetDisk(0, img2, sizeof(img2));
  cmd(0x08);
  expect("swap: not ready during gap", st() & 0x80, 0x80);
  runLines(25);
  expect("swap: ready after gap", st() & 0x80, 0x00);
  expect("swap: 7FFD bit2=0 once", msx2FdcRead(5) & 4, 0);
  expect("swap: 7FFD bit2=1 then", msx2FdcRead(5) & 4, 4);
  msx2FdcWrite(1, 0); msx2FdcWrite(4, 1); msx2FdcWrite(2, 1); cmd(0x80);
  expect("1DD image: side 1 -> RNF", st() & 0x10, 0x10);
  msx2FdcWrite(4, 0); msx2FdcWrite(2, 2); cmd(0x80);
  expect("1DD image: new disk data", msx2FdcRead(3), 0x80 ^ 1);
  printf("FDC TEST %s (%d failures)\n", fails ? "NG" : "OK", fails);
  return fails ? 1 : 0;
}

static int boot(const std::string &dir, int frames, const std::string &out, int every) {
  uint32_t n;
  bool msx2Bios = false;
  uint8_t *bios = loadAny(dir, { "MSX2P.ROM", "msx2p.rom" }, &n);
  if (!bios && (bios = loadAny(dir, { "MSX2.ROM", "msx2.rom" }, &n)) != nullptr) msx2Bios = true;
  if (!bios) bios = loadAny(dir, { "cbios_main_msx2+_jp.rom", "cbios_main_msx2+.rom" }, &n);
  if (!bios) { printf("BIOS not found in %s\n", dir.c_str()); return 1; }
  msx2SetBios(bios);
  // MSX2.ino と同じく、BIOS の機種番号(002Dh)が MSX2 なら V9938 として動かす(VDP=9938/9958 で指定も可)
  bool v9938 = n > 0x2D && bios[0x2D] < 2;
  if (const char *e = getenv("VDP")) v9938 = (strcmp(e, "9938") == 0);
  vdpSetV9938(v9938);
  printf("BIOS version byte %02X -> VDP %s\n", bios[0x2D], v9938 ? "V9938" : "V9958");
  bool cbios = false;
  if (uint8_t *p = loadAny(dir, { "cbios_logo_msx2+.rom" }, &n)) { msx2SetRom(0, 0, 0x8000, p, n); cbios = true; }
  if (cbios) { msx2SetSlotExpanded(0, false); msx2SetF4Inverted(true); }   // C-BIOS の前提に合わせる
  if (uint8_t *p = msx2Bios ? loadAny(dir, { "MSX2EXT.ROM", "msx2ext.rom" }, &n)
                            : loadAny(dir, { "MSX2PEXT.ROM", "msx2pext.rom", "cbios_sub.rom" }, &n)) msx2SetSubRom(p);
  if (!msx2Bios)
    if (uint8_t *p = loadAny(dir, { "KNJBASIC.ROM", "MSXKANJI.ROM" }, &n)) msx2SetKanjiBasic(p, n);
  // MSX2.ino と同じく、FMPAC.ROM があれば FM-PAC をスロット2に挿して内蔵MSX-MUSICは載せない
  bool fmpac = false;
  if (uint8_t *p = loadAny(dir, { "FMPAC.ROM", "fmpac.rom" }, &n)) {
    static uint8_t sram[0x2000];
    memset(sram, 0xFF, sizeof(sram));
    msx2SetFmPac(p, n, sram);
    fmpac = true;
  }
  if (!fmpac)
    if (uint8_t *p = loadAny(dir, { "MSX2PMUS.ROM", "FMBASIC.ROM", "cbios_music.rom" }, &n)) {
      if (cbios) msx2SetMusicRom(p, 3, 3); else msx2SetMusicRom(p);
    }
  if (uint8_t *p = loadAny(dir, { "KANJI.ROM" }, &n)) msx2SetKanjiFont(p, n);
  insertCartFromEnv();
  // FDCLOG=1: FDC のコマンドを出す
  if (getenv("FDCLOG"))
    wd2793CmdHook = [](uint8_t cmd, uint8_t tr, uint8_t sec, uint8_t side, int drv, uint8_t st) {
      printf("  FDC f%u L%u cmd=%02X tr=%u sec=%u side=%u drv=%d st=%02X PC=%04X\n", msx2LineClock() / MSX2_LINES_PER_FRAME,
             msx2LineClock() % MSX2_LINES_PER_FRAME, cmd, tr, sec, side, drv, st, msx2_cpu.PC.W);
    };
  // VDPLOG=<from>-<to>: そのフレームの範囲の VDP レジスタ書き込みを「フレーム ライン R#=値 PC」で出す
  static int vlFrom = -1, vlTo = -1;
  if (const char *vl = getenv("VDPLOG")) sscanf(vl, "%d-%d", &vlFrom, &vlTo);
  if (vlFrom >= 0)
    vdpRegHook = [](int r, uint8_t v) {
      uint32_t lc = msx2LineClock() - 1, f = lc / MSX2_LINES_PER_FRAME + 1;
      if ((int)f < vlFrom || (int)f > vlTo) return;
      if (r == 46) {
        const uint8_t *g = vdpState()->reg;
        printf("  VDP f%u L%u c%d R46=%02X SX=%d SY=%d DX=%d DY=%d NX=%d NY=%d ARG=%02X PC=%04X\n", f, lc % MSX2_LINES_PER_FRAME, vdpLineCycle(), v,
               g[32] | (g[33] << 8), g[34] | ((g[35] & 3) << 8), g[36] | (g[37] << 8), g[38] | ((g[39] & 3) << 8),
               g[40] | ((g[41] & 1) << 8), g[42] | ((g[43] & 3) << 8), g[45], msx2_cpu.PC.W);
      } else if (r < 32 || r > 45) {
        printf("  VDP f%u L%u c%d R%d=%02X PC=%04X\n", f, lc % MSX2_LINES_PER_FRAME, vdpLineCycle(), r, v, msx2_cpu.PC.W);
      }
    };
  // DISK1=<file> DISK2=<file> でドライブ1/2にディスクを入れる
  for (int d = 0; d < 2; d++)
    if (const char *dp = getenv(d ? "DISK2" : "DISK1"))
      if (uint8_t *p = loadFile(dp, &n)) wd2793SetDisk(d, p, n);
  // MSX2.ino と同じく、カートリッジだけで起動するときは DISK.ROM を外す
  if (!getenv("CART"))
    if (uint8_t *p = loadAny(dir, { "DISK.ROM" }, &n)) msx2SetDiskRom(p);
  msx2Reset();
  if (const char *cc = getenv("CPUCYCLES")) msx2SetCpuCycles(atoi(cc));   // CPUCYCLES=228|300|372 (MSX2.ino の F6)
  msx2_cpu.Trace = 1;
#if MSX2_PROFILE
  // MSX2.ino と同じく300フレームごとに内訳を出す(PCの時間は参考。回数は ESP32 と同じになる)
  uint32_t clockPerUs = profClockPerUs();
  printf("profile clock: %u counts/us\n", clockPerUs);
#endif
  // KEYS=<frame>:<row>:<bit>,... : そのフレームから10フレームのあいだキーを押す (スペース = 8:0、リターン = 7:7)
  struct KeyEv { int frame, row, bit; };
  std::vector<KeyEv> keys;
  if (const char *ks = getenv("KEYS")) {
    for (const char *p = ks; *p;) {
      KeyEv e;
      if (sscanf(p, "%d:%d:%d", &e.frame, &e.row, &e.bit) == 3) keys.push_back(e);
      const char *c = strchr(p, ',');
      if (!c) break;
      p = c + 1;
    }
  }
  // SWAP=<frame>:<drive 0/1>:<file>|... : そのフレームでディスクを入れ替える(MSX2.ino の F9/F10 と同じ)
  struct SwapEv { int frame, drive; std::string path; };
  std::vector<SwapEv> swaps;
  if (const char *sw = getenv("SWAP")) {
    std::string all = sw;
    size_t pos = 0;
    while (pos < all.size()) {
      size_t e = all.find('|', pos);
      std::string item = all.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
      int fr, dr, used = 0;
      if (sscanf(item.c_str(), "%d:%d:%n", &fr, &dr, &used) == 2 && used > 0) swaps.push_back({ fr, dr, item.substr(used) });
      if (e == std::string::npos) break;
      pos = e + 1;
    }
  }
  for (int f = 1; f <= frames; f++) {
    for (const SwapEv &e : swaps)
      if (e.frame == f) {
        uint32_t sz;
        if (uint8_t *p = loadFile(e.path, &sz)) { wd2793SetDisk(e.drive, p, sz); printf("  frame %d: drive %d <- %s\n", f, e.drive, e.path.c_str()); }
      }
    for (int r = 0; r < 16; r++) msx2_key_matrix[r] = 0xFF;
    for (const KeyEv &e : keys)
      if (f >= e.frame && f < e.frame + 10) msx2_key_matrix[e.row & 15] &= (uint8_t)~(1 << e.bit);
    msx2RunFrame();
    if (int mm = vdpShadowMismatch()) {                // スプライト表の写しと VRAM の食い違い(あってはならない)
      static int reported;
      if (reported++ < 5) printf("SHADOW MISMATCH frame %d: %d bytes\n", f, mm);
    }
#if MSX2_PROFILE
    if (f % 300 == 0) {
      char prof[768];
      msx2ProfReport(prof, sizeof(prof), clockPerUs);
      fputs(prof, stdout);
    }
#endif
    // PNGSTART=<frame>: そのフレームより前の画像は出さない
    static int pngStart = getenv("PNGSTART") ? atoi(getenv("PNGSTART")) : 0;
    if (f >= pngStart && (f % every == 0 || f == frames)) {
      char name[64];
      snprintf(name, sizeof(name), "/frame%04d.png", f);
      dumpScreen(out + name);
      printf("  frame %d: mode=%02X lines=%d PC=%04X slots=%02X sub3=%02X\n", f, vdpDisplayMode(),
             vdpDisplayLines(), msx2_cpu.PC.W, msx2PrimarySlots(), msx2SubSlots(3));
    }
    // VRAMDUMP=<frame>: そのフレームの VRAM(128KB)とレジスタ・パレットを outdir/vram%04d.bin に書く
    if (const char *vd = getenv("VRAMDUMP"))
      if (atoi(vd) == f) {
        char name[64];
        snprintf(name, sizeof(name), "/vram%04d.bin", f);
        if (FILE *fp = fopen((out + name).c_str(), "wb")) {
          VdpState *v = vdpState();
          fwrite(vram, 1, VDP_VRAM_SIZE, fp);
          fwrite(v->reg, 1, 64, fp);
          fwrite(v->palette, 2, 16, fp);
          fclose(fp);
        }
      }
  }
  // MEMDUMP=lo-hi (16進): 最後に Z80 から見えるメモリを出す
  if (const char *md = getenv("MEMDUMP")) {
    int lo = 0, hi = 0;
    if (sscanf(md, "%x-%x", &lo, &hi) == 2)
      for (int a = lo & ~15; a <= hi; a += 16) {
        printf("%04X:", a);
        for (int i = 0; i < 16; i++) printf(" %02X", RdZ80((uint16_t)(a + i)));
        printf("\n");
      }
  }
  return 0;
}

int main(int argc, char **argv) {
  crcInit();
  vram = (uint8_t *)calloc(1, VDP_VRAM_SIZE);
  for (int y = 0; y < MSX2_SCREEN_H; y++) rows[y] = (uint8_t *)calloc(1, MSX2_SCREEN_W);
  static uint8_t *ram[MSX2_MAPPER_SEGMENTS];
  for (int i = 0; i < MSX2_MAPPER_SEGMENTS; i++) { ram[i] = (uint8_t *)calloc(1, 0x4000); msx2SetRamSegment(i, ram[i]); }
  vdpInit(vram, rows, kSync);

  std::string modeArg = argc > 1 ? argv[1] : "selftest";
  if (modeArg == "selftest") {
    testScreen5();
    testScreen8();
    testScreen12();
    testText2();
    testScreen2Sprites();
    return 0;
  }
  if (modeArg == "fmpactest") return fmpacTest();
  if (modeArg == "devtest") return devTest();
  if (modeArg == "fdctest") return fdcTest();
  if (modeArg == "adjtest") return adjTest();
  if (modeArg == "sprtest") {                        // スプライトの速い経路と今までの経路の比較
    int iters = argc > 2 ? atoi(argv[2]) : 2000, fast = 0, coll = 0;
    int fails = vdpSpriteSelfTest(12345, iters, &fast, &coll);
    printf("SPRITE TEST %s: %d tables x 256 lines, fast-path lines %d, collision lines %d, mismatches %d\n",
           fails ? "NG" : "OK", iters, fast, coll, fails);
    return fails ? 1 : 0;
  }
  if (modeArg == "boot" && argc >= 5) {
    return boot(argv[2], atoi(argv[3]), argv[4], argc > 5 ? atoi(argv[5]) : 60);
  }
  printf("usage: msx2host selftest | fmpactest | boot <romdir> <frames> <outdir> [every]\n");
  return 1;
}
