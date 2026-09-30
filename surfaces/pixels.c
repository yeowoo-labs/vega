#include "vega.h"
#include "text.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The pixel surface: the TUI's grid drawn in a typeface, glyphs drawn once
 * per size (the exact area of their curves) in their tone's colour, straight
 * into pixels: no page, no scene. Two ways:
 *  - cells: each cell's character in its cell, in the first font that has it;
 *  - type, when the first font shapes: the layout's spans set as lines of
 *    text (text/layout.h). A span starts at its column, or where the text it
 *    follows stopped; it may go on to its end, but not past the next span's
 *    column that starts afresh; one that would is cut with an ellipsis. So
 *    the grid lays out (rows, columns, what is focused) and type shapes. */

/* Colours, 0xRRGGBB: the background, a focused row's, and one per tone. */
static const uint32_t background = 0x1e1e2e, focused = 0x313244, strong = 0xffffff;
static const uint32_t tones[VEGA_TONES] = {0xcdd6f4, 0x7f849c, 0x89b4fa, 0xa6e3a1, 0xf9e2af, 0xf38ba8};

/* Whether a font that shapes has a character: its character map does, and
 * its file holds that glyph (a file holds the glyphs its codes reach). */
static bool has(void *context, unsigned font, uint32_t code) {
    const VegaFace *face = context;
    unsigned k = face->font_face[font];
    uint32_t glyph = vega_shaping_glyph(&face->shaping[k], code);
    VegaSymbol symbol;
    return glyph && vega_symbol(face->symbols[k], VEGA_GLYPH_SYMBOLS + glyph, &symbol);
}

bool vega_face_open(VegaFace *face, const VegaSymbols *const *symbols, unsigned count, float pixels, char *error,
                    size_t error_size) {
    *face = (VegaFace){0};
    face->fonts = (VegaFonts){{0}, 0, has, face};
    face->pixels = pixels;
    if (!count || count > VEGA_FACES || pixels < 4 || pixels > 256) {
        snprintf(error, error_size, "1 to %d fonts, at 4 to 256 pixels", VEGA_FACES);
        return false;
    }
    /* A cell is as wide as a digit (the grid is monospace: wide characters
     * take two) and as tall as the first font's line. */
    const VegaSymbols *first = symbols[0];
    VegaSymbol zero;
    if (!first->units || first->ascent <= first->descent || !vega_symbol(first, '0', &zero) || zero.advance <= 0) {
        snprintf(error, error_size, "no line and digit to lay a grid out with");
        return false;
    }
    float scale = pixels / first->units;
    face->cell_width = (int)lroundf(zero.advance * scale);
    face->baseline = (int)ceilf(first->ascent * scale);
    face->cell_height = face->baseline + (int)ceilf(-first->descent * scale);
    if (face->cell_width <= 0) {
        snprintf(error, error_size, "a digit narrower than a pixel");
        return false;
    }
    for (unsigned k = 0; k < count; ++k) {
        face->symbols[k] = symbols[k];
        face->glyphs[k] = symbols[k]->units ? vega_glyphs_open(symbols[k], pixels / symbols[k]->units) : NULL;
        face->count = k + 1;
        if (!face->glyphs[k]) {
            snprintf(error, error_size, "font %u: no glyphs to draw", k + 1);
            goto fail;
        }
        /* Type is set when the first font shapes, in the fonts that do. */
        if (!symbols[k]->shaping || (k && !face->fonts.count)) continue;
        char why[128];
        if (!vega_shaping_open(&face->shaping[k], symbols[k]->shaping, symbols[k]->shaping_size, why, sizeof(why))) {
            snprintf(error, error_size, "font %u: %s", k + 1, why);
            goto fail;
        }
        face->font_face[face->fonts.count] = k;
        face->fonts.fonts[face->fonts.count++] = &face->shaping[k];
    }
    if (face->fonts.count && !(face->shaper = vega_shaper_new())) {
        snprintf(error, error_size, "out of memory");
        goto fail;
    }
    return true;
fail:
    vega_face_close(face);
    return false;
}

