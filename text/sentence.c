/* Sentence boundaries (UAX #29, Unicode 15.1): text/text.h. Extends and
 * formats fold into the character before them (SB5); the rules after a
 * sentence terminal look back through closing punctuation and spaces (kept
 * as the pass goes) and, for SB8, ahead to the next letter (found for
 * every position by one pass from the end). */
#include "text.h"
#include <stdlib.h>

enum { OTHER = VEGA_SB_OTHER, CR = VEGA_SB_CR, LF = VEGA_SB_LF, EXTEND = VEGA_SB_EXTEND, SEP = VEGA_SB_SEP,
       FORMAT = VEGA_SB_FORMAT, SP = VEGA_SB_SP, LOWER = VEGA_SB_LOWER, UPPER = VEGA_SB_UPPER, OLETTER = VEGA_SB_OLETTER,
       NUMERIC = VEGA_SB_NUMERIC, ATERM = VEGA_SB_ATERM, SCONTINUE = VEGA_SB_SCONTINUE, STERM = VEGA_SB_STERM,
       CLOSE = VEGA_SB_CLOSE };

static bool paragraph_end(unsigned c) { return c == SEP || c == CR || c == LF; }
static bool terminal(unsigned c) { return c == ATERM || c == STERM; }

bool vega_sentences(const uint32_t *codes, size_t count, uint8_t *breaks) {
    breaks[0] = count > 0; /* SB1 */
    if (!count) return true;
    uint8_t *eff = malloc(3 * count);
    if (!eff) return false;
    uint8_t *folded = eff + count, *stop = eff + 2 * count;
    for (size_t i = 0; i < count; ++i) {
        unsigned c = vega_sentence(codes[i]);
        folded[i] = (c == EXTEND || c == FORMAT) && i && !paragraph_end(eff[i - 1]);
        eff[i] = folded[i] ? eff[i - 1] : (uint8_t)c;
    }
    /* SB8's look ahead: from each position, the class of the first unit
     * that is a letter, a paragraph end or a terminal (OTHER when none is). */
    unsigned next = OTHER;
    for (size_t i = count; i--;) {
        unsigned c = eff[i];
        if (!folded[i] && (c == OLETTER || c == UPPER || c == LOWER || paragraph_end(c) || terminal(c))) next = c;
        stop[i] = (uint8_t)next;
    }
    /* After each unit: the terminal the text ends in, with only closes and
     * then spaces after it (OTHER when it does not end so); whether spaces
     * came yet; the unit before that terminal. */
    unsigned term = OTHER, before_term = OTHER, previous = OTHER;
    bool spaces = false;
    for (size_t i = 0; i < count; ++i) {
        if (folded[i]) {
            breaks[i] = 0; /* SB5 */
            continue;
        }
        unsigned B = eff[i];
        if (i) {
            unsigned A = previous;
            bool join;
            if (A == CR && B == LF) join = true;                                         /* SB3 */
            else if (paragraph_end(A)) join = false;                                     /* SB4 */
            else if (A == ATERM && B == NUMERIC) join = true;                            /* SB6 */
            else if (A == ATERM && (before_term == UPPER || before_term == LOWER) && B == UPPER) join = true; /* SB7 */
            else if (term == ATERM && stop[i] == LOWER) join = true;                     /* SB8 */
            else if (term != OTHER && (B == SCONTINUE || terminal(B))) join = true;      /* SB8a */
            else if (term != OTHER && !spaces && (B == CLOSE || B == SP || paragraph_end(B))) join = true; /* SB9 */
            else if (term != OTHER && (B == SP || paragraph_end(B))) join = true;        /* SB10 */
            else if (term != OTHER) join = false;                                        /* SB11 */
            else join = true;                                                            /* SB998 */
            breaks[i] = !join;
        }
        if (terminal(B)) {
            before_term = previous;
            term = B;
            spaces = false;
        } else if (term != OTHER && B == CLOSE && !spaces) {
            /* still closing the sentence */
        } else if (term != OTHER && B == SP) spaces = true;
        else term = OTHER;
        previous = B;
    }
    breaks[count] = 1; /* SB2 */
    free(eff);
    return true;
}
