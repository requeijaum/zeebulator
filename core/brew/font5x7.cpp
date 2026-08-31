#include "core/brew/font5x7.h"

namespace zeebulator {

namespace {

// Each row is a 5-bit pattern, MSB (bit 4) = leftmost column. Written as
// binary literals so the bit pattern lines up visually with the ASCII
// art in each comment (# = set bit, . = clear bit).
using Glyph = uint8_t[7];

constexpr Glyph kDigit0 = {
    // .###.
    0b01110,
    // #...#
    0b10001,
    // #..##
    0b10011,
    // #.#.#
    0b10101,
    // ##..#
    0b11001,
    // #...#
    0b10001,
    // .###.
    0b01110,
};
constexpr Glyph kDigit1 = {
    0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111,
};
constexpr Glyph kDigit2 = {
    0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111,
};
constexpr Glyph kDigit3 = {
    0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110,
};
constexpr Glyph kDigit4 = {
    0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010,
};
constexpr Glyph kDigit5 = {
    0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110,
};
constexpr Glyph kDigit6 = {
    0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110,
};
constexpr Glyph kDigit7 = {
    0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000,
};
constexpr Glyph kDigit8 = {
    0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110,
};
constexpr Glyph kDigit9 = {
    0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100,
};

constexpr Glyph kLetterA = {
    0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001,
};
constexpr Glyph kLetterB = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110,
};
constexpr Glyph kLetterC = {
    0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111,
};
constexpr Glyph kLetterD = {
    0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110,
};
constexpr Glyph kLetterE = {
    0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111,
};
constexpr Glyph kLetterF = {
    0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000,
};
constexpr Glyph kLetterG = {
    0b01111, 0b10000, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111,
};
constexpr Glyph kLetterH = {
    0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001,
};
constexpr Glyph kLetterI = {
    0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111,
};
constexpr Glyph kLetterJ = {
    0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100,
};
constexpr Glyph kLetterK = {
    0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001,
};
constexpr Glyph kLetterL = {
    0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111,
};
constexpr Glyph kLetterM = {
    0b10001, 0b11011, 0b10101, 0b10001, 0b10001, 0b10001, 0b10001,
};
constexpr Glyph kLetterN = {
    0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001,
};
constexpr Glyph kLetterO = {
    0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110,
};
constexpr Glyph kLetterP = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000,
};
constexpr Glyph kLetterQ = {
    0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101,
};
constexpr Glyph kLetterR = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001,
};
constexpr Glyph kLetterS = {
    0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110,
};
constexpr Glyph kLetterT = {
    0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100,
};
constexpr Glyph kLetterU = {
    0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110,
};
constexpr Glyph kLetterV = {
    0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100,
};
constexpr Glyph kLetterW = {
    0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010,
};
constexpr Glyph kLetterX = {
    0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001,
};
constexpr Glyph kLetterY = {
    0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100,
};
constexpr Glyph kLetterZ = {
    0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111,
};

// Fallback for any unmapped character (punctuation, lowercase, ...): a
// small centered box, distinguishable from both a blank space and a
// real letter/digit.
constexpr Glyph kFallback = {
    0b00000, 0b01110, 0b01010, 0b01010, 0b01110, 0b00000, 0b00000,
};

}  // namespace

const uint8_t* GetGlyph5x7(char c) {
  switch (c) {
    case '0': return kDigit0;
    case '1': return kDigit1;
    case '2': return kDigit2;
    case '3': return kDigit3;
    case '4': return kDigit4;
    case '5': return kDigit5;
    case '6': return kDigit6;
    case '7': return kDigit7;
    case '8': return kDigit8;
    case '9': return kDigit9;
    case 'A': return kLetterA;
    case 'B': return kLetterB;
    case 'C': return kLetterC;
    case 'D': return kLetterD;
    case 'E': return kLetterE;
    case 'F': return kLetterF;
    case 'G': return kLetterG;
    case 'H': return kLetterH;
    case 'I': return kLetterI;
    case 'J': return kLetterJ;
    case 'K': return kLetterK;
    case 'L': return kLetterL;
    case 'M': return kLetterM;
    case 'N': return kLetterN;
    case 'O': return kLetterO;
    case 'P': return kLetterP;
    case 'Q': return kLetterQ;
    case 'R': return kLetterR;
    case 'S': return kLetterS;
    case 'T': return kLetterT;
    case 'U': return kLetterU;
    case 'V': return kLetterV;
    case 'W': return kLetterW;
    case 'X': return kLetterX;
    case 'Y': return kLetterY;
    case 'Z': return kLetterZ;
    case ' ': return nullptr;
    default: return kFallback;
  }
}

}  // namespace zeebulator