void vega_face_close(VegaFace *face) {
    for (unsigned k = 0; k < face->count; ++k) vega_glyphs_close(face->glyphs[k]);
    for (unsigned f = 0; f < face->fonts.count; ++f) vega_shaping_close(&face->shaping[face->font_face[f]]);
    vega_shaper_free(face->shaper);
    vega_layout_free(&face->layout);
    free(face->work);
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

/* A character no font has: a box where it goes. */
static void missing(uint32_t *out, size_t stride, int x, int y, int width, int height, int limit, uint32_t color) {
    int left = x + 1, right = (x + width < limit ? x + width : limit) - 2, top = y + height / 5, bottom = y + height - height / 5;
    if (left < 0 || right < left) return;
    for (int i = left; i <= right; ++i) out[(size_t)top * stride + (size_t)i] = out[(size_t)bottom * stride + (size_t)i] = color;
    for (int j = top; j <= bottom; ++j) out[(size_t)j * stride + (size_t)left] = out[(size_t)j * stride + (size_t)right] = color;
}

void vega_pixels_clear(uint32_t *out, size_t stride, unsigned width, unsigned height) {
    fill(out, stride, 0, 0, (int)width, (int)height, background);
}

static uint32_t color_of(uint8_t tone, bool is_strong) { return is_strong ? strong : tones[tone < VEGA_TONES ? tone : 0]; }

/* Cells [first, end) of a row, each character in the first font that has it. */
static void draw_cells(const VegaGrid *grid, VegaFace *face, unsigned r, unsigned first, unsigned end, uint32_t *out,
                       size_t stride, int width, int height) {
    for (unsigned c = first; c < end && c < grid->width; ++c) {
        const VegaCell *cell = &grid->cells[(size_t)r * grid->width + c];
        if (!cell->ch || cell->ch == ' ') continue;
        uint32_t color = color_of(cell->tone, cell->strong);
        int x = (int)c * face->cell_width, y = (int)r * face->cell_height;
        const VegaBitmap *glyph = NULL;
        unsigned k = 0;
        while (k < face->count && !(glyph = vega_glyph(face->glyphs[k], cell->ch))) ++k;
        unsigned cells = vega_width(cell->ch) == 2 ? 2 : 1;
        /* A wide character's glyph is centred in its two cells: a CJK font's full width is
         * less than two cells of a monospace Latin font. */
        int centred = glyph && cells == 2 && glyph->advance < 2 * face->cell_width
                          ? (2 * face->cell_width - glyph->advance) / 2 : 0;
        if (!glyph) missing(out, stride, x, y, face->cell_width * (int)cells, face->cell_height, width, color);
        else draw(out, stride, width, height, glyph, x + centred, y + face->baseline, color);
        if (!cell->mark) continue;
        for (k = 0; k < face->count; ++k)
            if ((glyph = vega_glyph(face->glyphs[k], cell->mark))) {
                draw(out, stride, width, height, glyph, x, y + face->baseline, color);
                break;
            }
    }
}

/* ---- setting type ----------------------------------------------------- */

/* What setting works in: a line's characters, room for trying it cut, its
 * grapheme boundaries, where each of its spans starts in it, and the spans
 * in the order they are set. */
typedef struct {
    uint32_t *codes, *cut;
    uint8_t *clusters;
    size_t *order, *starts;
} Work;

static bool work_for(VegaFace *face, const VegaGrid *grid, Work *w) {
    /* A line's characters are at most the text's bytes. */
    size_t most = grid->text_used;
    size_t need = 2 * grid->span_count * sizeof(size_t) + (2 * most + 2) * sizeof(uint32_t) + most + 1;
    if (need > face->work_size) {
        void *grown = realloc(face->work, need);
        if (!grown) return false;
        face->work = grown;
        face->work_size = need;
    }
    w->order = face->work;
    w->starts = w->order + grid->span_count;
    w->codes = (uint32_t *)(w->starts + grid->span_count);
    w->cut = w->codes + most + 1;
    w->clusters = (uint8_t *)(w->cut + most + 1);
    return true;
}

/* A line set: where its pen starts (in the layout's x), how far all its
 * glyphs go and how much of that shows (spaces at its end left out), its
 * direction, and how many of its characters it keeps (all, or those before
 * an ellipsis). */
typedef struct { int32_t start, advance, shows; bool rtl; size_t kept; } Set;
typedef enum { SET, NO_ROOM, UNSET } Setting;

static bool set_line(VegaFace *face, const uint32_t *codes, size_t count, Set *out) {
    VegaLayout *l = &face->layout;
    if (!vega_layout(l, face->shaper, &face->fonts, codes, count, VEGA_BIDI_AUTO, 0) || l->line_count != 1) return false;
    const VegaLine *line = &l->lines[0];
    int32_t advance = 0;
    for (size_t g = line->first; g < line->first + line->count; ++g) advance += l->glyphs[g].advance;
    /* Right to left, the spaces at its end hang past its left edge. */
    out->rtl = l->level & 1;
    out->start = out->rtl ? line->left - (advance - line->width) : line->left;
    out->advance = advance;
    out->shows = line->width;
    out->kept = count;
    return true;
}

/* codes[0, count) cut after n characters, spaces before the cut left out,
 * with an ellipsis: in w->cut, its length returned. */
static size_t cut_at(Work *w, size_t n) {
    while (n && vega_bidi_class(w->codes[n - 1]) == VEGA_BIDI_WS) --n;
    memcpy(w->cut, w->codes, n * sizeof(*w->cut));
    w->cut[n] = 0x2026;
    return n + 1;
}

/* Set codes[0, count) to show within room (the first font's units): whole,
 * or the longest start of it, of whole grapheme clusters, that fits with an
 * ellipsis after it. */
static Setting set_within(VegaFace *face, Work *w, size_t count, int32_t room, Set *out) {
    if (!set_line(face, w->codes, count, out)) return UNSET;
    if (out->shows <= room) return SET;
    vega_graphemes(w->codes, count, w->clusters);
    size_t low = 0, high = count, best = SIZE_MAX; /* the cut is a cluster boundary in [low, high) */
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        while (middle > low && !w->clusters[middle]) --middle;
        Set tried;
        if (!set_line(face, w->cut, cut_at(w, middle), &tried)) return UNSET;
        if (tried.shows <= room) {
            best = middle;
            for (low = middle + 1; low < high && !w->clusters[low];) ++low;
        } else
            high = middle;
    }
    if (best == SIZE_MAX) return NO_ROOM;
    size_t kept = best;
    if (!set_line(face, w->cut, cut_at(w, best), out)) return UNSET;
    out->kept = kept;
    return SET;
}

