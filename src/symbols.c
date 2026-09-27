#include "symbols.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const unsigned char magic[8] = {'V', 'E', 'G', 'A', 'S', 'Y', 'M', '1'};

static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t i16(const unsigned char *p) { return (int16_t)u16(p); }
static uint32_t u32(const unsigned char *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

VegaSymbol vega_symbol_at(const VegaSymbols *s, uint32_t index) {
    const unsigned char *p = s->bytes + VEGA_SYMBOLS_HEADER + (size_t)index * VEGA_SYMBOL_BYTES;
    return (VegaSymbol){u32(p), u32(p + 4), u32(p + 8), i16(p + 12), i16(p + 14), i16(p + 16), i16(p + 18), i16(p + 20)};
}

VegaCurve vega_curve(const VegaSymbols *s, uint32_t index) {
    const unsigned char *p = s->bytes + VEGA_SYMBOLS_HEADER + (size_t)s->symbols * VEGA_SYMBOL_BYTES +
                             (size_t)index * VEGA_CURVE_BYTES;
    return (VegaCurve){i16(p), i16(p + 2), i16(p + 4), i16(p + 6), i16(p + 8), i16(p + 10)};
}

#define REFUSE(...) do { snprintf(error, error_size, __VA_ARGS__); return false; } while (0)

bool vega_symbols_open(VegaSymbols *s, const unsigned char *bytes, size_t size, char *error, size_t error_size) {
    if (size < VEGA_SYMBOLS_HEADER || memcmp(bytes, magic, sizeof(magic))) REFUSE("not a symbol file");
    *s = (VegaSymbols){bytes, size, u16(bytes + 8), u32(bytes + 12), u32(bytes + 16)};
    uint64_t expected = VEGA_SYMBOLS_HEADER + (uint64_t)s->symbols * VEGA_SYMBOL_BYTES + (uint64_t)s->curves * VEGA_CURVE_BYTES;
    if (u16(bytes + 10) || u32(bytes + 24) || !s->units) REFUSE("header");
    if (u32(bytes + 20) != size || expected != size) REFUSE("size: %zu bytes for %u symbols and %u curves", size, s->symbols, s->curves);
    for (uint32_t i = 0; i < s->symbols; ++i) {
        VegaSymbol symbol = vega_symbol_at(s, i);
        const unsigned char *p = bytes + VEGA_SYMBOLS_HEADER + (size_t)i * VEGA_SYMBOL_BYTES;
        if (u16(p + 22)) REFUSE("symbol %u: flags", i);
        if (i && symbol.code <= vega_symbol_at(s, i - 1).code) REFUSE("symbol %u: codes out of order", i);
        if (symbol.first > s->curves || symbol.count > s->curves - symbol.first) REFUSE("symbol %u: curves", i);
        if (symbol.left > symbol.right || symbol.bottom > symbol.top) REFUSE("symbol %u: bounds", i);
    }
    return true;
}

bool vega_symbol(const VegaSymbols *s, uint32_t code, VegaSymbol *out) {
    uint32_t low = 0, high = s->symbols;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        VegaSymbol symbol = vega_symbol_at(s, middle);
        if (symbol.code == code) { *out = symbol; return true; }
        if (symbol.code < code) low = middle + 1; else high = middle;
    }
    return false;
}

