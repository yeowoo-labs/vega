/* Line breaking (UAX #14, Unicode 15.1): text/text.h. Each boundary is
 * decided by the first rule, in the standard's order, that matches it; the
 * rules that look back over spaces, marks or numbers read what a pass
 * before them worked out for every position. */
#include "text.h"
#include <stdlib.h>

enum { BK = VEGA_LB_BK, CR = VEGA_LB_CR, LF = VEGA_LB_LF, NL = VEGA_LB_NL, SP = VEGA_LB_SP, ZW = VEGA_LB_ZW,
       ZWJ = VEGA_LB_ZWJ, CM = VEGA_LB_CM, WJ = VEGA_LB_WJ, GL = VEGA_LB_GL, BA = VEGA_LB_BA, BB = VEGA_LB_BB,
       B2 = VEGA_LB_B2, HY = VEGA_LB_HY, CB = VEGA_LB_CB, CL = VEGA_LB_CL, CP = VEGA_LB_CP, EX = VEGA_LB_EX,
       IN = VEGA_LB_IN, NS = VEGA_LB_NS, OP = VEGA_LB_OP, QU = VEGA_LB_QU, IS = VEGA_LB_IS, NU = VEGA_LB_NU,
       PO = VEGA_LB_PO, PR = VEGA_LB_PR, SY = VEGA_LB_SY, AK = VEGA_LB_AK, AL = VEGA_LB_AL, AP = VEGA_LB_AP,
       AS = VEGA_LB_AS, EB = VEGA_LB_EB, EM = VEGA_LB_EM, H2 = VEGA_LB_H2, H3 = VEGA_LB_H3, HL = VEGA_LB_HL,
       ID = VEGA_LB_ID, JL = VEGA_LB_JL, JV = VEGA_LB_JV, JT = VEGA_LB_JT, RI = VEGA_LB_RI, VF = VEGA_LB_VF,
       VI = VEGA_LB_VI };
enum { NONE = VEGA_BREAK_NONE, ALLOWED = VEGA_BREAK_ALLOWED, MANDATORY = VEGA_BREAK_MANDATORY };
#define NO_UNIT SIZE_MAX

/* LB1: ambiguous, surrogate and unknown are alphabetic; complex context is
 * a mark if it is one, else alphabetic; conditional Japanese starters are
 * nonstarters. */
static uint8_t resolve(uint32_t code) {
    VegaLineBreak c = vega_line_break(code);
    switch (c) {
    case VEGA_LB_AI: case VEGA_LB_SG: case VEGA_LB_XX: return AL;
    case VEGA_LB_SA: {
        VegaCategory g = vega_category(code);
        return g == VEGA_MN || g == VEGA_MC ? CM : AL;
    }
    case VEGA_LB_CJ: return NS;
    default: return (uint8_t)c;
    }
}

typedef struct {
    const uint32_t *codes;
    size_t count;
    uint8_t *cls;     /* each character's class (LB1) */
    uint8_t *eff;     /* its unit's class: a mark takes its base's (LB9), a mark with none is alphabetic (LB10) */
    uint8_t *joined;  /* a mark that LB9 joins to the character before */
    uint8_t *number;  /* its unit ends NU (NU | SY | IS)* */
    size_t *start;    /* where its unit starts */
    size_t *nonspace; /* where the last unit up to it that is not a space starts, or NO_UNIT */
    size_t *ri;       /* how many regional indicators end at its unit */
} Text;

static bool one_of(unsigned c, unsigned a, unsigned b) { return c == a || c == b; }

/* The unit after the one at i, or count. */
static size_t next_unit(const Text *t, size_t i) {
    size_t j = i + 1;
    while (j < t->count && t->joined[j]) j++;
    return j;
}

static bool aksara_start(const Text *t, size_t unit) {
    unsigned c = t->eff[unit];
    return c == AK || c == AS || t->codes[unit] == 0x25CC;
}

