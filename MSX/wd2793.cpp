// ============================================================================
//  WD2793 FDC + ソニー(フィリップス)型ディスクインターフェース
//  (MSX2版 MSX2\wd2793.cpp と同じもの。MSX1 では I/O ポート型(Microsol)のインターフェースからも使う。
//   元は MSX1版 MSX.ino の実装で、openMSX の PhilipsFDC / WD2793 / RealDrive の動作に
//   合わせて直したもの。fMSX・blueMSX も同じ考え方)
//
//  7FF8 R:ステータス W:コマンド / 7FF9 トラック / 7FFA セクタ / 7FFB データ
//  7FFC bit0:サイド
//  7FFD W: bit0-1:ドライブ(0,2=A 1=B 3=なし) bit7:モーター
//       R: 書いた値。ただし bit2 は「ディスクが入れ替わっていない」とき 1。入れ替えたあと
//          最初の1回だけ 0 を返す(DISK.ROM の DSKCHG がこれを見る。これが無いと DOS は
//          前のディスクの FAT のまま読み、入れ替えを受け付けないゲームがある)
//  7FFF R: bit6=/INTRQ, bit7=/DRQ (どちらもアクティブLOW)、ほかのビットは 1
//
//  コマンドは一瞬で終わる(fMSX と同じ)。時間に関係するのはインデックスパルス(300rpm)と、
//  ディスクを入れ替えたときに少しの間ディスクが無い状態になることだけ。
// ============================================================================
#include "wd2793.h"
#include <string.h>

// 時間はラインの数で測る(15.734kHz)
static const uint32_t LINES_PER_ROT  = 3147;   // 300rpm = 1回転 200ms
static const uint32_t INDEX_LINES    = 63;     // インデックスパルスの幅 約4ms
static const uint32_t SWAP_GAP_LINES = 4720;   // 入れ替えのとき、ディスクが無いことにする時間 約0.3s
static const int      MAX_TRACK      = 83;
static const int      TRACK_BYTES    = 6250;   // 2DD 1トラックの生データ(Write Track で受け取る量)

struct Disk {
  uint8_t *buf;
  uint32_t size;
  uint8_t  sides, sectors;
  uint8_t  head;            // ヘッドの位置(物理トラック)。トラックレジスタとは別に持つ
  bool     connected;       // ドライブがあるか(A は常に。B はイメージを入れたことがあれば)
  bool     changed;         // 入れ替えた印(7FFD の bit2 で1回だけ返す)
  bool     gap;             // 入れ替え中(gapEnd まではディスクが無い)
  uint32_t gapEnd;
};
static Disk disk[2] = { { nullptr, 0, 0, 0, 0, true, false, false, 0 },
                        { nullptr, 0, 0, 0, 0, false, false, false, 0 } };

enum Mode { M_IDLE, M_READ, M_WRITE, M_READADDR, M_WRITETRACK };
static Mode     mode;
static uint8_t  regStatus, regTrack, regSector = 1, regData, regSide, driveReg;
static int      curDrive;                   // -1 = どのドライブも選んでいない
static uint8_t *sectorPtr;
static bool     typeI = true;               // ステータスを Type I の形で返すか
static bool     multiSector, intrq, intrqHold, headLoaded;
static int      dataIndex, dataLen;
static int      stepDir = -1;
static uint8_t  idField[6], idSector = 1;

void wd2793Reset() {
  mode = M_IDLE;
  regStatus = 0; regTrack = 0; regSector = 1; regData = 0; regSide = 0; driveReg = 0;
  curDrive = 0; sectorPtr = nullptr; typeI = true;
  multiSector = intrq = intrqHold = headLoaded = false;
  dataIndex = dataLen = 0; stepDir = -1;
}