/* The layout's glyphs with their pen starting at x (pixels), each in the
 * colour of the span its character came from (an ellipsis: the one it
 * cuts). */
static void draw_set(VegaFace *face, const Set *set, const VegaSpan *const *spans, const size_t *starts, size_t count,
                     double x, int top, uint32_t *out, size_t stride, int width, int height) {
    const VegaLayout *l = &face->layout;
    double scale = (double)face->pixels / face->fonts.fonts[0]->upem;
    for (size_t g = 0; g < l->glyph_count; ++g) {
        const VegaPlaced *p = &l->glyphs[g];
        size_t character = p->cluster < set->kept ? p->cluster : set->kept ? set->kept - 1 : 0, k = 0;
        while (k + 1 < count && starts[k + 1] <= character) ++k;
        uint32_t color = color_of(spans[k]->tone, spans[k]->strong);
        unsigned font = face->font_face[p->font];
        const VegaBitmap *b = vega_glyph(face->glyphs[font], VEGA_GLYPH_SYMBOLS + p->glyph);
        int gx = (int)lround(x + (p->x - set->start) * scale);
        if (b) draw(out, stride, width, height, b, gx, top + face->baseline - (int)lround(p->y * scale), color);
        else if (p->advance > 0)
            missing(out, stride, gx, top, (int)lround(p->advance * scale), face->cell_height, width, color);
    }
}

static bool before(const VegaSpan *spans, size_t a, size_t b) {
    const VegaSpan *x = &spans[a], *y = &spans[b];
    if (x->row != y->row) return x->row < y->row;
    if (x->column != y->column) return x->column < y->column;
    return a < b;
}

/* Spans by row, then column, then as written: an insertion sort, as they
 * come nearly in order. */
