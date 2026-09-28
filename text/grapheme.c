/* Grapheme clusters (UAX #29, Unicode 15.1): text/text.h. One pass, the
 * rules in order; three pieces of state for the rules that look further
 * back than one character (GB9c, GB11, GB12-13). */
#include "text.h"

void vega_graphemes(const uint32_t *codes, size_t count, uint8_t *breaks) {
    breaks[0] = count > 0; /* GB1 */
    /* conjunct: 1 after a consonant and extends, 2 once a linker came too
     * (GB9c); pictograph: 1 after a pictograph and extends, 2 after its ZWJ
     * (GB11); regional: indicators in a row (GB12, GB13). */
    unsigned conjunct = 0, pictograph = 0, regional = 0, before = 0;
    for (size_t i = 0; i < count; ++i) {
        uint8_t property = vega_grapheme(codes[i]);
        unsigned now = property & 15, incb = property >> 4;
        bool pictographic = vega_extended_pictographic(codes[i]);
        if (i) {
            bool join;
            if (before == VEGA_GCB_CR && now == VEGA_GCB_LF) join = true;                       /* GB3 */
            else if (before == VEGA_GCB_CONTROL || before == VEGA_GCB_CR || before == VEGA_GCB_LF) join = false; /* GB4 */
            else if (now == VEGA_GCB_CONTROL || now == VEGA_GCB_CR || now == VEGA_GCB_LF) join = false;          /* GB5 */
            else if (before == VEGA_GCB_L && (now == VEGA_GCB_L || now == VEGA_GCB_V || now == VEGA_GCB_LV || now == VEGA_GCB_LVT))
                join = true;                                                                   /* GB6 */
            else if ((before == VEGA_GCB_LV || before == VEGA_GCB_V) && (now == VEGA_GCB_V || now == VEGA_GCB_T))
                join = true;                                                                   /* GB7 */
            else if ((before == VEGA_GCB_LVT || before == VEGA_GCB_T) && now == VEGA_GCB_T) join = true; /* GB8 */
            else if (now == VEGA_GCB_EXTEND || now == VEGA_GCB_ZWJ) join = true;               /* GB9 */
            else if (now == VEGA_GCB_SPACING_MARK) join = true;                                /* GB9a */
            else if (before == VEGA_GCB_PREPEND) join = true;                                  /* GB9b */
            else if (conjunct == 2 && incb == VEGA_INCB_CONSONANT) join = true;                /* GB9c */
            else if (pictograph == 2 && pictographic) join = true;                             /* GB11 */
            else if (before == VEGA_GCB_RI && now == VEGA_GCB_RI && regional % 2) join = true; /* GB12, GB13 */
            else join = false;                                                                 /* GB999 */
            breaks[i] = !join;
        }
        conjunct = incb == VEGA_INCB_CONSONANT ? 1
                   : conjunct && incb == VEGA_INCB_LINKER ? 2
                   : conjunct && incb == VEGA_INCB_EXTEND ? conjunct : 0;
        pictograph = pictographic ? 1
                     : pictograph == 1 && now == VEGA_GCB_EXTEND ? 1
                     : pictograph == 1 && now == VEGA_GCB_ZWJ ? 2 : 0;
        regional = now == VEGA_GCB_RI ? regional + 1 : 0;
        before = now;
    }
    breaks[count] = count > 0; /* GB2 */
}