void wd2793SetDisk(int d, uint8_t *buf, uint32_t size) {
  if (d < 0 || d > 1) return;
  Disk &k = disk[d];
  k.buf = buf;
  k.size = buf ? size : 0;
  k.changed = true;
  // 実行中の入れ替えは、実機と同じく一度ディスクが抜けた状態を通す(起動前は不要)
  k.gap = (fdcLineClock() != 0);
  k.gapEnd = fdcLineClock() + SWAP_GAP_LINES;
  if (!buf) return;
  k.connected = true;
  // ブートセクタに正しい BPB があればそれに従う(片面 360KB の中身を 720KB のファイルに
  // 入れたイメージがある。破邪の封印のユーザーディスク: 総セクタ数720・片面)
  {
    uint16_t bps = buf[0x0B] | (buf[0x0C] << 8), tot = buf[0x13] | (buf[0x14] << 8);
    uint16_t spt = buf[0x18] | (buf[0x19] << 8), heads = buf[0x1A] | (buf[0x1B] << 8);
    if ((buf[0] == 0xEB || buf[0] == 0xE9) && bps == 512 && (spt == 8 || spt == 9) && (heads == 1 || heads == 2) &&
        buf[0x15] >= 0xF8 && tot && tot % (spt * heads) == 0 && (uint32_t)tot * 512 <= size) {
      k.sectors = (uint8_t)spt;
      k.sides = (uint8_t)heads;
      return;
    }
  }
  // BPB が無ければサイズで決める(DSK はセクタを並べただけ)。
  // (ゲームのデータディスクは先頭セクタもデータで BPB が無く、media byte の位置に
  //  たまたま F8h などがあると片面と誤判定して別のセクタを渡し、入れ替えを受け付けなかった)
  switch (size) {
    case 737280: k.sectors = 9; k.sides = 2; return;   // 2DD 9セクタ
    case 368640: k.sectors = 9; k.sides = 1; return;   // 1DD 9セクタ
    case 655360: k.sectors = 8; k.sides = 2; return;   // 2DD 8セクタ
    case 327680: k.sectors = 8; k.sides = 1; return;   // 1DD 8セクタ
    default: break;
  }
  // 標準外のサイズのときは、ブートセクタの BPB(1トラックのセクタ数 / ヘッド数)を見る。変なら media byte で決める。
  uint16_t spt = buf[0x18] | (buf[0x19] << 8), heads = buf[0x1A] | (buf[0x1B] << 8);
  if ((spt == 8 || spt == 9) && (heads == 1 || heads == 2)) {
    k.sectors = (uint8_t)spt;
    k.sides = (uint8_t)heads;
    return;
  }
  switch (buf[0x15]) {
    case 0xF8: k.sectors = 9; k.sides = 1; break;
    case 0xF9: k.sectors = 9; k.sides = 2; break;
    case 0xFA: k.sectors = 8; k.sides = 1; break;
    case 0xFB: k.sectors = 8; k.sides = 2; break;
    case 0xFC: k.sectors = 9; k.sides = 1; break;
    case 0xFD: k.sectors = 9; k.sides = 2; break;
    case 0xFE: k.sectors = 8; k.sides = 1; break;
    case 0xFF: k.sectors = 8; k.sides = 2; break;
    default:   k.sectors = 9; k.sides = (size <= 368640) ? 1 : 2; break;
  }
}

bool wd2793HasDisk(int d) { return d >= 0 && d < 2 && disk[d].buf != nullptr; }
bool wd2793Busy() { return mode != M_IDLE; }

static Disk *selected() { return curDrive >= 0 ? &disk[curDrive] : nullptr; }

// ディスクが入っていて読める状態か(入れ替え中は入っていない扱い)
static bool ready() {
  Disk *k = selected();
  if (!k || !k->connected || !k->buf) return false;
  if (k->gap) {
    if ((int32_t)(fdcLineClock() - k->gapEnd) < 0) return false;
    k->gap = false;
  }
  return true;
}

static bool indexPulse() {
  return ready() && (fdcLineClock() % LINES_PER_ROT) < INDEX_LINES;
}

// head のトラック・side 面・sector 番のセクタ。ID のトラック番号(=ヘッドの位置)が
// トラックレジスタと違う、片面ディスクの裏面、範囲外は見つからない(Record Not Found)。
static uint8_t *sectorAtHead(uint8_t side, uint8_t sector) {
  Disk *k = selected();
  if (!k || !k->buf) return nullptr;
  if (sector < 1 || sector > k->sectors || side >= k->sides) return nullptr;
  uint32_t idx = ((uint32_t)k->head * k->sides + side) * k->sectors + (sector - 1);
  uint32_t off = idx * 512;
  if (off + 512 > k->size) return nullptr;
  return &k->buf[off];
}
static uint8_t *sectorPointer(uint8_t side, uint8_t sector) {
  Disk *k = selected();
  if (!k || regTrack != k->head) return nullptr;
  return sectorAtHead(side, sector);
}

