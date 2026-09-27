#ifndef VEGA_SYMBOLS_H
#define VEGA_SYMBOLS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Symbols: glyphs and icons as quadratic curves in one file, verified once
 * and then read where it lies (docs/design/002-symbol-format.md). */
enum { VEGA_SYMBOLS_HEADER = 28, VEGA_SYMBOL_BYTES = 24, VEGA_CURVE_BYTES = 12 };
typedef struct { int16_t x0, y0, x1, y1, x2, y2; } VegaCurve;
typedef struct {
    uint32_t code, first, count;          /* its curves: [first, first + count) */
    int16_t left, bottom, right, top;     /* bounds, in units, y up */
    int16_t advance;
} VegaSymbol;
typedef struct {
    const unsigned char *bytes;
    size_t size;
    uint16_t units; /* per em */
    uint32_t symbols, curves;
} VegaSymbols;

/* Verify bytes as a symbol file (kept, not copied); false with why. What
 * reading needs is checked, per symbol: after this no read leaves the file.
 * The curves themselves are not: one outside its symbol's bounds draws wrong. */
bool vega_symbols_open(VegaSymbols *, const unsigned char *bytes, size_t size, char *error, size_t error_size);
/* The symbol of code (a binary search), or false. */
bool vega_symbol(const VegaSymbols *, uint32_t code, VegaSymbol *out);
VegaSymbol vega_symbol_at(const VegaSymbols *, uint32_t index);
VegaCurve vega_curve(const VegaSymbols *, uint32_t index);
/* The coverage (0 to 1) of the pixel whose center is (x, y) in units, at
 * scale pixels per unit: as a GPU would compute it per pixel. */
double vega_symbol_coverage(const VegaSymbols *, const VegaSymbol *, double scale, double x, double y);
/* width x height 8-bit coverage, rows top first; pixel (i, j) has its center
 * at (left + (i + 0.5) / scale, top - (j + 0.5) / scale) in units. */
void vega_symbol_raster(const VegaSymbols *, const VegaSymbol *, double scale, double left, double top, int width,
                        int height, unsigned char *out);
#endif