/* Where a t^2 + b t + c = 0 inside (0, 1), appended to ts. */
static int roots(double a, double b, double c, double *ts) {
    int n = 0;
    if (fabs(a) < 1e-12) {
        if (fabs(b) > 1e-12) ts[n++] = -c / b;
    } else {
        double d = b * b - 4 * a * c;
        if (d >= 0) {
            /* The root that does not cancel, and the other by the product. */
            double q = -0.5 * (b + copysign(sqrt(d), b));
            ts[n++] = q / a;
            if (q != 0) ts[n++] = c / q;
        }
    }
    int kept = 0;
    for (int i = 0; i < n; ++i) if (ts[i] > 0 && ts[i] < 1) ts[kept++] = ts[i];
    return kept;
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* The curve's share of the pixel [0, 1]^2 (points relative to its lower left
 * corner, in pixels): the integral of clamp(1 - x, 0, 1) dy along the part
 * with y in [0, 1]. Over a closed outline these add up to the covered area,
 * negative for a counterclockwise one (Green's theorem), so the covered area
 * is exact, as FreeType's is. The curve splits where it crosses the pixel's
 * sides; each piece lies left of it (dy counts whole), right of it (nothing),
 * or across it (a cubic in t, integrated in closed form). */
static double curve_area(double x0, double y0, double x1, double y1, double x2, double y2) {
    if (fmax(fmax(y0, y1), y2) <= 0 || fmin(fmin(y0, y1), y2) >= 1 || fmin(fmin(x0, x1), x2) >= 1) return 0;
    if (fmax(fmax(x0, x1), x2) <= 0) return clamp01(y2) - clamp01(y0);
    /* x(t) = x0 - 2 bx t + ax t^2, and y alike. */
    double ax = x0 - 2 * x1 + x2, bx = x0 - x1, ay = y0 - 2 * y1 + y2, by = y0 - y1;
    double ts[10] = {0};
    int n = 1;
    n += roots(ay, -2 * by, y0, ts + n);
    n += roots(ay, -2 * by, y0 - 1, ts + n);
    n += roots(ax, -2 * bx, x0, ts + n);
    n += roots(ax, -2 * bx, x0 - 1, ts + n);
    ts[n++] = 1;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && ts[j] < ts[j - 1]; --j) { double t = ts[j]; ts[j] = ts[j - 1]; ts[j - 1] = t; }
    /* (1 - x(t)) y'(t) = (u0 + u1 t + u2 t^2)(v0 + v1 t) */
    double u0 = 1 - x0, u1 = 2 * bx, u2 = -ax, v0 = -2 * by, v1 = 2 * ay;
    double k1 = u0 * v0, k2 = (u0 * v1 + u1 * v0) / 2, k3 = (u1 * v1 + u2 * v0) / 3, k4 = u2 * v1 / 4, area = 0;
    for (int i = 0; i + 1 < n; ++i) {
        double ta = ts[i], tb = ts[i + 1], t = (ta + tb) / 2;
        if (tb <= ta) continue;
        double y = (ay * t - 2 * by) * t + y0, x = (ax * t - 2 * bx) * t + x0;
        if (y <= 0 || y >= 1 || x >= 1) continue;
        if (x <= 0) area += ((ay * tb - 2 * by) * tb) - ((ay * ta - 2 * by) * ta);
        else area += (((k4 * tb + k3) * tb + k2) * tb + k1) * tb - (((k4 * ta + k3) * ta + k2) * ta + k1) * ta;
    }
    return area;
}

double vega_symbol_coverage(const VegaSymbols *s, const VegaSymbol *symbol, double scale, double x, double y) {
    /* The pixel's lower left corner, in units. */
    double left = x - 0.5 / scale, bottom = y - 0.5 / scale, area = 0;
    for (uint32_t k = symbol->first; k < symbol->first + symbol->count; ++k) {
        VegaCurve c = vega_curve(s, k);
        area += curve_area((c.x0 - left) * scale, (c.y0 - bottom) * scale, (c.x1 - left) * scale, (c.y1 - bottom) * scale,
                           (c.x2 - left) * scale, (c.y2 - bottom) * scale);
    }
    area = fabs(area);
    return area > 1 ? 1 : area;
}

void vega_symbol_raster(const VegaSymbols *s, const VegaSymbol *symbol, double scale, double left, double top, int width,
                        int height, unsigned char *out) {
    for (int j = 0; j < height; ++j)
        for (int i = 0; i < width; ++i) {
            double coverage = vega_symbol_coverage(s, symbol, scale, left + (i + 0.5) / scale, top - (j + 0.5) / scale);
            out[(size_t)j * (size_t)width + (size_t)i] = (unsigned char)lround(coverage * 255);
        }
}
