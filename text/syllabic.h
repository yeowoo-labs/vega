#ifndef VEGA_SYLLABIC_H
#define VEGA_SYLLABIC_H
#include <stdint.h>

/* What HarfBuzz's syllabic shapers read, as text/syllabic.py makes it
 * (text/syllabic-data.c): characters' categories, and the machines that
 * find syllables in runs of them. */

/* Categories (hb-ot-shaper-indic.hh's ot_category_t: Indic's, and Khmer's
 * and Myanmar's after them) and positions (ot_position_t). */
enum {
    VEGA_OT_X = 0, VEGA_OT_C = 1, VEGA_OT_V = 2, VEGA_OT_N = 3, VEGA_OT_H = 4, VEGA_OT_ZWNJ = 5, VEGA_OT_ZWJ = 6,
    VEGA_OT_M = 7, VEGA_OT_SM = 8, VEGA_OT_A = 9, VEGA_OT_VD = 9, VEGA_OT_PLACEHOLDER = 10, VEGA_OT_DOTTEDCIRCLE = 11,
    VEGA_OT_RS = 12, VEGA_OT_MPST = 13, VEGA_OT_REPHA = 14, VEGA_OT_RA = 15, VEGA_OT_CM = 16, VEGA_OT_SYMBOL = 17,
    VEGA_OT_CS = 18, VEGA_OT_SMPST = 57, VEGA_OT_VABV = 20, VEGA_OT_VBLW = 21, VEGA_OT_VPRE = 22, VEGA_OT_VPST = 23, VEGA_OT_ROBATIC = 25,
    VEGA_OT_XGROUP = 26, VEGA_OT_YGROUP = 27, VEGA_OT_AS = 32, VEGA_OT_MH = 35, VEGA_OT_MR = 36, VEGA_OT_MW = 37,
    VEGA_OT_MY = 38, VEGA_OT_PT = 39, VEGA_OT_VS = 40, VEGA_OT_ML = 41
};
enum {
    VEGA_POS_START, VEGA_POS_RA_TO_BECOME_REPH, VEGA_POS_PRE_M, VEGA_POS_PRE_C, VEGA_POS_BASE_C, VEGA_POS_AFTER_MAIN,
    VEGA_POS_ABOVE_C, VEGA_POS_BEFORE_SUB, VEGA_POS_BELOW_C, VEGA_POS_AFTER_SUB, VEGA_POS_BEFORE_POST, VEGA_POS_POST_C,
    VEGA_POS_AFTER_POST, VEGA_POS_SMVD, VEGA_POS_END
};
/* A character's category | position << 8 (hb_indic_get_categories). */
uint16_t vega_indic_categories(uint32_t u);

/* The Universal Shaping Engine's categories (hb-ot-shaper-use-machine.rl). */
enum {
    VEGA_USE_O = 0, VEGA_USE_B = 1, VEGA_USE_N = 4, VEGA_USE_GB = 5, VEGA_USE_CGJ = 6, VEGA_USE_SUB = 11,
    VEGA_USE_H = 12, VEGA_USE_HN = 13, VEGA_USE_ZWNJ = 14, VEGA_USE_WJ = 16, VEGA_USE_R = 18, VEGA_USE_VPRE = 22,
    VEGA_USE_VMPRE = 23, VEGA_USE_FABV = 24, VEGA_USE_FBLW = 25, VEGA_USE_FPST = 26, VEGA_USE_MABV = 27,
    VEGA_USE_MBLW = 28, VEGA_USE_MPST = 29, VEGA_USE_MPRE = 30, VEGA_USE_CMABV = 31, VEGA_USE_CMBLW = 32,
    VEGA_USE_VABV = 33, VEGA_USE_VBLW = 34, VEGA_USE_VPST = 35, VEGA_USE_VMABV = 37, VEGA_USE_VMBLW = 38,
    VEGA_USE_VMPST = 39, VEGA_USE_SMABV = 41, VEGA_USE_SMBLW = 42, VEGA_USE_CS = 43, VEGA_USE_IS = 44,
    VEGA_USE_FMABV = 45, VEGA_USE_FMBLW = 46, VEGA_USE_FMPST = 47, VEGA_USE_SK = 48, VEGA_USE_G = 49, VEGA_USE_J = 50,
    VEGA_USE_SB = 51, VEGA_USE_SE = 52, VEGA_USE_HVM = 53, VEGA_USE_HM = 54, VEGA_USE_HR = 55, VEGA_USE_RK = 56
};
/* A character's USE category (hb_use_get_category). */
uint8_t vega_use_category(uint32_t u);

/* A syllable machine: a DFA over classes of symbols (categories, those
 * past 63 taken as 63). From each position the longest run it accepts is
 * a syllable, ties to the pattern its grammar lists first (Ragel's
 * scanner), which the accepting state already says. */
typedef struct {
    const uint8_t *class_of; /* [64] */
    unsigned classes, states;
    const void *next;        /* [states][classes]: the next state, states when none */
    unsigned width;          /* bytes of a next state: 1 or 2 */
    const uint8_t *accept;   /* [states]: the syllable type a state ends, plus one; 0 for none */
    unsigned broken;         /* the broken cluster's type, plus one */
} VegaMachine;
extern const VegaMachine vega_indic_machine, vega_khmer_machine, vega_myanmar_machine, vega_use_machine;

/* Arabic's joining type (hb-ot-shaper-arabic-table.hh): U 0, L 1, R 2, D and
 * C 3, Syriac's Alaph 4 and Dalath-Rish 5, T 7, and 8 where the general
 * category decides (T for Mn, Me and Cf, else U). */
uint8_t vega_arabic_joining(uint32_t u);
/* For fallback shaping: the presentation forms (init, medi, fina, isol) of
 * U+0621..06D3, and the ligatures of presentation forms, in HarfBuzz's
 * order: those of three components, of two, of marks. */
enum { VEGA_ARABIC_FORMS_FIRST = 0x0621, VEGA_ARABIC_FORMS_LAST = 0x06D3 };
extern const uint16_t vega_arabic_forms[VEGA_ARABIC_FORMS_LAST - VEGA_ARABIC_FORMS_FIRST + 1][4];
typedef struct { uint16_t first, ligature, components[2]; } VegaArabicLigature;
typedef struct { const VegaArabicLigature *ligatures; unsigned count, length; } VegaArabicLigatures;
extern const VegaArabicLigatures vega_arabic_ligatures[3];
#endif
