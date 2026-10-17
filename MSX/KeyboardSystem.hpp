#pragma once

#include <Arduino.h>

// ============================================================================
//  PS/2(スキャンコードセット2)→ MSX キーマトリクス(MSX1 / MSX2 共通。両方のフォルダに同じファイル)
//
//  MSX のキーマトリクス(日本・各国共通の並び。行 0〜10)
//    行0: 0 1 2 3 4 5 6 7
//    行1: 8 9 - ^ ¥ @ [ ;        (各国版: 8 9 - = \ [ ] ;)
//    行2: : ] , . / _ A B        (各国版: ' ` , . / DEAD A B)
//    行3: C D E F G H I J   行4: K L M N O P Q R   行5: S T U V W X Y Z
//    行6: SHIFT CTRL GRAPH CAPS かな(各国版: CODE) F1 F2 F3
//    行7: F4 F5 ESC TAB STOP BS SELECT RETURN
//    行8: SPACE HOME INS DEL ← ↑ ↓ →
//    行9: テンキー * + / 0 1 2 3 4    行10: テンキー 5 6 7 8 9 - , .
//  各国版 MSX は、同じ位置のキーが同じマトリクスの位置につながっていて、印字(文字)だけが国ごとに
//  違う(例: L の右のキーは US の ; / スペインの Ñ / ブラジルの Ç で、どれも 行1 bit7)。
//  そこで日本語以外は PC キーボードの「位置」で各国版 MSX のマトリクスへ割り当てる。
//  どの文字になるかは、その国の MSX の BIOS(MSX.ROM)が決める(日本の BIOS では日本の文字になる)。
//
//  key_map[sc]    : E0 の付かないスキャンコード → 0xRB(R = 行、B = ビット)、0xFF = 割り当てなし
//  key_map_e0[sc] : E0 の付くスキャンコード(カーソル・Home・テンキーの / と Enter・右 Alt など)
// ============================================================================

// --- 1. Keyboard Types ---
enum class KeyboardLayout {
    JAPANESE,    // 日本語 106/109 キーボード + 日本の MSX
    US_ENGLISH,  // US 101/104 キーボード + 各国版(インターナショナル)MSX
    PORTUGUESE,  // ブラジル ABNT2 キーボード + ブラジルの MSX(Gradiente Expert など)
    SPANISH,     // スペイン語キーボード + スペインの MSX
    ITALIAN      // イタリア語キーボード + イタリアの MSX(Philips VG-8020 イタリア版など)
};

// Global key maps
inline uint8_t key_map[256];
inline uint8_t key_map_e0[256];

// --- 2. Function Prototypes ---
void initKeyMap(KeyboardLayout layout);
void setJapaneseKeyMap();
void setUSKeyMap();
void setPortugueseKeyMap();
void setSpanishKeyMap();
void setItalianKeyMap();

// --- 3. Implementations ---

// MSX のキーの名前(0xRB → 起動メニューの表示用。0xFF = なし)。日本の MSX の印字で、
// 行1・行2 の記号は各国版では印字が違う(位置は同じ)。
inline const char *msxKeyName(uint8_t rb) {
    static const char *const names[11][8] = {
        { "0", "1", "2", "3", "4", "5", "6", "7" },
        { "8", "9", "-", "^", "\\", "@", "[", ";" },
        { ":", "]", ",", ".", "/", "_", "A", "B" },
        { "C", "D", "E", "F", "G", "H", "I", "J" },
        { "K", "L", "M", "N", "O", "P", "Q", "R" },
        { "S", "T", "U", "V", "W", "X", "Y", "Z" },
        { "SHIFT", "CTRL", "GRAPH", "CAPS", "KANA/CODE", "F1", "F2", "F3" },
        { "F4", "F5", "ESC", "TAB", "STOP", "BS", "SELECT", "RETURN" },
        { "SPACE", "HOME", "INS", "DEL", "LEFT", "UP", "DOWN", "RIGHT" },
        { "NUM *", "NUM +", "NUM /", "NUM 0", "NUM 1", "NUM 2", "NUM 3", "NUM 4" },
        { "NUM 5", "NUM 6", "NUM 7", "NUM 8", "NUM 9", "NUM -", "NUM ,", "NUM ." },
    };
    if (rb == 0xFF) return "NONE";
    return (rb >> 4) < 11 ? names[rb >> 4][rb & 7] : "?";
}