static uint8_t decide(const Text *t, size_t i) {
    unsigned a = t->cls[i - 1], b = t->cls[i], A = t->eff[i - 1], B = t->eff[i];
    size_t unit = t->start[i - 1], before = t->nonspace[i - 1];
    uint32_t base = t->codes[unit];
    if (a == BK) return MANDATORY;                                         /* LB4 */
    if (a == CR && b == LF) return NONE;                                   /* LB5 */
    if (a == CR || a == LF || a == NL) return MANDATORY;
    if (b == BK || b == CR || b == LF || b == NL) return NONE;             /* LB6 */
    if (b == SP || b == ZW) return NONE;                                   /* LB7 */
    if (before != NO_UNIT && t->eff[before] == ZW) return ALLOWED;         /* LB8: ZW SP* ÷ */
    if (a == ZWJ) return NONE;                                             /* LB8a */
    if (t->joined[i]) return NONE;                                         /* LB9 */
    if (A == WJ || B == WJ) return NONE;                                   /* LB11 */
    if (A == GL) return NONE;                                              /* LB12 */
    if (B == GL && A != SP && A != BA && A != HY) return NONE;             /* LB12a */
    if (B == CL || B == CP || B == EX || B == IS || B == SY) return NONE;  /* LB13 */
    if (before != NO_UNIT && t->eff[before] == OP) return NONE;            /* LB14: OP SP* × */
    /* LB15a: an initial quotation mark after a start, opening or space,
     * then spaces, and no break */
    if (before != NO_UNIT && t->eff[before] == QU && vega_category(t->codes[before]) == VEGA_PI) {
        if (!before) return NONE;
        unsigned q = t->eff[before - 1];
        if (q == BK || q == CR || q == LF || q == NL || q == OP || q == QU || q == GL || q == SP || q == ZW) return NONE;
    }
    /* LB15b: no break before a final quotation mark that ends something */
    if (B == QU && vega_category(t->codes[i]) == VEGA_PF) {
        size_t j = next_unit(t, i);
        if (j == t->count) return NONE;
        unsigned n = t->eff[j];
        if (n == SP || n == GL || n == WJ || n == CL || n == QU || n == CP || n == EX || n == IS || n == SY ||
            n == BK || n == CR || n == LF || n == NL || n == ZW)
            return NONE;
    }
    if (B == NS && before != NO_UNIT && one_of(t->eff[before], CL, CP)) return NONE; /* LB16 */
    if (B == B2 && before != NO_UNIT && t->eff[before] == B2) return NONE;            /* LB17 */
    if (A == SP) return ALLOWED;                                           /* LB18 */
    if (B == QU || A == QU) return NONE;                                   /* LB19 */
    if (B == CB || A == CB) return ALLOWED;                                /* LB20 */
    if (B == BA || B == HY || B == NS || A == BB) return NONE;             /* LB21 */
    if (one_of(A, HY, BA) && unit && t->eff[unit - 1] == HL) return NONE;  /* LB21a */
    if (A == SY && B == HL) return NONE;                                   /* LB21b */
    if (B == IN) return NONE;                                              /* LB22 */
    if (one_of(A, AL, HL) && B == NU) return NONE;                         /* LB23 */
    if (A == NU && one_of(B, AL, HL)) return NONE;
    if (A == PR && (B == ID || B == EB || B == EM)) return NONE;           /* LB23a */
    if ((A == ID || A == EB || A == EM) && B == PO) return NONE;
    if (one_of(A, PR, PO) && one_of(B, AL, HL)) return NONE;               /* LB24 */
    if (one_of(A, AL, HL) && one_of(B, PR, PO)) return NONE;
    /* LB25, as Example 7 of section 8.2 tailors it: numbers */
    if (one_of(A, PR, PO)) {
        if (B == NU) return NONE;
        if (one_of(B, OP, HY)) {
            size_t j = next_unit(t, i);
            if (j < t->count && t->eff[j] == NU) return NONE;
        }
    }
    if (one_of(A, OP, HY) && B == NU) return NONE;
    if (t->number[i - 1] && (B == NU || B == SY || B == IS || B == CL || B == CP)) return NONE;
    if (one_of(B, PO, PR) && (t->number[i - 1] || (one_of(A, CL, CP) && unit && t->number[unit - 1]))) return NONE;
    if (A == JL && (B == JL || B == JV || B == H2 || B == H3)) return NONE; /* LB26 */
    if (one_of(A, JV, H2) && one_of(B, JV, JT)) return NONE;
    if (one_of(A, JT, H3) && B == JT) return NONE;
    if ((A == JL || A == JV || A == JT || A == H2 || A == H3) && B == PO) return NONE; /* LB27 */
    if (A == PR && (B == JL || B == JV || B == JT || B == H2 || B == H3)) return NONE;
    if (one_of(A, AL, HL) && one_of(B, AL, HL)) return NONE;               /* LB28 */
    /* LB28a: aksaras of Brahmic scripts hold together */
    bool a_start = aksara_start(t, unit), b_start = aksara_start(t, i);
    if (A == AP && b_start) return NONE;
    if (a_start && one_of(B, VF, VI)) return NONE;
    if (A == VI && unit && aksara_start(t, t->start[unit - 1]) && (B == AK || t->codes[i] == 0x25CC)) return NONE;
    if (a_start && b_start) {
        size_t j = next_unit(t, i);
        if (j < t->count && t->eff[j] == VF) return NONE;
    }
    if (A == IS && one_of(B, AL, HL)) return NONE;                         /* LB29 */
    if ((A == AL || A == HL || A == NU) && B == OP && !vega_east_asian_bracket(t->codes[i])) return NONE; /* LB30 */
    if (A == CP && !vega_east_asian_bracket(base) && (B == AL || B == HL || B == NU)) return NONE;
    if (A == RI && B == RI && t->ri[i - 1] % 2) return NONE;               /* LB30a */
    if (B == EM && (A == EB || (vega_extended_pictographic(base) && vega_category(base) == VEGA_CN))) return NONE; /* LB30b */
    return ALLOWED;                                                        /* LB31 */
}

