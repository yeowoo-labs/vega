#ifndef VEGA_LAYOUT_H
#define VEGA_LAYOUT_H
#include "shape.h"

/* A paragraph set into lines (docs/design/004-text.md): its bidi levels
 * (UAX #9), lines broken where UAX #14 allows and they fit (spaces at a
 * line's end hang past it), and each line's runs (one level, one script)
 * shaped and put in visual order. Positions are in the font's units: x
 * from the paragraph's left edge, y up from the line's baseline. */
typedef struct {
    uint32_t glyph;   /* the font's: its symbol is VEGA_GLYPH_SYMBOLS + glyph */
    uint32_t cluster; /* the paragraph index of the first character it came from */
    int32_t x, y, advance;
} VegaPlaced;
typedef struct {
    size_t first, count; /* its glyphs, left to right */
    size_t start, end;   /* the characters it holds, [start, end) */
    int32_t left, width; /* where it starts and how far what shows goes */
} VegaLine;
typedef struct {
    VegaLine *lines;
    size_t line_count;
    VegaPlaced *glyphs;
    size_t glyph_count;
    int level;           /* the paragraph's: odd is right to left, and its lines align right */
    int32_t width;       /* the widest line's */
    /* what setting the paragraph works in, kept for the next */
    void *work;
    size_t work_size, lines_allocated, glyphs_allocated;
} VegaLayout;

/* Set count characters in lines of at most width (0: no limit) with the
 * font's shaping; direction is VEGA_BIDI_AUTO, _LTR or _RTL. The layout
 * holds the result until it is set again; false without memory. */
bool vega_layout(VegaLayout *, VegaShaper *, const VegaShaping *, const uint32_t *codes, size_t count, int direction,
                 int32_t width);
void vega_layout_free(VegaLayout *);
#endif