// どの国でも同じキー(英字・数字・ファンクション・テンキー・カーソルなど)
inline void setCommonKeyMap() {
    // 行0: 0〜7
    key_map[0x45] = 0x00; key_map[0x16] = 0x01; key_map[0x1E] = 0x02; key_map[0x26] = 0x03;
    key_map[0x25] = 0x04; key_map[0x2E] = 0x05; key_map[0x36] = 0x06; key_map[0x3D] = 0x07;
    // 行1: 8 9 -
    key_map[0x3E] = 0x10; key_map[0x46] = 0x11; key_map[0x4E] = 0x12;
    // 行2: , . / A B
    key_map[0x41] = 0x22; key_map[0x49] = 0x23; key_map[0x4A] = 0x24;
    key_map[0x1C] = 0x26; key_map[0x32] = 0x27;
    // 行3〜5: C〜Z
    key_map[0x21] = 0x30; key_map[0x23] = 0x31; key_map[0x24] = 0x32; key_map[0x2B] = 0x33;
    key_map[0x34] = 0x34; key_map[0x33] = 0x35; key_map[0x43] = 0x36; key_map[0x3B] = 0x37;
    key_map[0x42] = 0x40; key_map[0x4B] = 0x41; key_map[0x3A] = 0x42; key_map[0x31] = 0x43;
    key_map[0x44] = 0x44; key_map[0x4D] = 0x45; key_map[0x15] = 0x46; key_map[0x2D] = 0x47;
    key_map[0x1B] = 0x50; key_map[0x2C] = 0x51; key_map[0x3C] = 0x52; key_map[0x2A] = 0x53;
    key_map[0x1D] = 0x54; key_map[0x22] = 0x55; key_map[0x35] = 0x56; key_map[0x1A] = 0x57;
    // 行6: SHIFT CTRL GRAPH CAPS (かな/CODE は国ごと) F1 F2 F3
    key_map[0x12] = 0x60;  // 左 SHIFT
    key_map[0x59] = 0x60;  // 右 SHIFT
    key_map[0x14] = 0x61;  // 左 CTRL
    key_map_e0[0x14] = 0x61;  // 右 CTRL
    key_map[0x11] = 0x62;  // 左 Alt → GRAPH
    key_map[0x58] = 0x63;  // CAPS
    key_map[0x05] = 0x65; key_map[0x06] = 0x66; key_map[0x04] = 0x67;   // F1 F2 F3
    // 行7: F4 F5 ESC TAB STOP BS SELECT RETURN
    key_map[0x0C] = 0x70; key_map[0x03] = 0x71;   // F4 F5
    key_map[0x76] = 0x72;  // ESC
    key_map[0x0D] = 0x73;  // TAB
    key_map_e0[0x7D] = 0x74;  // PageUp → STOP
    key_map_e0[0x7A] = 0x74;  // PageDown → STOP
    key_map[0x66] = 0x75;  // BackSpace → BS
    key_map_e0[0x69] = 0x76;  // End → SELECT
    key_map[0x5A] = 0x77;  // Enter → RETURN
    key_map_e0[0x5A] = 0x77;  // テンキー Enter → RETURN
    // 行8: SPACE HOME INS DEL ← ↑ ↓ →
    key_map[0x29] = 0x80;
    key_map_e0[0x6C] = 0x81;  // Home
    key_map_e0[0x70] = 0x82;  // Insert
    key_map_e0[0x71] = 0x83;  // Delete
    key_map_e0[0x6B] = 0x84;  // ←
    key_map_e0[0x75] = 0x85;  // ↑
    key_map_e0[0x72] = 0x86;  // ↓
    key_map_e0[0x74] = 0x87;  // →
    // 行9 / 行10: テンキー
    key_map[0x7C] = 0x90;  // *
    key_map[0x79] = 0x91;  // +
    key_map_e0[0x4A] = 0x92;  // /
    key_map[0x70] = 0x93; key_map[0x69] = 0x94; key_map[0x72] = 0x95; key_map[0x7A] = 0x96;   // 0 1 2 3
    key_map[0x6B] = 0x97; key_map[0x73] = 0xA0; key_map[0x74] = 0xA1; key_map[0x6C] = 0xA2;   // 4 5 6 7
    key_map[0x75] = 0xA3; key_map[0x7D] = 0xA4;                                               // 8 9
    key_map[0x7B] = 0xA5;  // -
    key_map[0x71] = 0xA7;  // .
}