static void moveHead(int dir) {
  Disk *k = selected();
  if (!k || !k->connected) return;
  int h = k->head + dir;
  k->head = (uint8_t)(h < 0 ? 0 : h > MAX_TRACK ? MAX_TRACK : h);
}

static void typeICommand(uint8_t cmd) {
  Disk *k = selected();
  bool connected = k && k->connected;
  bool update = (cmd & 0x10) != 0;
  regStatus = 0;
  if (cmd & 0x08) headLoaded = true;
  switch (cmd & 0xE0) {
    case 0x00:
      if (!(cmd & 0x10)) {                          // Restore: TRACK00 まで戻す
        stepDir = -1;
        if (connected) { k->head = 0; regTrack = 0; }
        else regStatus |= 0x10;                     // TRACK00 が来ない = ドライブが無い(Seek Error)
      } else {                                      // Seek: トラックレジスタがデータレジスタになるまで
        int diff = (int)regData - (int)regTrack;
        if (diff) stepDir = diff > 0 ? 1 : -1;
        moveHead(diff);
        regTrack = regData;
      }
      break;
    case 0x20: moveHead(stepDir); if (update) regTrack += stepDir; break;                  // Step
    case 0x40: stepDir = 1;  moveHead(1);  if (update) regTrack++; break;                  // Step In
    case 0x60: stepDir = -1; moveHead(-1); if (update && regTrack > 0) regTrack--; break;  // Step Out
  }
  if (cmd & 0x04) {                                 // Verify: ヘッドの位置とトラックレジスタを比べる
    headLoaded = true;
    if (!ready() || regTrack != k->head) regStatus |= 0x10;
  }
  typeI = true;
  intrq = true;
}

static void command(uint8_t cmd) {
  // INTRQ はステータス読み出しか、新しいコマンドの書き込みで下がる。
  // 前のコマンドのINTRQが残っていると、完了待ちのBIOSが「即完了」と誤認する。
  intrq = intrqHold = false;
  dataIndex = 0;

  if ((cmd & 0xF0) == 0xD0) {                       // Type IV: Force Interrupt
    mode = M_IDLE;
    typeI = true;
    regStatus = 0;
    if (cmd & 0x08) { intrq = intrqHold = true; }   // D8: すぐ割り込み(次のコマンドまで保持)
    else if (cmd & 0x04) intrq = true;              // D4: 次のインデックスで割り込み(すぐ出す)
    return;                                         // D0: 割り込みなしで止めるだけ
  }
  if ((cmd & 0x80) == 0) { typeICommand(cmd); return; }

  typeI = false;
  headLoaded = true;
  mode = M_IDLE;
  if (!ready()) { regStatus = 0x80; intrq = true; return; }

  if ((cmd & 0xC0) == 0x80) {                       // Type II: Read / Write Sector
    multiSector = (cmd & 0x10) != 0;
    sectorPtr = sectorPointer(regSide, regSector);
    if (!sectorPtr) { regStatus = 0x10; intrq = true; return; }   // Record Not Found
    mode = ((cmd & 0xE0) == 0x80) ? M_READ : M_WRITE;
    dataLen = 512;
    regStatus = 0x03;                               // BUSY + DRQ
    return;
  }
  switch (cmd & 0xF0) {
    case 0xC0: {                                    // Read Address: 次に来るセクタの ID
      Disk *k = selected();
      if (idSector < 1 || idSector > k->sectors) idSector = 1;
      idField[0] = k->head; idField[1] = regSide; idField[2] = idSector; idField[3] = 2;  // 512バイト
      idField[4] = idField[5] = 0;                  // CRC(見ない)
      idSector = (uint8_t)(idSector % k->sectors + 1);
      mode = M_READADDR;
      dataLen = 6;
      regStatus = 0x03;
      return;
    }
    case 0xF0:                                      // Write Track (フォーマット)
      mode = M_WRITETRACK;
      dataLen = TRACK_BYTES;
      regStatus = 0x03;
      return;
    default:                                        // Read Track: データは返さず、すぐ終わる
      regStatus = 0;
      intrq = true;
      return;
  }
}

