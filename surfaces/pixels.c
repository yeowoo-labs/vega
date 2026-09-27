#include "vega.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The pixel surface: the TUI's grid of cells, each drawn with its glyph
 * (drawn once per size, the exact area of its curves) in its tone's colour.
 * The view's cells go straight into pixels: no page, no scene. */

/* Colours, 0xRRGGBB: the background, a focused row's, and one per tone. */
static const uint32_t background = 0x1e1e2e, focused = 0x313244, strong = 0xffffff;
static const uint32_t tones[VEGA_TONES] = {0xcdd6f4, 0x7f849c, 0x89b4fa, 0xa6e3a1, 0xf9e2af, 0xf38ba8};

bool vega_face_open(VegaFace *face, const VegaSymbols *symbols, float pixels) {
    *face = (VegaFace){0};
    if (!symbols->units || symbols->ascent <= symbols->descent || pixels < 4 || pixels > 256) return false;
    float scale = pixels / symbols->units;
    /* A cell is as wide as a digit (the grid is monospace: wide characters
     * take two) and as tall as the font's line. */
    VegaSymbol zero;
    if (!vega_symbol(symbols, '0', &zero) || zero.advance <= 0) return false;
    face->cell_width = (int)lroundf(zero.advance * scale);
    face->baseline = (int)ceilf(symbols->ascent * scale);
    face->cell_height = face->baseline + (int)ceilf(-symbols->descent * scale);
    face->glyphs = vega_glyphs_open(symbols, scale);
    return face->glyphs && face->cell_width > 0;
}

void vega_face_close(VegaFace *face) {
    vega_glyphs_close(face->glyphs);
    *face = (VegaFace){0};
}

static void fill(uint32_t *out, size_t stride, int x, int y, int width, int height, uint32_t color) {
    for (int j = 0; j < height; ++j)
        for (int i = 0; i < width; ++i) out[(size_t)(y + j) * stride + (size_t)(x + i)] = color;
}

/* Coverage a (0-255) of color over the pixel: straight alpha, per channel. */
static uint32_t blend(uint32_t under, uint32_t color, unsigned a) {
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        int u = (int)(under >> shift & 0xff), c = (int)(color >> shift & 0xff);
        out |= (uint32_t)(u + ((c - u) * (int)a + 127) / 255) << shift;
    }
    return out;
}

static void draw(uint32_t *out, size_t stride, int width, int height, const VegaBitmap *glyph, int pen_x, int pen_y,
                 uint32_t color) {
    const VegaPlace *p = &glyph->place;
    for (int j = 0; j < p->height; ++j) {
        int y = pen_y - p->top + j;
        if (y < 0 || y >= height) continue;
        for (int i = 0; i < p->width; ++i) {
            int x = pen_x + p->left + i;
            unsigned a = glyph->pixels[(size_t)j * (size_t)p->width + (size_t)i];
            if (a && x >= 0 && x < width) out[(size_t)y * stride + (size_t)x] = blend(out[(size_t)y * stride + (size_t)x], color, a);
        }
    }
}

/* A character the symbols lack: a box in its cell. */
static void missing(uint32_t *out, size_t stride, int x, int y, int width, int height, int limit, uint32_t color) {
    int left = x + 1, right = (x + width < limit ? x + width : limit) - 2, top = y + height / 5, bottom = y + height - height / 5;
    for (int i = left; i <= right; ++i) out[(size_t)top * stride + (size_t)i] = out[(size_t)bottom * stride + (size_t)i] = color;
    for (int j = top; j <= bottom; ++j) out[(size_t)j * stride + (size_t)left] = out[(size_t)j * stride + (size_t)right] = color;
}

void vega_pixels_clear(uint32_t *out, size_t stride, unsigned width, unsigned height) {
    fill(out, stride, 0, 0, (int)width, (int)height, background);
}

void vega_pixels(const VegaGrid *grid, VegaFace *face, uint32_t *out, size_t stride) {
    int width = (int)grid->width * face->cell_width, height = (int)grid->height * face->cell_height;
    /* Backgrounds first: a wide glyph spans its neighbour's cell. */
    for (unsigned r = 0; r < grid->height; ++r)
        for (unsigned c = 0; c < grid->width; ++c)
            fill(out, stride, (int)c * face->cell_width, (int)r * face->cell_height, face->cell_width, face->cell_height,
                 grid->cells[(size_t)r * grid->width + c].focus ? focused : background);
    for (unsigned r = 0; r < grid->height; ++r)
        for (unsigned c = 0; c < grid->width; ++c) {
            const VegaCell *cell = &grid->cells[(size_t)r * grid->width + c];
            if (!cell->ch || cell->ch == ' ') continue;
            uint32_t color = cell->strong ? strong : tones[cell->tone < VEGA_TONES ? cell->tone : 0];
            int x = (int)c * face->cell_width, y = (int)r * face->cell_height;
            const VegaBitmap *glyph = vega_glyph(face->glyphs, cell->ch);
            unsigned cells = vega_width(cell->ch) == 2 ? 2 : 1;
            /* A wide character's glyph is centred in its two cells: a CJK font's full width is
             * less than two cells of a monospace Latin font. */
            int centred = glyph && cells == 2 && glyph->advance < 2 * face->cell_width
                              ? (2 * face->cell_width - glyph->advance) / 2 : 0;
            if (!glyph) missing(out, stride, x, y, face->cell_width * (int)cells, face->cell_height, width, color);
            else draw(out, stride, width, height, glyph, x + centred, y + face->baseline, color);
            if (cell->mark && (glyph = vega_glyph(face->glyphs, cell->mark)))
                draw(out, stride, width, height, glyph, x, y + face->baseline, color);
        }
}
