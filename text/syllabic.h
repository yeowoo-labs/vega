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
extern const VegaMachine vega_indic_machine, vega_khmer_machine, vega_myanmar_machine;
#endif