/**
 * Initialize key map based on the selected layout.
 */
inline void initKeyMap(KeyboardLayout layout) {
    memset(key_map, 0xFF, 256);
    memset(key_map_e0, 0xFF, 256);
    setCommonKeyMap();

    switch (layout) {
        case KeyboardLayout::JAPANESE:
            setJapaneseKeyMap();
            break;
        case KeyboardLayout::US_ENGLISH:
            setUSKeyMap();
            break;
        case KeyboardLayout::PORTUGUESE:
            setPortugueseKeyMap();
            break;
        case KeyboardLayout::SPANISH:
            setSpanishKeyMap();
            break;
        case KeyboardLayout::ITALIAN:
            setItalianKeyMap();
            break;
        default:
            setJapaneseKeyMap();
            break;
    }
}

/**
 * 日本語 106/109 キーボード → 日本の MSX
 */
inline void setJapaneseKeyMap() {
    // 行1: ^ ¥ @ [ ;
    key_map[0x55] = 0x13;  // ^
    key_map[0x6A] = 0x14;  // ¥
    key_map[0x54] = 0x15;  // @
    key_map[0x5B] = 0x16;  // [
    key_map[0x4C] = 0x17;  // ;
    // 行2: : ] _
    key_map[0x52] = 0x20;  // :
    key_map[0x5D] = 0x21;  // ]
    key_map[0x51] = 0x25;  // _ (ろ)
    // 行6: かな / GRAPH
    key_map[0x13] = 0x64;  // カタカナ/ひらがな → かな
    key_map[0x0E] = 0x64;  // 半角/全角 → かな
    key_map[0x64] = 0x62;  // 変換 → GRAPH
    key_map[0x67] = 0x62;  // 無変換 → GRAPH
    key_map_e0[0x11] = 0x62;  // 右 Alt → GRAPH
}

/**
 * US 101/104 キーボード → 各国版(インターナショナル)MSX
 */
inline void setUSKeyMap() {
    // 行1: = \ [ ] ;
    key_map[0x55] = 0x13;  // =
    key_map[0x5D] = 0x14;  // \ (Enter の上)
    key_map[0x54] = 0x15;  // [
    key_map[0x5B] = 0x16;  // ]
    key_map[0x4C] = 0x17;  // ;
    // 行2: ' ` DEAD
    key_map[0x52] = 0x20;  // '
    key_map[0x0E] = 0x21;  // `
    key_map[0x61] = 0x25;  // ISO キーボードの < > (左 SHIFT の右)→ DEAD
    // 行6: CODE
    key_map_e0[0x11] = 0x64;  // 右 Alt (AltGr) → CODE
}

/**
 * ブラジル ABNT2 キーボード → ブラジルの MSX(位置で割り当て)
 *   L の右(0x4C)= Ç、その右(0x52)= ~ ^、Enter の左上(0x5D)= ] }、
 *   右 SHIFT の左(0x51)= / ?、テンキーの , (0x71)と . (0x6D)
 */
inline void setPortugueseKeyMap() {
    setUSKeyMap();
    key_map[0x51] = 0x25;  // / ? (ABNT2 だけにあるキー)→ DEAD の位置
    key_map[0x71] = 0xA6;  // テンキー ,
    key_map[0x6D] = 0xA7;  // テンキー .
}

/**
 * スペイン語キーボード → スペインの MSX(位置で割り当て)
 *   L の右(0x4C)= Ñ、P の右(0x54 / 0x5B)= ` ^ と + *、Ñ の右(0x52)= ´ ¨、
 *   左 SHIFT の右(0x61)= < >
 */
inline void setSpanishKeyMap() {
    setUSKeyMap();
}

/**
 * イタリア語キーボード → イタリアの MSX(位置で割り当て)
 *   0 の右(0x4E / 0x55)= ' ? と ì ^、P の右(0x54 / 0x5B)= è é と + *、
 *   L の右(0x4C / 0x52)= ò ç と à °、Enter の左(0x5D)= ù §、1 の左(0x0E)= \ |、
 *   左 SHIFT の右(0x61)= < >、右 Alt(AltGr)= CODE
 */
inline void setItalianKeyMap() {
    setUSKeyMap();
}
