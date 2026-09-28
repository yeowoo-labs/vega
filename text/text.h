#ifndef VEGA_TEXT_H
#define VEGA_TEXT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Text as people write it (docs/design/004-text.md): the Unicode properties
 * it needs (tables generated from the UCD by text/unicode.py) and the
 * algorithms over them, checked against Unicode's conformance tests. */

/* The version of the Unicode Character Database the tables came from. */
const char *vega_unicode_version(void);

/* Bidi_Class (UAX #9), in the order of text/unicode.py. */
typedef enum {
    VEGA_BIDI_L, VEGA_BIDI_R, VEGA_BIDI_AL, VEGA_BIDI_EN, VEGA_BIDI_ES, VEGA_BIDI_ET, VEGA_BIDI_AN, VEGA_BIDI_CS,
    VEGA_BIDI_NSM, VEGA_BIDI_BN, VEGA_BIDI_B, VEGA_BIDI_S, VEGA_BIDI_WS, VEGA_BIDI_ON, VEGA_BIDI_LRE, VEGA_BIDI_LRO,
    VEGA_BIDI_RLE, VEGA_BIDI_RLO, VEGA_BIDI_PDF, VEGA_BIDI_LRI, VEGA_BIDI_RLI, VEGA_BIDI_FSI, VEGA_BIDI_PDI,
    VEGA_BIDI_CLASSES
} VegaBidiClass;
VegaBidiClass vega_bidi_class(uint32_t code);
/* A paired bracket: 1 opening, 2 closing (its pair in *pair), 0 none. */
int vega_bracket(uint32_t code, uint32_t *pair);
/* The character that mirrors code (Bidi_Mirroring_Glyph), or 0. */
uint32_t vega_mirror(uint32_t code);

/* Joining_Type (Arabic and others' cursive joining: U non-joining, D dual,
 * R right, L left, C join-causing, T transparent), General_Category (in the
 * order of text/unicode.py) and Script. */
typedef enum { VEGA_JOINING_U, VEGA_JOINING_D, VEGA_JOINING_R, VEGA_JOINING_L, VEGA_JOINING_C, VEGA_JOINING_T } VegaJoining;
typedef enum {
    VEGA_CN, VEGA_LU, VEGA_LL, VEGA_LT, VEGA_LM, VEGA_LO, VEGA_MN, VEGA_MC, VEGA_ME, VEGA_ND, VEGA_NL, VEGA_NO, VEGA_PC,
    VEGA_PD, VEGA_PS, VEGA_PE, VEGA_PI, VEGA_PF, VEGA_PO, VEGA_SM, VEGA_SC, VEGA_SK, VEGA_SO, VEGA_ZS, VEGA_ZL, VEGA_ZP,
    VEGA_CC, VEGA_CF, VEGA_CS, VEGA_CO
} VegaCategory;
/* Scripts in the order Scripts.txt first names them (text/unicode.py checks
 * the ones named here). */
typedef uint8_t VegaScript;
enum { VEGA_SCRIPT_UNKNOWN = 0, VEGA_SCRIPT_COMMON = 1, VEGA_SCRIPT_INHERITED = 2, VEGA_SCRIPT_LATIN = 3,
       VEGA_SCRIPT_GREEK = 4, VEGA_SCRIPT_CYRILLIC = 5, VEGA_SCRIPT_HEBREW = 7, VEGA_SCRIPT_ARABIC = 8,
       VEGA_SCRIPT_HANGUL = 26 };
VegaJoining vega_joining(uint32_t code);
VegaCategory vega_category(uint32_t code);
VegaScript vega_script(uint32_t code);
const char *vega_script_name(VegaScript);
/* A script's ISO 15924 code as a tag: 'Latn' is 0x4C61746E. */
uint32_t vega_script_code(VegaScript);
/* Extended_Pictographic (emoji-data.txt). */
bool vega_extended_pictographic(uint32_t code);
static inline bool vega_mark(uint32_t code) {
    VegaCategory c = vega_category(code);
    return c == VEGA_MN || c == VEGA_MC || c == VEGA_ME;
}

/* Line_Break (UAX #14), in the order of text/unicode.py. */
typedef enum {
    VEGA_LB_BK, VEGA_LB_CR, VEGA_LB_LF, VEGA_LB_NL, VEGA_LB_SP, VEGA_LB_ZW, VEGA_LB_ZWJ, VEGA_LB_CM, VEGA_LB_WJ,
    VEGA_LB_GL, VEGA_LB_BA, VEGA_LB_BB, VEGA_LB_B2, VEGA_LB_HY, VEGA_LB_CB, VEGA_LB_CL, VEGA_LB_CP, VEGA_LB_EX,
    VEGA_LB_IN, VEGA_LB_NS, VEGA_LB_OP, VEGA_LB_QU, VEGA_LB_IS, VEGA_LB_NU, VEGA_LB_PO, VEGA_LB_PR, VEGA_LB_SY,
    VEGA_LB_AI, VEGA_LB_AK, VEGA_LB_AL, VEGA_LB_AP, VEGA_LB_AS, VEGA_LB_CJ, VEGA_LB_EB, VEGA_LB_EM, VEGA_LB_H2,
    VEGA_LB_H3, VEGA_LB_HL, VEGA_LB_ID, VEGA_LB_JL, VEGA_LB_JV, VEGA_LB_JT, VEGA_LB_RI, VEGA_LB_SA, VEGA_LB_SG,
    VEGA_LB_VF, VEGA_LB_VI, VEGA_LB_XX, VEGA_LB_CLASSES
} VegaLineBreak;
VegaLineBreak vega_line_break(uint32_t code);
/* $EastAsian of UAX #14: East_Asian_Width Fullwidth, Wide or Halfwidth. */
bool vega_east_asian(uint32_t code);
/* Where a line may break (UAX #14, with the number tailoring of its
 * Example 7, as Unicode's tests do): breaks[i] is the boundary before
 * codes[i], i from 0 to count (breaks has count + 1 room). */