bool vega_line_breaks(const uint32_t *codes, size_t count, uint8_t *breaks) {
    breaks[0] = NONE; /* LB2 */
    if (!count) return true;
    uint8_t *bytes = malloc(count * 4);
    size_t *sizes = malloc(count * 3 * sizeof(size_t));
    if (!bytes || !sizes) {
        free(bytes);
        free(sizes);
        return false;
    }
    Text t = {codes, count, bytes, bytes + count, bytes + 2 * count, bytes + 3 * count, sizes, sizes + count, sizes + 2 * count};
    for (size_t i = 0; i < count; ++i) {
        unsigned c = t.cls[i] = resolve(codes[i]);
        bool mark = c == CM || c == ZWJ;
        unsigned previous = i ? t.eff[i - 1] : SP;
        /* LB9: a mark joins any unit but these; LB10: one that does not is alphabetic. */
        t.joined[i] = mark && i && previous != BK && previous != CR && previous != LF && previous != NL &&
                      previous != SP && previous != ZW;
        t.eff[i] = t.joined[i] ? (uint8_t)previous : mark ? AL : (uint8_t)c;
        t.start[i] = t.joined[i] ? t.start[i - 1] : i;
        t.nonspace[i] = t.eff[i] != SP ? t.start[i] : i ? t.nonspace[i - 1] : NO_UNIT;
        unsigned e = t.eff[i];
        t.number[i] = t.joined[i] ? t.number[i - 1] : e == NU || ((e == SY || e == IS) && i && t.number[i - 1]);
        t.ri[i] = t.joined[i] ? t.ri[i - 1] : e == RI ? (i ? t.ri[i - 1] : 0) + 1 : 0;
    }
    for (size_t i = 1; i < count; ++i) breaks[i] = decide(&t, i);
    breaks[count] = MANDATORY; /* LB3 */
    free(bytes);
    free(sizes);
    return true;
}
