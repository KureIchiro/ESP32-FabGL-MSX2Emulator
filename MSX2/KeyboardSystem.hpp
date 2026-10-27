#pragma once

#include <Arduino.h>

enum class KeyboardLayout {
    JAPANESE,
    US_ENGLISH,
    PORTUGUESE,
    SPANISH,
    ITALIAN
};

inline uint8_t key_map[256];
inline uint8_t key_map_e0[256];

void initKeyMap(KeyboardLayout layout);
void setJapaneseKeyMap();
void setUSKeyMap();
void setPortugueseKeyMap();
void setSpanishKeyMap();
void setItalianKeyMap();

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

inline void setCommonKeyMap() {
    key_map[0x45] = 0x00; key_map[0x16] = 0x01; key_map[0x1E] = 0x02; key_map[0x26] = 0x03;
    key_map[0x25] = 0x04; key_map[0x2E] = 0x05; key_map[0x36] = 0x06; key_map[0x3D] = 0x07;
    key_map[0x3E] = 0x10; key_map[0x46] = 0x11; key_map[0x4E] = 0x12;
    key_map[0x41] = 0x22; key_map[0x49] = 0x23; key_map[0x4A] = 0x24;
    key_map[0x1C] = 0x26; key_map[0x32] = 0x27;
    key_map[0x21] = 0x30; key_map[0x23] = 0x31; key_map[0x24] = 0x32; key_map[0x2B] = 0x33;
    key_map[0x34] = 0x34; key_map[0x33] = 0x35; key_map[0x43] = 0x36; key_map[0x3B] = 0x37;
    key_map[0x42] = 0x40; key_map[0x4B] = 0x41; key_map[0x3A] = 0x42; key_map[0x31] = 0x43;
    key_map[0x44] = 0x44; key_map[0x4D] = 0x45; key_map[0x15] = 0x46; key_map[0x2D] = 0x47;
    key_map[0x1B] = 0x50; key_map[0x2C] = 0x51; key_map[0x3C] = 0x52; key_map[0x2A] = 0x53;
    key_map[0x1D] = 0x54; key_map[0x22] = 0x55; key_map[0x35] = 0x56; key_map[0x1A] = 0x57;
    key_map[0x12] = 0x60;
    key_map[0x59] = 0x60;
    key_map[0x14] = 0x61;
    key_map_e0[0x14] = 0x61;
    key_map[0x11] = 0x62;
    key_map[0x58] = 0x63;
    key_map[0x05] = 0x65; key_map[0x06] = 0x66; key_map[0x04] = 0x67;
    key_map[0x0C] = 0x70; key_map[0x03] = 0x71;
    key_map[0x76] = 0x72;
    key_map[0x0D] = 0x73;
    key_map_e0[0x7D] = 0x74;
    key_map_e0[0x7A] = 0x74;
    key_map[0x66] = 0x75;
    key_map_e0[0x69] = 0x76;
    key_map[0x5A] = 0x77;
    key_map_e0[0x5A] = 0x77;
    key_map[0x29] = 0x80;
    key_map_e0[0x6C] = 0x81;
    key_map_e0[0x70] = 0x82;
    key_map_e0[0x71] = 0x83;
    key_map_e0[0x6B] = 0x84;
    key_map_e0[0x75] = 0x85;
    key_map_e0[0x72] = 0x86;
    key_map_e0[0x74] = 0x87;
    key_map[0x7C] = 0x90;
    key_map[0x79] = 0x91;
    key_map_e0[0x4A] = 0x92;
    key_map[0x70] = 0x93; key_map[0x69] = 0x94; key_map[0x72] = 0x95; key_map[0x7A] = 0x96;
    key_map[0x6B] = 0x97; key_map[0x73] = 0xA0; key_map[0x74] = 0xA1; key_map[0x6C] = 0xA2;
    key_map[0x75] = 0xA3; key_map[0x7D] = 0xA4;
    key_map[0x7B] = 0xA5;
    key_map[0x71] = 0xA7;
}

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

inline void setJapaneseKeyMap() {
    key_map[0x55] = 0x13;
    key_map[0x6A] = 0x14;
    key_map[0x54] = 0x15;
    key_map[0x5B] = 0x16;
    key_map[0x4C] = 0x17;
    key_map[0x52] = 0x20;
    key_map[0x5D] = 0x21;
    key_map[0x51] = 0x25;
    key_map[0x13] = 0x64;
    key_map[0x0E] = 0x64;
    key_map[0x64] = 0x62;
    key_map[0x67] = 0x62;
    key_map_e0[0x11] = 0x62;
}

inline void setUSKeyMap() {
    key_map[0x55] = 0x13;
    key_map[0x5D] = 0x14;
    key_map[0x54] = 0x15;
    key_map[0x5B] = 0x16;
    key_map[0x4C] = 0x17;
    key_map[0x52] = 0x20;
    key_map[0x0E] = 0x21;
    key_map[0x61] = 0x25;
    key_map_e0[0x11] = 0x64;
}

inline void setPortugueseKeyMap() {
    setUSKeyMap();
    key_map[0x51] = 0x25;
    key_map[0x71] = 0xA6;
    key_map[0x6D] = 0xA7;
}

inline void setSpanishKeyMap() {
    setUSKeyMap();
}

inline void setItalianKeyMap() {
    setUSKeyMap();
}