static void finish(uint8_t err) {
  mode = M_IDLE;
  regStatus = err;
  intrq = true;
}

static uint8_t status() {
  if (!intrqHold) intrq = false;
  if (typeI) {
    Disk *k = selected();
    uint8_t s = regStatus & 0x18;                   // Seek Error / CRC
    if (!ready()) s |= 0xC0;                        // NOT READY (ディスクが無いと書き込み禁止にも見える)
    if (headLoaded && ready()) s |= 0x20;
    if (k && k->connected && k->head == 0) s |= 0x04;   // ドライブが無ければ TRACK00 にならない
    if (indexPulse()) s |= 0x02;
    return s;
  }
  uint8_t s = regStatus;
  if (mode != M_IDLE) {
    s |= 0x01;
    if (dataIndex < dataLen) s |= 0x02; else s &= ~0x02;
  } else {
    s &= ~0x03;
  }
  if (!ready()) s |= 0x80;
  return s;
}

static uint8_t readData() {
  if (mode == M_READADDR) {
    uint8_t v = idField[dataIndex++];
    if (dataIndex >= 6) { regSector = idField[0]; finish(0); }   // ID のトラック番号がセクタレジスタに入る
    return v;
  }
  if (mode != M_READ || !sectorPtr) return regData;
  uint8_t v = sectorPtr[dataIndex++];
  regData = v;
  if (dataIndex >= 512) {
    if (multiSector) {
      regSector++;
      sectorPtr = sectorPointer(regSide, regSector);
      if (sectorPtr) { dataIndex = 0; return v; }
      finish(0x10);                                 // 次のセクタが無い: Record Not Found で終わる
      return v;
    }
    finish(0);
  }
  return v;
}

static void writeData(uint8_t v) {
  regData = v;
  if (mode == M_WRITETRACK) {
    if (++dataIndex >= TRACK_BYTES) {
      // フォーマット: ヘッドの位置のトラック・面のセクタを E5h で埋める(ID の書き換えは扱わない)
      Disk *k = selected();
      for (int s = 1; k && s <= k->sectors; s++) {
        uint8_t *p = sectorAtHead(regSide, (uint8_t)s);
        if (p) memset(p, 0xE5, 512);
      }
      finish(0);
    }
    return;
  }
  if (mode != M_WRITE || !sectorPtr) return;
  sectorPtr[dataIndex++] = v;
  if (dataIndex >= 512) {
    if (multiSector) {
      regSector++;
      sectorPtr = sectorPointer(regSide, regSector);
      if (sectorPtr) { dataIndex = 0; return; }
      finish(0x10);
      return;
    }
    finish(0);
  }
}

uint8_t wd2793Read(int r) {
  switch (r & 7) {
    case 0: return status();
    case 1: return regTrack;
    case 2: return regSector;
    case 3: return readData();
    case 4: return 0xFE | (regSide & 1);
    case 5: {
      uint8_t v = driveReg & ~4;
      Disk *k = selected();
      bool changed = k && k->changed;
      if (k) k->changed = false;
      return changed ? v : (uint8_t)(v | 4);
    }
    case 7: {
      uint8_t v = 0xFF;
      if (intrq) v &= ~0x40;
      if (mode != M_IDLE && dataIndex < dataLen) v &= ~0x80;
      return v;
    }
    default: return 0xFF;
  }
}

void wd2793Write(int r, uint8_t v) {
  switch (r & 7) {
    case 0: command(v); break;
    case 1: regTrack = v; break;
    case 2: regSector = v; break;
    case 3: writeData(v); break;
    case 4: regSide = v & 1; break;
    case 5:
      driveReg = v;
      switch (v & 3) { case 1: curDrive = 1; break; case 3: curDrive = -1; break; default: curDrive = 0; break; }
      break;
    default: break;
  }
}

void wd2793SelectDrive(int d) { curDrive = (d == 0 || d == 1) ? d : -1; }