enum { VEGA_BREAK_NONE, VEGA_BREAK_ALLOWED, VEGA_BREAK_MANDATORY };
bool vega_line_breaks(const uint32_t *codes, size_t count, uint8_t *breaks);

/* Grapheme_Cluster_Break in the low four bits (in the order of
 * text/unicode.py), Indic_Conjunct_Break in the high four; and where
 * grapheme clusters begin (UAX #29): breaks[i] is 1 before codes[i], i
 * from 0 to count. */
enum { VEGA_GCB_OTHER, VEGA_GCB_CR, VEGA_GCB_LF, VEGA_GCB_CONTROL, VEGA_GCB_EXTEND, VEGA_GCB_ZWJ, VEGA_GCB_RI,
       VEGA_GCB_PREPEND, VEGA_GCB_SPACING_MARK, VEGA_GCB_L, VEGA_GCB_V, VEGA_GCB_T, VEGA_GCB_LV, VEGA_GCB_LVT };
enum { VEGA_INCB_NONE, VEGA_INCB_LINKER, VEGA_INCB_CONSONANT, VEGA_INCB_EXTEND };
uint8_t vega_grapheme(uint32_t code);
void vega_graphemes(const uint32_t *codes, size_t count, uint8_t *breaks);

/* Word_Break (in the order of text/unicode.py), and where words begin and
 * end (UAX #29): breaks[i] is 1 before codes[i], i from 0 to count. */
enum { VEGA_WB_OTHER, VEGA_WB_CR, VEGA_WB_LF, VEGA_WB_NEWLINE, VEGA_WB_EXTEND, VEGA_WB_ZWJ, VEGA_WB_RI, VEGA_WB_FORMAT,
       VEGA_WB_KATAKANA, VEGA_WB_HEBREW_LETTER, VEGA_WB_ALETTER, VEGA_WB_SINGLE_QUOTE, VEGA_WB_DOUBLE_QUOTE,
       VEGA_WB_MIDNUMLET, VEGA_WB_MIDLETTER, VEGA_WB_MIDNUM, VEGA_WB_NUMERIC, VEGA_WB_EXTENDNUMLET, VEGA_WB_WSEGSPACE };
uint8_t vega_word(uint32_t code);
bool vega_words(const uint32_t *codes, size_t count, uint8_t *breaks);

/* Sentence_Break (in the order of text/unicode.py), and where sentences
 * begin (UAX #29): breaks[i] is 1 before codes[i], i from 0 to count. */
enum { VEGA_SB_OTHER, VEGA_SB_CR, VEGA_SB_LF, VEGA_SB_EXTEND, VEGA_SB_SEP, VEGA_SB_FORMAT, VEGA_SB_SP, VEGA_SB_LOWER,
       VEGA_SB_UPPER, VEGA_SB_OLETTER, VEGA_SB_NUMERIC, VEGA_SB_ATERM, VEGA_SB_SCONTINUE, VEGA_SB_STERM, VEGA_SB_CLOSE };
uint8_t vega_sentence(uint32_t code);
bool vega_sentences(const uint32_t *codes, size_t count, uint8_t *breaks);

/* Canonical_Combining_Class; a code point's full canonical decomposition
 * (0 when it has none; Hangul syllables are left to vega_nfd); the primary
 * composite of two code points, or 0. */
uint8_t vega_combining_class(uint32_t code);
size_t vega_decomposition(uint32_t code, const uint32_t **out);
uint32_t vega_composition(uint32_t first, uint32_t second);
/* A code point's canonical decomposition mapping, one step: into first and
 * second, or first alone (second 0); false when it has none (Hangul
 * syllables included: they are left to arithmetic). */
bool vega_decomposition_step(uint32_t code, uint32_t *first, uint32_t *second);
/* Normalization forms D and C (UAX #15): out has room for 4 * count code
 * points (the most one decomposes into); returns how many it holds. */
size_t vega_nfd(const uint32_t *in, size_t count, uint32_t *out);
size_t vega_nfc(const uint32_t *in, size_t count, uint32_t *out);

/* The embedding levels of one paragraph by the Unicode Bidirectional
 * Algorithm (UAX #9, rules P2 to I2), then L1 as for one line: classes are
 * its characters' Bidi_Class; codes, when given, are the characters, so
 * that paired brackets resolve (rule N0); direction is VEGA_BIDI_AUTO (from
 * its first strong character, P2-P3), VEGA_BIDI_LTR or VEGA_BIDI_RTL. Each
 * character's level goes to levels, VEGA_BIDI_REMOVED for those rule X9
 * removes (embedding controls and boundary neutrals). Returns the
 * paragraph's level, or -1 without memory. */
enum { VEGA_BIDI_AUTO = 0, VEGA_BIDI_LTR = 1, VEGA_BIDI_RTL = 2, VEGA_BIDI_REMOVED = 0xff };
int vega_bidi_levels(const VegaBidiClass *classes, const uint32_t *codes, size_t count, int direction, uint8_t *levels);
/* The visual order of one line (rule L2), left to right: the positions of
 * its characters, those removed left out; returns how many. */
size_t vega_bidi_order(const uint8_t *levels, size_t count, uint32_t *order);
#endif
