/* Word boundaries (UAX #29, Unicode 15.1): text/text.h. As with lines,
 * each boundary goes to the first rule that decides it; extends, formats
 * and joiners fold into the character before them (WB4), so the rules after
 * it see through them. */
#include "text.h"
#include <stdlib.h>

enum { OTHER = VEGA_WB_OTHER, CR = VEGA_WB_CR, LF = VEGA_WB_LF, NEWLINE = VEGA_WB_NEWLINE, EXTEND = VEGA_WB_EXTEND,
       ZWJ = VEGA_WB_ZWJ, RI = VEGA_WB_RI, FORMAT = VEGA_WB_FORMAT, KATAKANA = VEGA_WB_KATAKANA,
       HEBREW = VEGA_WB_HEBREW_LETTER, ALETTER = VEGA_WB_ALETTER, SINGLE_QUOTE = VEGA_WB_SINGLE_QUOTE,
       DOUBLE_QUOTE = VEGA_WB_DOUBLE_QUOTE, MIDNUMLET = VEGA_WB_MIDNUMLET, MIDLETTER = VEGA_WB_MIDLETTER,
       MIDNUM = VEGA_WB_MIDNUM, NUMERIC = VEGA_WB_NUMERIC, EXTENDNUMLET = VEGA_WB_EXTENDNUMLET,
       WSEGSPACE = VEGA_WB_WSEGSPACE };

static bool letter(unsigned c) { return c == ALETTER || c == HEBREW; }                       /* AHLetter */
static bool mid_letter(unsigned c) { return c == MIDLETTER || c == MIDNUMLET || c == SINGLE_QUOTE; }
static bool mid_number(unsigned c) { return c == MIDNUM || c == MIDNUMLET || c == SINGLE_QUOTE; }
static bool ignored(unsigned c) { return c == EXTEND || c == FORMAT || c == ZWJ; }
static bool line_end(unsigned c) { return c == CR || c == LF || c == NEWLINE; }

bool vega_words(const uint32_t *codes, size_t count, uint8_t *breaks) {
    breaks[0] = count > 0; /* WB1 */
    if (!count) return true;
    /* Per character: its class; its unit's (WB4); where its unit starts;
     * regional indicators ending at its unit. */
    uint8_t *raw = malloc(2 * count);
    size_t *start = malloc(2 * count * sizeof(size_t));
    if (!raw || !start) {
        free(raw);
        free(start);
        return false;
    }
    uint8_t *eff = raw + count;
    size_t *ri = start + count;
    for (size_t i = 0; i < count; ++i) {
        raw[i] = vega_word(codes[i]);
        bool folds = ignored(raw[i]) && i && !line_end(raw[start[i - 1]]);
        eff[i] = folds ? eff[i - 1] : raw[i];
        start[i] = folds ? start[i - 1] : i;
        ri[i] = folds ? ri[i - 1] : eff[i] == RI ? (i ? ri[i - 1] : 0) + 1 : 0;
    }
    for (size_t i = 1; i < count; ++i) {
        unsigned a = raw[i - 1], b = raw[i], A = eff[i - 1];
        size_t unit = start[i - 1], next = i + 1;
        while (next < count && start[next] != next) next++;
        unsigned after = next < count ? eff[next] : OTHER, earlier = unit ? eff[unit - 1] : OTHER;
        bool join;
        if (a == CR && b == LF) join = true;                                          /* WB3 */
        else if (line_end(a) || line_end(b)) join = false;                            /* WB3a, WB3b */
        else if (a == ZWJ && vega_extended_pictographic(codes[i])) join = true;      /* WB3c */
        else if (a == WSEGSPACE && b == WSEGSPACE) join = true;                       /* WB3d */
        else if (ignored(b)) join = true;                                             /* WB4 */
        else if (letter(A) && letter(b)) join = true;                                 /* WB5 */
        else if (letter(A) && (mid_letter(b)) && letter(after)) join = true;          /* WB6 */
        else if (mid_letter(A) && letter(b) && unit && letter(earlier)) join = true;  /* WB7 */
        else if (A == HEBREW && b == SINGLE_QUOTE) join = true;                       /* WB7a */
        else if (A == HEBREW && b == DOUBLE_QUOTE && after == HEBREW) join = true;    /* WB7b */
        else if (A == DOUBLE_QUOTE && b == HEBREW && unit && earlier == HEBREW) join = true; /* WB7c */
        else if (A == NUMERIC && b == NUMERIC) join = true;                           /* WB8 */
        else if (letter(A) && b == NUMERIC) join = true;                              /* WB9 */
        else if (A == NUMERIC && letter(b)) join = true;                              /* WB10 */
        else if (mid_number(A) && b == NUMERIC && unit && earlier == NUMERIC) join = true; /* WB11 */
        else if (A == NUMERIC && mid_number(b) && after == NUMERIC) join = true;      /* WB12 */
        else if (A == KATAKANA && b == KATAKANA) join = true;                         /* WB13 */
        else if ((letter(A) || A == NUMERIC || A == KATAKANA || A == EXTENDNUMLET) && b == EXTENDNUMLET) join = true; /* WB13a */
        else if (A == EXTENDNUMLET && (letter(b) || b == NUMERIC || b == KATAKANA)) join = true; /* WB13b */
        else if (A == RI && b == RI && ri[i - 1] % 2) join = true;                    /* WB15, WB16 */
        else join = false;                                                            /* WB999 */
        breaks[i] = !join;
    }
    breaks[count] = 1; /* WB2 */
    free(raw);
    free(start);
    return true;
}