static void sort_spans(size_t *order, size_t count, const VegaSpan *spans) {
    for (size_t i = 0; i < count; ++i) order[i] = i;
    for (size_t i = 1; i < count; ++i)
        for (size_t k = i; k && before(spans, order[k], order[k - 1]); --k) {
            size_t t = order[k];
            order[k] = order[k - 1];
            order[k - 1] = t;
        }
}

/* Each line of text: a span, and those that follow it. It starts at the
 * span's column (a right one ends at its end) and has until the nearest of
 * its spans' ends and the next span on the row. */
static bool set_type(const VegaGrid *grid, VegaFace *face, uint32_t *out, size_t stride, int width, int height) {
    Work w;
    if (!work_for(face, grid, &w)) return false;
    size_t n = grid->span_count;
    sort_spans(w.order, n, grid->spans);
    double scale = (double)face->pixels / face->fonts.fonts[0]->upem;
    int caret = -1;
    const VegaSpan *line[VEGA_TEXT];
    for (size_t i = 0, j; i < n; i = j) {
        const VegaSpan *s = &grid->spans[w.order[i]];
        unsigned end = s->end, reach = s->reach;
        size_t spans = 0, count = 0;
        for (j = i; j < n && spans < VEGA_TEXT; ++j) {
            const VegaSpan *t = &grid->spans[w.order[j]];
            if (j > i && (t->row != s->row || !t->follows)) break;
            if (t->end < end) end = t->end;
            reach = t->reach;
            line[spans] = t;
            w.starts[spans++] = count;
            for (size_t at = 0; at < t->length;) {
                uint32_t code;
                at += vega_decode(grid->text + t->at, t->length, at, &code);
                w.codes[count++] = vega_visible(code); /* one line: no breaks in it */
            }
        }
        if (j < n && grid->spans[w.order[j]].row == s->row && grid->spans[w.order[j]].column < end)
            end = grid->spans[w.order[j]].column;
        if (end > grid->width) end = grid->width;
        if (end <= s->column) continue;
        double box = (double)s->column * face->cell_width, limit = (double)end * face->cell_width;
        int top = (int)s->row * face->cell_height;
        Set set;
        Setting setting = set_within(face, &w, count, (int32_t)floor((limit - box) / scale), &set);
        if (setting == UNSET) {
            /* No font can set it (none has a plan for its script): its cells, then. */
            draw_cells(grid, face, s->row, s->column, reach, out, stride, width, height);
            continue;
        }
        if (setting == NO_ROOM) continue;
        /* Right to left, the spaces at its end hang past its left edge. */
        double hang = (set.advance - set.shows) * scale, x = s->right ? limit - set.advance * scale + (set.rtl ? 0 : hang)
                                                                       : box - (set.rtl ? hang : 0);
        draw_set(face, &set, line, w.starts, spans, x, top, out, stride, width, height);
        /* The cursor at the end of the row's last line: right to left, its left. */
        if ((int)s->row == grid->cursor_row) caret = (int)lround(set.rtl ? (x > box ? x : box) : x + set.advance * scale);
    }
    /* The text cursor, after the typed text. */
    if (caret >= 0) {
        int top = grid->cursor_row * face->cell_height, bar = face->cell_height / 16 + 1;
        int x = caret < width - bar ? caret : width - bar;
        fill(out, stride, x, top + face->cell_height / 8, bar, face->cell_height - face->cell_height / 4, tones[VEGA_NORMAL]);
    }
    return true;
}

void vega_pixels(const VegaGrid *grid, VegaFace *face, uint32_t *out, size_t stride) {
    int width = (int)grid->width * face->cell_width, height = (int)grid->height * face->cell_height;
    /* Backgrounds first: a wide glyph spans its neighbour's cell. */
    for (unsigned r = 0; r < grid->height; ++r)
        for (unsigned c = 0; c < grid->width; ++c)
            fill(out, stride, (int)c * face->cell_width, (int)r * face->cell_height, face->cell_width, face->cell_height,
                 grid->cells[(size_t)r * grid->width + c].focus ? focused : background);
    if (face->fonts.count && set_type(grid, face, out, stride, width, height)) return;
    for (unsigned r = 0; r < grid->height; ++r) draw_cells(grid, face, r, 0, grid->width, out, stride, width, height);
}
