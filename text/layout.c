/* Paragraphs set into lines (text/layout.h). The parts are the ones Unicode
 * and HarfBuzz check (bidi.c, linebreak.c, grapheme.c, shape.c); this puts
 * them together as UAX #9 section 3.4 and UAX #14 describe:
 *   1. levels for the paragraph (P2 to I2), each removed character at the
 *      level of the one before it;
 *   2. a script for each character, Common and Inherited ones taking the
 *      one before them (at the start, the first after); and a font for each
 *      grapheme cluster (UAX #29), so no cluster is split between fonts;
 *   3. break opportunities, and one shaping of the paragraph to measure it;
 *   4. lines: the last opportunity where what shows still fits (spaces at a
 *      line's end hang past it), or the first when none does;
 *   5. each line: L1 at its end, runs of one level and one script shaped
 *      again (so joining and kerning end with the line), the runs in L2's
 *      order, the glyphs placed left to right. */
#include "layout.h"
#include "text.h"
#include <stdlib.h>
#include <string.h>

typedef struct { size_t start, end; uint8_t level; VegaScript script; uint8_t font; } Run;

static bool grow(void **array, size_t *allocated, size_t need, size_t size) {
    if (need <= *allocated) return true;
    size_t n = *allocated ? *allocated : 16;
    while (n < need) n *= 2;
    void *grown = realloc(*array, n * size);
    if (!grown) return false;
    *array = grown;
    *allocated = n;
    return true;
}

static bool isolate_control(VegaBidiClass c) {
    return c == VEGA_BIDI_LRI || c == VEGA_BIDI_RLI || c == VEGA_BIDI_FSI || c == VEGA_BIDI_PDI;
}
static bool removed(VegaBidiClass c) {
    return c == VEGA_BIDI_BN || c == VEGA_BIDI_LRE || c == VEGA_BIDI_RLE || c == VEGA_BIDI_LRO || c == VEGA_BIDI_RLO ||
           c == VEGA_BIDI_PDF;
}

/* v in the units of a font of upem `from` as the first font's (`to`), rounded. */
static int32_t scaled(int64_t v, uint32_t from, uint32_t to) {
    if (from == to || !from) return (int32_t)v;
    int64_t t = v * to;
    return (int32_t)((t >= 0 ? t + from / 2 : t - from / 2) / from);
}

/* Shape [r->start, r->end) and add its glyphs at *pen: in the first font's
 * units, the run's own pen scaled (so rounding does not add up). */
static bool shape_run(VegaLayout *l, VegaShaper *shaper, const VegaFonts *fonts, const uint32_t *codes, const Run *r,
                      int32_t *pen) {
    const VegaShaping *s = fonts->fonts[r->font];
    const VegaShaped *g;
    long n = vega_shape(shaper, s, codes + r->start, r->end - r->start, vega_script_code(r->script),
                        r->level & 1 ? VEGA_SHAPE_RTL : VEGA_SHAPE_LTR, &g);
    if (n < 0 || !grow((void **)&l->glyphs, &l->glyphs_allocated, l->glyph_count + (size_t)n, sizeof(VegaPlaced)))
        return false;
    uint32_t from = s->upem, to = fonts->fonts[0]->upem;
    int64_t at = 0;
    for (long k = 0; k < n; ++k) {
        int32_t x = scaled(at, from, to);
        l->glyphs[l->glyph_count++] = (VegaPlaced){g[k].glyph, (uint32_t)(r->start + g[k].cluster),
                                                   *pen + scaled(at + g[k].x_offset, from, to),
                                                   scaled(g[k].y_offset, from, to),
                                                   scaled(at + g[k].x_advance, from, to) - x, r->font};
        at += g[k].x_advance;
    }
    *pen += scaled(at, from, to);
    return true;
}

/* Runs of one level, one script and one font in [start, end). */
static size_t itemize(const uint8_t *levels, const VegaScript *scripts, const uint8_t *fonts, size_t start, size_t end,
                      Run *out) {
    size_t n = 0;
    for (size_t i = start; i < end; ++i) {
        if (n && out[n - 1].level == levels[i] && out[n - 1].script == scripts[i] && out[n - 1].font == fonts[i])
            out[n - 1].end = i + 1;
        else out[n++] = (Run){i, i + 1, levels[i], scripts[i], fonts[i]};
    }
    return n;
}

static bool has(const VegaFonts *fonts, unsigned font, uint32_t code) {
    if (vega_default_ignorable(code)) return true;
    return fonts->has ? fonts->has(fonts->context, font, code) : vega_shaping_glyph(fonts->fonts[font], code) != 0;
}

