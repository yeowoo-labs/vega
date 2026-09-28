#ifndef VEGA_LAYOUT_H
#define VEGA_LAYOUT_H
#include "shape.h"

/* A paragraph set into lines (docs/design/004-text.md): its bidi levels
 * (UAX #9), a font for each grapheme cluster, lines broken where UAX #14
 * allows and they fit (spaces at a line's end hang past it), and each
 * line's runs (one level, one script, one font) shaped and put in visual
 * order. Positions are in the first font's units: x from the paragraph's
 * left edge, y up from the line's baseline. */
typedef struct {
    uint32_t glyph;   /* its font's: its symbol is VEGA_GLYPH_SYMBOLS + glyph */
    uint32_t cluster; /* the paragraph index of the first character it came from */
    int32_t x, y, advance;
    uint32_t font;    /* which of the fonts */
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

/* What text is set in: fonts in order of preference, each cluster in the
 * first that has all its characters (default ignorables aside: shaping
 * hides them), a cluster of Common and Inherited characters (spaces,
 * digits, punctuation) in the font before it when that one has them; no
 * font having them all, the first that has its first character, else the
 * first font. has says whether a font has a character, NULL whether its
 * character map does: a font whose glyphs are not all at hand can say it
 * lacks the others. */
enum { VEGA_LAYOUT_FONTS = 16 };
typedef struct {
    const VegaShaping *fonts[VEGA_LAYOUT_FONTS];
    unsigned count;
    bool (*has)(void *context, unsigned font, uint32_t code);
    void *context;
} VegaFonts;

/* Set count characters in lines of at most width (0: no limit) in the
 * fonts; direction is VEGA_BIDI_AUTO, _LTR or _RTL. The layout holds the
 * result until it is set again; false without memory, or when a run's font
 * has no plan for its script (nor for Common). */
bool vega_layout(VegaLayout *, VegaShaper *, const VegaFonts *, const uint32_t *codes, size_t count, int direction,
                 int32_t width);
void vega_layout_free(VegaLayout *);
#endif
