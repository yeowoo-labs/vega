/* Paragraph layout against what its parts promise (text/layout.h):
 *     test-layout SHAPED.vsym [PARAGRAPHS]
 * Random paragraphs of Latin, Arabic and Hebrew words (with marks, digits,
 * brackets, directional marks and line feeds between), set at random
 * widths. Every layout must: cover the paragraph with lines in order; end
 * a line at every mandatory break; keep each line within the width unless
 * it has no break opportunity inside; keep each glyph's cluster in its
 * line; and put glyphs in the order UAX #9 puts the line's characters
 * (vega_bidi_order, with L1 at the line's end). Prints one JSON line;
 * exits 1 on a failure (the first few shown). */
#define _POSIX_C_SOURCE 200809L
#include "layout.h"
#include "symbols.h"
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned failures;
static void fail(size_t paragraph, const char *what) {
    if (failures++ < 10) fprintf(stderr, "paragraph %zu: %s\n", paragraph, what);
}

static uint64_t state = 0x9E3779B97F4A7C15ULL;
static uint32_t below(uint32_t n) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return n ? (uint32_t)((state * 0x2545F4914F6CDD1DULL) >> 33) % n : 0;
}

typedef struct { uint32_t items[4096]; size_t count; } Pool;

static bool whitespace(VegaBidiClass c) { return c == VEGA_BIDI_WS; }
static bool trailing_kind(VegaBidiClass c) {
    return c == VEGA_BIDI_WS || c == VEGA_BIDI_LRI || c == VEGA_BIDI_RLI || c == VEGA_BIDI_FSI || c == VEGA_BIDI_PDI ||
           c == VEGA_BIDI_BN || c == VEGA_BIDI_LRE || c == VEGA_BIDI_RLE || c == VEGA_BIDI_LRO || c == VEGA_BIDI_RLO ||
           c == VEGA_BIDI_PDF;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test-layout SHAPED.vsym [PARAGRAPHS]\n");
        return 2;
    }
    size_t paragraphs = argc > 2 ? strtoul(argv[2], NULL, 10) : 3000;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *bytes = malloc(size);
    if (!bytes || fread(bytes, 1, size, f) != size) return 2;
    fclose(f);
    char error[128];
    VegaSymbols symbols;
    VegaShaping shaping;
    if (!vega_symbols_open(&symbols, bytes, size, error, sizeof(error)) || !symbols.shaping ||
        !vega_shaping_open(&shaping, symbols.shaping, symbols.shaping_size, error, sizeof(error))) {
        fprintf(stderr, "%s: %s\n", argv[1], symbols.shaping ? error : "no shaping");
        return 2;
    }
    /* Words from each script's letters (marks after some), and what goes
     * between them. */
    static Pool latin, arabic, hebrew, marks_latin, marks_arabic, marks_hebrew;
    for (uint32_t i = 0; i < symbols.symbols; ++i) {
        uint32_t code = vega_symbol_at(&symbols, i).code;
        if (code >= VEGA_GLYPH_SYMBOLS) continue;
        VegaScript script = vega_script(code);
        VegaCategory c = vega_category(code);
        bool mark = c == VEGA_MN || c == VEGA_MC;
        bool letter = c >= VEGA_LU && c <= VEGA_LO;
        Pool *p = NULL;
        if (script == VEGA_SCRIPT_LATIN && letter) p = &latin;
        else if (script == VEGA_SCRIPT_ARABIC && letter && code != 0x0640) p = &arabic;
        else if (script == VEGA_SCRIPT_HEBREW && letter) p = &hebrew;
        else if (mark && code >= 0x0300 && code <= 0x036F) p = &marks_latin;
        else if (mark && code >= 0x064B && code <= 0x0652) p = &marks_arabic;
        else if (mark && code >= 0x05B0 && code <= 0x05C2) p = &marks_hebrew;
        if (p && p->count < 4096) p->items[p->count++] = code;
    }
    if (!latin.count || !arabic.count || !hebrew.count) {
        fprintf(stderr, "%s: needs Latin, Arabic and Hebrew letters\n", argv[1]);
        return 2;
    }
    static const uint32_t between[] = {' ', ' ', ' ', ' ', ',', '.', '(', ')', '[', ']', '-', '!', '?', 0x060C, 0x061F,
                                       '1', '2', '3', 0x0661, 0x0662, 0x200E, 0x200F, 0x2028, 0x2014, 0x00A0};
    VegaShaper *shaper = vega_shaper_new();
    VegaLayout layout = {0};
    size_t lines = 0, glyphs = 0, characters = 0;
    double seconds = 0;
    static uint32_t codes[2048];
    static uint8_t breaks[2049], levels[2048], line_levels[2048];
    static VegaBidiClass classes[2048];
    static uint32_t order[2048], rank[2048];
    for (size_t k = 0; k < paragraphs; ++k) {
        size_t n = 0;
        unsigned words = 1 + below(40);
        for (unsigned w = 0; w < words && n < 1900; ++w) {
            unsigned which = below(10);
            const Pool *letters = which < 5 ? &latin : which < 8 ? &arabic : &hebrew;
            const Pool *marks = which < 5 ? &marks_latin : which < 8 ? &marks_arabic : &marks_hebrew;
            if (w) {
                codes[n++] = between[below(sizeof(between) / sizeof(*between))];
                if (below(3)) codes[n++] = ' ';
            }
            unsigned length = 1 + below(12);
            for (unsigned c = 0; c < length; ++c) {
                codes[n++] = letters->items[below((uint32_t)letters->count)];
                if (marks->count && below(100) < (which < 5 ? 5u : 30u)) codes[n++] = marks->items[below((uint32_t)marks->count)];
            }
        }
        int direction = below(3) == 0 ? VEGA_BIDI_AUTO : below(2) ? VEGA_BIDI_LTR : VEGA_BIDI_RTL;
        int32_t width = below(5) == 0 ? 0 : (int32_t)(shaping.upem * (1 + below(30)));
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (!vega_layout(&layout, shaper, &shaping, codes, n, direction, width)) {
            fail(k, "no layout");
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        seconds += (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
        lines += layout.line_count;
        glyphs += layout.glyph_count;
        characters += n;
        vega_line_breaks(codes, n, breaks);
        for (size_t i = 0; i < n; ++i) classes[i] = vega_bidi_class(codes[i]);
        int paragraph = vega_bidi_levels(classes, codes, n, direction, levels);
        size_t expect = 0;
        for (size_t i = 0; i < layout.line_count; ++i) {
            const VegaLine *line = &layout.lines[i];
            if (line->start != expect || line->end <= line->start || line->end > n) {
                fail(k, "lines do not cover the paragraph in order");
                break;
            }
            expect = line->end;
            for (size_t p = line->start + 1; p < line->end; ++p)
                if (breaks[p] == VEGA_BREAK_MANDATORY) fail(k, "a line runs past a mandatory break");
            if (width && line->width > width) {
                bool inside = false;
                for (size_t p = line->start + 1; p < line->end && !inside; ++p) {
                    /* an opportunity after only spaces is no way to shorten the line */
                    bool spaces_only = true;
                    for (size_t q = p; q < line->end && spaces_only; ++q) spaces_only = whitespace(classes[q]);
                    inside = breaks[p] != VEGA_BREAK_NONE && !spaces_only;
                }
                if (inside) fail(k, "a line wider than asked though it could break inside");
            }
            for (size_t g = line->first; g < line->first + line->count; ++g)
                if (layout.glyphs[g].cluster < line->start || layout.glyphs[g].cluster >= line->end)
                    fail(k, "a glyph's cluster is outside its line");
            /* L1 at the line's end, then the characters' visual order. */
            memcpy(line_levels, levels + line->start, line->end - line->start);
            for (size_t i2 = line->end, trailing = 1; i2-- > line->start;) {
                VegaBidiClass c = classes[i2];
                if (c == VEGA_BIDI_S || c == VEGA_BIDI_B) line_levels[i2 - line->start] = (uint8_t)paragraph, trailing = 1;
                else if (trailing_kind(c)) {
                    if (trailing && line_levels[i2 - line->start] != VEGA_BIDI_REMOVED)
                        line_levels[i2 - line->start] = (uint8_t)paragraph;
                } else
                    trailing = 0;
            }
            size_t shown = vega_bidi_order(line_levels, line->end - line->start, order);
            for (size_t r = 0; r < line->end - line->start; ++r) rank[r] = UINT32_MAX;
            for (size_t r = 0; r < shown; ++r) rank[order[r]] = (uint32_t)r;
            uint32_t last = 0;
            for (size_t g = line->first; g < line->first + line->count; ++g) {
                uint32_t r = rank[layout.glyphs[g].cluster - line->start];
                if (r == UINT32_MAX) continue;
                if (r < last) {
                    fail(k, "glyphs out of the characters' visual order");
                    break;
                }
                last = r;
            }
        }
        if (expect != n) fail(k, "lines end before the paragraph does");
    }
    printf("{\"paragraphs\":%zu,\"characters\":%zu,\"lines\":%zu,\"glyphs\":%zu,\"ns_per_character\":%.1f,"
           "\"failures\":%u}\n",
           paragraphs, characters, lines, glyphs, characters ? 1e9 * seconds / (double)characters : 0, failures);
    vega_layout_free(&layout);
    vega_shaper_free(shaper);
    vega_shaping_close(&shaping);
    free(bytes);
    return failures ? 1 : 0;
}