/* The font for the cluster codes[0, count), after one in previous. */
static unsigned font_for(const VegaFonts *fonts, const uint32_t *codes, size_t count, unsigned previous) {
    bool common = true;
    for (size_t i = 0; i < count && common; ++i) common = vega_script(codes[i]) <= VEGA_SCRIPT_INHERITED;
    for (unsigned k = common ? 0 : 1; k <= fonts->count; ++k) {
        unsigned f = k ? k - 1 : previous;
        bool all = true;
        for (size_t i = 0; i < count && all; ++i) all = has(fonts, f, codes[i]);
        if (all) return f;
    }
    for (unsigned f = 0; f < fonts->count; ++f)
        if (has(fonts, f, codes[0])) return f;
    return common ? previous : 0;
}

static bool set_line(VegaLayout *l, VegaShaper *shaper, const VegaFonts *fonts, const uint32_t *codes,
                     const VegaBidiClass *classes, const uint8_t *levels, uint8_t *line_levels, const VegaScript *scripts,
                     const uint8_t *font_of, Run *runs, size_t *order, int paragraph, size_t start, size_t end,
                     int32_t *pen_out, int32_t *hang_out);

bool vega_layout(VegaLayout *l, VegaShaper *shaper, const VegaFonts *fonts, const uint32_t *codes, size_t count,
                 int direction, int32_t width) {
    l->line_count = l->glyph_count = 0;
    l->width = 0;
    l->level = direction == VEGA_BIDI_RTL;
    if (!count) return true;
    if (!fonts->count || fonts->count > VEGA_LAYOUT_FONTS) return false;
    /* The work, in one block: classes, levels (the paragraph's, and a
     * line's), scripts, breaks, fonts, grapheme boundaries, each
     * character's advance, runs and their order, and each line's end
     * widths. */
    size_t need = count * (sizeof(VegaBidiClass) + 2 + sizeof(VegaScript) + 1 + 2 + sizeof(int32_t) + sizeof(Run) +
                           sizeof(size_t) + 2 * sizeof(int32_t)) + 64;
    if (!grow(&l->work, &l->work_size, need, 1)) return false;
    VegaBidiClass *classes = l->work;
    int32_t *advance = (int32_t *)(classes + count), *totals = advance + count, *hanging = totals + count;
    Run *runs = (Run *)(hanging + count);
    size_t *order = (size_t *)(runs + count);
    uint8_t *levels = (uint8_t *)(order + count), *line_levels = levels + count, *breaks = line_levels + count;
    VegaScript *scripts = breaks + count + 1;
    uint8_t *font_of = scripts + count, *clusters = font_of + count;

    /* 1. Levels. */
    for (size_t i = 0; i < count; ++i) classes[i] = vega_bidi_class(codes[i]);
    int paragraph = vega_bidi_levels(classes, codes, count, direction, levels);
    if (paragraph < 0) return false;
    l->level = paragraph;
    for (size_t i = 0; i < count; ++i)
        if (levels[i] == VEGA_BIDI_REMOVED) levels[i] = (uint8_t)(i ? levels[i - 1] : paragraph);
    /* 2. Scripts. */
    VegaScript first = VEGA_SCRIPT_COMMON, last = 0;
    for (size_t i = 0; i < count; ++i) {
        VegaScript script = vega_script(codes[i]);
        if (script > VEGA_SCRIPT_INHERITED) {
            first = script;
            break;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        VegaScript script = vega_script(codes[i]);
        if (script > VEGA_SCRIPT_INHERITED) last = script;
        scripts[i] = script > VEGA_SCRIPT_INHERITED ? script : last ? last : first;
    }
    memset(font_of, 0, count);
    if (fonts->count > 1) {
        vega_graphemes(codes, count, clusters);
        for (size_t i = 0, font = 0; i < count;) {
            size_t end = i + 1;
            while (end < count && !clusters[end]) ++end;
            font = font_for(fonts, codes + i, end - i, (unsigned)font);
            memset(font_of + i, (int)font, end - i);
            i = end;
        }
    }
    /* 3. Opportunities, and what each character takes. */
    if (!vega_line_breaks(codes, count, breaks)) return false;
    memset(advance, 0, count * sizeof(*advance));
    size_t run_count = itemize(levels, scripts, font_of, 0, count, runs);
    for (size_t r = 0; r < run_count; ++r) {
        size_t before = l->glyph_count;
        int32_t pen = 0;
        if (!shape_run(l, shaper, fonts, codes, &runs[r], &pen)) return false;
        for (size_t k = before; k < l->glyph_count; ++k) advance[l->glyphs[k].cluster] += l->glyphs[k].advance;
        l->glyph_count = before;
    }
    /* 4. Lines, and 5. what each holds. */
    int32_t widest = 0;
    for (size_t start = 0; start < count;) {
        int32_t sum = 0, trail = 0;
        size_t candidate = 0, end = count;
        for (size_t i = start; i < count; ++i) {
            sum += advance[i];
            trail = classes[i] == VEGA_BIDI_WS ? trail + advance[i] : 0;
            uint8_t b = breaks[i + 1];
            if (b == VEGA_BREAK_NONE) continue;
            bool fits = !width || sum - trail <= width;
            if (b == VEGA_BREAK_MANDATORY) {
                end = fits || !candidate ? i + 1 : candidate;
                break;
            }
            if (fits) candidate = i + 1;
            else {
                end = candidate ? candidate : i + 1;
                break;
            }
        }
        size_t first = l->glyph_count;
        int32_t pen, hang;
        for (;;) {
            if (!set_line(l, shaper, fonts, codes, classes, levels, line_levels, scripts, font_of, runs, order, paragraph,
                          start, end, &pen, &hang))
                return false;
            /* Shaped as a line, it may be wider than measured (a letter
             * at its end takes its final form): then end at the opportunity
             * before, if there is one. */
            if (!width || pen - hang <= width) break;
            size_t earlier = end - 1;
            while (earlier > start && breaks[earlier] == VEGA_BREAK_NONE) --earlier;
            if (earlier == start) break;
            end = earlier;
            l->glyph_count = first;
        }
        if (!grow((void **)&l->lines, &l->lines_allocated, l->line_count + 1, sizeof(VegaLine))) return false;
        totals[l->line_count] = pen;
        hanging[l->line_count] = hang;
        l->lines[l->line_count++] = (VegaLine){first, l->glyph_count - first, start, end, 0, pen - hang};
        if (pen - hang > widest) widest = pen - hang;
        start = end;
    }
    /* Right to left, lines end at the paragraph's right edge. */
    l->width = width ? width : widest;
    if (paragraph & 1)
        for (size_t i = 0; i < l->line_count; ++i) {
            VegaLine *line = &l->lines[i];
            line->left = l->width - totals[i];
            for (size_t k = line->first; k < line->first + line->count; ++k) l->glyphs[k].x += line->left;
            line->left += hanging[i];
        }
    return true;
}

/* Lay out [start, end) as one line: its glyphs added to the layout, how
 * far they go (pen) and how much of that is spaces hanging at its end. */
static bool set_line(VegaLayout *l, VegaShaper *shaper, const VegaFonts *fonts, const uint32_t *codes,
                     const VegaBidiClass *classes, const uint8_t *levels, uint8_t *line_levels, const VegaScript *scripts,
                     const uint8_t *font_of, Run *runs, size_t *order, int paragraph, size_t start, size_t end,
                     int32_t *pen_out, int32_t *hang_out) {
    /* L1 at the line's end: separators, and the whitespace and isolate
     * controls before them or at the end, go to the paragraph's level. */
    memcpy(line_levels + start, levels + start, end - start);
    for (size_t i = end, trailing = 1; i-- > start;) {
        VegaBidiClass c = classes[i];
        if (c == VEGA_BIDI_S || c == VEGA_BIDI_B) line_levels[i] = (uint8_t)paragraph, trailing = 1;
        else if (c == VEGA_BIDI_WS || isolate_control(c) || removed(c)) {
            if (trailing) line_levels[i] = (uint8_t)paragraph;
        } else
            trailing = 0;
    }
    size_t n = itemize(line_levels, scripts, font_of, start, end, runs);
    /* L2 over the runs: from the highest level to the lowest odd one,
     * reverse each stretch of runs at that level or higher. */
    uint8_t highest = 0, lowest_odd = 0xff;
    for (size_t r = 0; r < n; ++r) {
        order[r] = r;
        if (runs[r].level > highest) highest = runs[r].level;
        if ((runs[r].level & 1) && runs[r].level < lowest_odd) lowest_odd = runs[r].level;
    }
    for (int level = highest; lowest_odd != 0xff && level >= lowest_odd; --level)
        for (size_t k = 0; k < n;) {
            if (runs[order[k]].level < level) {
                ++k;
                continue;
            }
            size_t stop = k;
            while (stop < n && runs[order[stop]].level >= level) ++stop;
            for (size_t a = k, z = stop - 1; a < z; ++a, --z) {
                size_t swap = order[a];
                order[a] = order[z];
                order[z] = swap;
            }
            k = stop;
        }
    size_t first = l->glyph_count;
    int32_t pen = 0;
    for (size_t r = 0; r < n; ++r)
        if (!shape_run(l, shaper, fonts, codes, &runs[order[r]], &pen)) return false;
    /* The spaces at its end hang: they are not in its width. */
    size_t shown = end;
    while (shown > start && classes[shown - 1] == VEGA_BIDI_WS) --shown;
    int32_t hang = 0;
    for (size_t k = first; k < l->glyph_count; ++k)
        if (l->glyphs[k].cluster >= shown) hang += l->glyphs[k].advance;
    *pen_out = pen;
    *hang_out = hang;
    return true;
}

void vega_layout_free(VegaLayout *l) {
    free(l->lines);
    free(l->glyphs);
    free(l->work);
    *l = (VegaLayout){0};
}
