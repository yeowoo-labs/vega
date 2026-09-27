#ifndef VEGA_SYMBOLS_H
#define VEGA_SYMBOLS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Symbols: glyphs and icons as outlines of quadratic curves in one file,
 * verified once and then read where it lies (docs/design/002-symbol-format.md).
 * An outline is a chain of points, on or off the curve, as in TrueType: two
 * off points in a row imply the on point halfway between them. */
enum { VEGA_SYMBOLS_HEADER = 28, VEGA_SYMBOL_BYTES = 24, VEGA_POINT_BYTES = 4 };
typedef struct {
    int16_t x, y;
    bool off;  /* a control point */
    bool move; /* starts a contour */
} VegaPoint;
typedef struct { float x0, y0, x1, y1, x2, y2; } VegaCurve; /* a line has its middle as control */
typedef struct {
    uint32_t code, first, count;          /* its points: [first, first + count) */
    int16_t left, bottom, right, top;     /* bounds, in units, y up */
    int16_t advance;
} VegaSymbol;
typedef struct {
    const unsigned char *bytes;
    size_t size;
    uint16_t units;          /* per em */
    int16_t ascent, descent; /* a font's line, in units from the baseline, y up (0 and 0 for icons) */
    uint32_t symbols, points;
} VegaSymbols;

/* Verify bytes as a symbol file (kept, not copied); false with why. What
 * reading needs is checked, per symbol: after this no read leaves the file
 * or its symbol. The points themselves are not: one outside its symbol's
 * bounds draws wrong. */
bool vega_symbols_open(VegaSymbols *, const unsigned char *bytes, size_t size, char *error, size_t error_size);
/* The symbol of code (a binary search), or false. */
bool vega_symbol(const VegaSymbols *, uint32_t code, VegaSymbol *out);
VegaSymbol vega_symbol_at(const VegaSymbols *, uint32_t index);
VegaPoint vega_symbols_point(const VegaSymbols *, uint32_t index);
/* The curve that point index of symbol begins, if any: an off point is the
 * control of one, and an on point followed by another on point in the same
 * contour starts a line. Every curve of a symbol is had once this way, from
 * the point and its two neighbours alone (as a GPU would fetch it). */
bool vega_symbol_curve(const VegaSymbols *, const VegaSymbol *, uint32_t index, VegaCurve *out);
/* All of a symbol's curves, in one pass over its points; out has room for
 * symbol->count (a symbol has no more curves than points). */
uint32_t vega_symbol_curves(const VegaSymbols *, const VegaSymbol *, VegaCurve *out);
/* The coverage (0 to 1) of the pixel whose center is (x, y) in units, at
 * scale pixels per unit: the exact area the curves' outline covers. */
double vega_coverage(const VegaCurve *, uint32_t count, double scale, double x, double y);
/* The pixels a symbol reaches at scale (pixels per unit), one more on each
 * side: width and height, and the offset of the box's top left from the
 * symbol's origin (right, up). x and y are where a set of bitmaps (an atlas)
 * keeps it. A symbol with nothing to draw has an empty box. */
typedef struct { int32_t x, y, width, height, left, top; } VegaPlace;
VegaPlace vega_symbol_place(const VegaSymbol *, float scale);
/* width x height 8-bit coverage, rows top first, stride bytes apart; pixel
 * (i, j) has its center at (left + (i + 0.5) / scale, top - (j + 0.5) / scale)
 * in units. */
void vega_symbol_raster(const VegaSymbols *, const VegaSymbol *, double scale, double left, double top, int width,
                        int height, unsigned char *out, size_t stride);

/* Symbols drawn once at one scale and kept, for surfaces that compose them
 * on the CPU: each the first time it is asked for. */
typedef struct { VegaPlace place; const unsigned char *pixels; } VegaBitmap; /* pixels: place.width per row */
typedef struct VegaGlyphs VegaGlyphs;
VegaGlyphs *vega_glyphs_open(const VegaSymbols *, float scale);
/* The bitmap of code, or NULL when the file has no such symbol. */
const VegaBitmap *vega_glyph(VegaGlyphs *, uint32_t code);
void vega_glyphs_close(VegaGlyphs *);
#endif
