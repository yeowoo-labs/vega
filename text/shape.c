/* Shaping as HarfBuzz 10 does it (text/shape.h), with its default, Arabic,
 * Hebrew, Hangul and Indic shapers. The code follows HarfBuzz's own step
 * for step (hb-ot-shape.cc, hb-ot-shape-normalize.cc, hb-buffer.cc,
 * hb-ot-layout-gsubgpos.hh, the GSUB and GPOS lookups, hb-ot-shaper-*.cc),
 * under HarfBuzz's names, so that the two read side by side; the build
 * tool compares the results with HarfBuzz's (symbols-tool.c). */
#include "shape.h"
#include "syllabic.h"
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* ---- OpenType tables, read in place: a read past a table's end gives 0 */

static uint32_t get16(VegaTable t, uint32_t at) {
    return at < t.size && t.size - at >= 2 ? (uint32_t)t.p[at] << 8 | t.p[at + 1] : 0;
}
static int32_t gets16(VegaTable t, uint32_t at) { return (int16_t)get16(t, at); }
static uint32_t get32(VegaTable t, uint32_t at) {
    return at < t.size && t.size - at >= 4
               ? (uint32_t)t.p[at] << 24 | (uint32_t)t.p[at + 1] << 16 | (uint32_t)t.p[at + 2] << 8 | t.p[at + 3]
               : 0;
}
/* What the Offset16 at `at` points to from base; 0 is none (a table's
 * start is never an offset's target). */
static uint32_t offset16(VegaTable t, uint32_t base, uint32_t at) {
    uint32_t o = get16(t, at);
    return o ? base + o : 0;
}

#define NOT_COVERED 0xFFFFFFFFu

static uint32_t coverage(VegaTable t, uint32_t at, uint32_t glyph) {
    if (!at) return NOT_COVERED;
    uint32_t format = get16(t, at), count = get16(t, at + 2), low = 0, high = count;
    if (format == 1)
        while (low < high) {
            uint32_t middle = (low + high) / 2, g = get16(t, at + 4 + 2 * middle);
            if (g < glyph) low = middle + 1;
            else if (g > glyph) high = middle;
            else return middle;
        }
    else if (format == 2)
        while (low < high) {
            uint32_t middle = (low + high) / 2, r = at + 4 + 6 * middle;
            if (get16(t, r + 2) < glyph) low = middle + 1;
            else if (get16(t, r) > glyph) high = middle;
            else return get16(t, r + 4) + glyph - get16(t, r);
        }
    return NOT_COVERED;
}

static uint32_t class_of(VegaTable t, uint32_t at, uint32_t glyph) {
    if (!at) return 0;
    uint32_t format = get16(t, at);
    if (format == 1) {
        uint32_t start = get16(t, at + 2), count = get16(t, at + 4);
        return glyph >= start && glyph - start < count ? get16(t, at + 6 + 2 * (glyph - start)) : 0;
    }
    if (format == 2) {
        uint32_t low = 0, high = get16(t, at + 2);
        while (low < high) {
            uint32_t middle = (low + high) / 2, r = at + 4 + 6 * middle;
            if (get16(t, r + 2) < glyph) low = middle + 1;
            else if (get16(t, r) > glyph) high = middle;
            else return get16(t, r + 4);
        }
    }
    return 0;
}

/* ---- The section */

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static bool within(size_t size, uint64_t offset, uint64_t length) { return offset <= size && length <= size - offset; }

#define REFUSE(...)                                  \
    do {                                             \
        snprintf(error, error_size, __VA_ARGS__);    \
        vega_shaping_close(s);                       \
        return false;                                \
    } while (0)

/* ---- Digests */

static const unsigned digest_shifts[3] = {4, 0, 9};

static void digest_add_range(VegaDigest *d, uint32_t first, uint32_t last) {
    for (int k = 0; k < 3; ++k) {
        uint32_t a = first >> digest_shifts[k], b = last >> digest_shifts[k];
        if (b - a >= 63) d->mask[k] = ~0ULL;
        else
            for (uint32_t v = a; v <= b; ++v) d->mask[k] |= 1ULL << (v & 63);
    }
}
static bool digest_may_have(const VegaDigest *d, uint32_t glyph) {
    for (int k = 0; k < 3; ++k)
        if (!(d->mask[k] >> (glyph >> digest_shifts[k] & 63) & 1)) return false;
    return true;
}
static bool digest_may_intersect(const VegaDigest *a, const VegaDigest *b) {
    for (int k = 0; k < 3; ++k)
        if (!(a->mask[k] & b->mask[k])) return false;
    return true;
}

static void digest_add_coverage(VegaDigest *d, VegaTable t, uint32_t at) {
    if (!at) return;
    uint32_t format = get16(t, at), count = get16(t, at + 2);
    for (uint32_t i = 0; i < count; ++i)
        if (format == 1) digest_add_range(d, get16(t, at + 4 + 2 * i), get16(t, at + 4 + 2 * i));
        else if (format == 2) digest_add_range(d, get16(t, at + 4 + 6 * i), get16(t, at + 6 + 6 * i));
}

/* The coverage a subtable starts from: the first glyph's. */
static uint32_t first_coverage(VegaTable t, int table, const VegaSubtable *st) {
    uint32_t at = st->at;
    if (!at) return 0;
    unsigned context = table ? 7 : 5, chain = table ? 8 : 6;
    if ((st->type == context || st->type == chain) && get16(t, at) == 3) {
        if (st->type == context) return offset16(t, at, at + 6);
        return offset16(t, at, at + 4 + 2 * get16(t, at + 2) + 2);
    }
    return offset16(t, at, at + 2);
}

/* A table's lookups (table 0 GSUB, 1 GPOS), their subtables' types
 * resolved through extensions. */
static bool resolve_lookups(VegaShaping *s, VegaTable t, int table, VegaLookup **out, uint32_t *out_count,
                            size_t *subtables, size_t *allocated) {
    uint32_t list = t.size ? offset16(t, 0, 8) : 0, count = list ? get16(t, list) : 0;
    *out_count = count;
    if (!(*out = calloc(count ? count : 1, sizeof(VegaLookup)))) return false;
    uint16_t extension = table ? 9 : 7;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = offset16(t, list, list + 2 + 2 * i), n = at ? get16(t, at + 4) : 0;
        VegaLookup *l = &(*out)[i];
        l->type = (uint16_t)get16(t, at);
        l->flags = (uint16_t)get16(t, at + 2);
        l->props = l->flags | (l->flags & 0x10 ? get16(t, at + 6 + 2 * n) << 16 : 0);
        l->first = (uint32_t)*subtables;
        l->count = n;
        for (uint32_t k = 0; k < n; ++k) {
            if (*subtables == *allocated) {
                *allocated = *allocated ? 2 * *allocated : 64;
                VegaSubtable *grown = realloc(s->subtables, *allocated * sizeof(*grown));
                if (!grown) return false;
                s->subtables = grown;
            }
            uint32_t st = offset16(t, at, at + 6 + 2 * k);
            uint16_t type = l->type;
            if (type == extension) {
                uint32_t target = get32(t, st + 4);
                type = get16(t, st) == 1 && target ? (uint16_t)get16(t, st + 2) : 0;
                st = type && type != extension ? st + target : 0;
                if (!st) type = 0;
            }
            s->subtables[(*subtables)++] = (VegaSubtable){type, st, {{0, 0, 0}}};
        }
        /* HarfBuzz: reverse by the lookup's type, or its first extension's. */
        l->reverse = !table && (l->type == 8 || (l->type == 7 && n && s->subtables[l->first].type == 8));
        for (uint32_t k = 0; k < n; ++k) {
            VegaSubtable *st = &s->subtables[l->first + k];
            digest_add_coverage(&st->digest, t, first_coverage(t, table, st));
            for (int m = 0; m < 3; ++m) l->digest.mask[m] |= st->digest.mask[m];
        }
    }
    return true;
}

bool vega_shaping_open(VegaShaping *s, const unsigned char *bytes, size_t size, char *error, size_t error_size) {
    *s = (VegaShaping){0};
    if (size < VEGA_SHAPING_HEADER || memcmp(bytes, "VSHP", 4) || le32(bytes + 4) != size)
        REFUSE("not a shaping section");
    s->bytes = bytes;
    s->size = size;
    s->upem = le16(bytes + 8);
    s->glyph_count = le16(bytes + 10);
    s->cmap_groups = le32(bytes + 12);
    uint32_t cmap = le32(bytes + 16), advances = le32(bytes + 20);
    if (!s->upem) REFUSE("units per em");
    if (!within(size, cmap, (uint64_t)s->cmap_groups * 12)) REFUSE("character map");
    s->cmap = bytes + cmap;
    for (uint32_t i = 0; i < s->cmap_groups; ++i) {
        const unsigned char *g = s->cmap + 12 * (size_t)i;
        uint32_t first = le32(g), last = le32(g + 4), glyph = le32(g + 8) & 0x7FFFFFFF;
        bool same = le32(g + 8) >> 31;
        if ((i && first <= le32(g - 8)) || last < first || last > 0x10FFFF || !glyph ||
            (uint64_t)glyph + (same ? 0 : last - first) >= s->glyph_count)
            REFUSE("character map: group %u", i);
    }
    for (uint32_t i = 0; i < s->cmap_groups; ++i) {
        const unsigned char *g = s->cmap + 12 * (size_t)i;
        uint32_t first = le32(g), last = le32(g + 4), glyph = le32(g + 8) & 0x7FFFFFFF;
        bool same = le32(g + 8) >> 31;
        if (first > 0xFFFF) break;
        for (uint32_t code = first; code <= last && code <= 0xFFFF; ++code) {
            uint16_t **page = &s->bmp[code >> 8];
            if (!*page && !(*page = calloc(256, sizeof(**page)))) REFUSE("out of memory");
            (*page)[code & 255] = (uint16_t)(same ? glyph : glyph + (code - first));
        }
    }
    if (!within(size, advances, 2 * (uint64_t)s->glyph_count)) REFUSE("advances");
    s->advances = bytes + advances;
    VegaTable *tables[3] = {&s->gdef, &s->gsub, &s->gpos};
    for (int k = 0; k < 3; ++k) {
        uint32_t at = le32(bytes + 24 + 8 * k), n = le32(bytes + 28 + 8 * k);
        if (!within(size, at, n)) REFUSE("layout table %d", k);
        *tables[k] = (VegaTable){n ? bytes + at : NULL, n};
    }
    uint32_t kern = le32(bytes + 64), kern_size = le32(bytes + 68), extents = le32(bytes + 72);
    if (!within(size, kern, kern_size)) REFUSE("kern table");
    s->kern = (VegaTable){kern_size ? bytes + kern : NULL, kern_size};
    if (extents && !within(size, extents, 8 * (uint64_t)s->glyph_count)) REFUSE("extents");
    s->extents = extents ? bytes + extents : NULL;
    s->plan_count = le32(bytes + 48);
    uint32_t plans = le32(bytes + 52), step_count = le32(bytes + 56), steps = le32(bytes + 60);
    if (!within(size, plans, (uint64_t)s->plan_count * VEGA_PLAN_BYTES)) REFUSE("plans");
    if (!within(size, steps, (uint64_t)step_count * VEGA_STEP_BYTES)) REFUSE("steps");
    s->plans = bytes + plans;
    s->steps = bytes + steps;
    if (s->gdef.size) {
        uint32_t minor = get16(s->gdef, 2);
        s->glyph_classes = offset16(s->gdef, 0, 4);
        s->attach_classes = offset16(s->gdef, 0, 10);
        s->mark_sets = minor >= 2 ? offset16(s->gdef, 0, 12) : 0;
    }
    s->sequence_count = le32(bytes + 80);
    uint32_t sequences = le32(bytes + 84);
    if (!within(size, sequences, (uint64_t)s->sequence_count * 12)) REFUSE("variation sequences");
    s->sequences = bytes + sequences;
    for (uint32_t i = 0; i < s->sequence_count; ++i) {
        const unsigned char *q = s->sequences + 12 * (size_t)i;
        if ((i && (le32(q) < le32(q - 12) || (le32(q) == le32(q - 12) && le32(q + 4) <= le32(q - 8)))) ||
            le32(q + 8) >= s->glyph_count)
            REFUSE("variation sequence %u", i);
    }
    size_t subtables = 0, allocated = 0;
    uint32_t fallback = le32(bytes + 76);
    if (fallback) {
        if (!within(size, fallback, 12) || bytes[fallback] > 7 || !within(size, fallback + 12, le32(bytes + fallback + 8)))
            REFUSE("fallback lookups");
        s->fallback = (VegaTable){bytes + fallback + 12, le32(bytes + fallback + 8)};
        memcpy(s->fallback_features, bytes + fallback + 1, 7);
    }
    if (!resolve_lookups(s, s->gsub, 0, &s->lookups[0], &s->lookup_count[0], &subtables, &allocated) ||
        !resolve_lookups(s, s->gpos, 1, &s->lookups[1], &s->lookup_count[1], &subtables, &allocated) ||
        !resolve_lookups(s, s->fallback, 0, &s->fallback_lookups, &s->fallback_count, &subtables, &allocated))
        REFUSE("out of memory");
    if (fallback && s->fallback_count != bytes[fallback]) REFUSE("fallback lookups");
    for (uint32_t i = 0; i < s->fallback_count; ++i)
        if (s->fallback_features[i] > 6) REFUSE("fallback lookups");
    for (uint32_t i = 0; i < s->plan_count; ++i) {
        const unsigned char *p = s->plans + (size_t)i * VEGA_PLAN_BYTES;
        uint32_t ranges[2][2] = {{le32(p + 80), le32(p + 84)}, {le32(p + 88), le32(p + 92)}};
        if ((p[4] != VEGA_SHAPE_LTR && p[4] != VEGA_SHAPE_RTL) || p[5] > VEGA_SHAPE_RTL || p[6] > VEGA_SHAPER_THAI)
            REFUSE("plan %u", i);
        if ((p[7] & VEGA_PLAN_FALLBACK_MARKS) && !s->extents) REFUSE("plan %u: marks placed without extents", i);
        for (int table = 0; table < 2; ++table) {
            if (ranges[table][0] > step_count || ranges[table][1] > step_count - ranges[table][0])
                REFUSE("plan %u: steps", i);
            for (uint32_t k = 0; k < ranges[table][1]; ++k) {
                const unsigned char *step = s->steps + (size_t)(ranges[table][0] + k) * VEGA_STEP_BYTES;
                bool pause = le16(step) == VEGA_STEP_PAUSE;
                if (pause ? table || !step[2] || step[2] > VEGA_PAUSE_RECORD_STCH : le16(step) >= s->lookup_count[table])
                    REFUSE("plan %u: a lookup the font lacks", i);
            }
        }
        for (int k = 0; k < VEGA_PLAN_STAGES; ++k)
            if ((uint32_t)le16(p + 100 + 8 * k) + le16(p + 102 + 8 * k) > ranges[0][1]) REFUSE("plan %u: stages", i);
    }
    return true;
}

void vega_shaping_close(VegaShaping *s) {
    for (int i = 0; i < 256; ++i) free(s->bmp[i]), s->bmp[i] = NULL;
    free(s->lookups[0]);
    free(s->lookups[1]);
    free(s->fallback_lookups);
    s->fallback_lookups = NULL;
    free(s->subtables);
    s->lookups[0] = s->lookups[1] = NULL;
    s->subtables = NULL;
}

uint32_t vega_shaping_glyph(const VegaShaping *s, uint32_t code) {
    if (code <= 0xFFFF) return s->bmp[code >> 8] ? s->bmp[code >> 8][code & 255] : 0;
    uint32_t low = 0, high = s->cmap_groups;
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        const unsigned char *g = s->cmap + 12 * (size_t)middle;
        if (le32(g + 4) < code) low = middle + 1;
        else if (le32(g) > code) high = middle;
        else return le32(g + 8) >> 31 ? le32(g + 8) & 0x7FFFFFFF : le32(g + 8) + (code - le32(g));
    }
    return 0;
}

static int32_t advance_of(const VegaShaping *s, uint32_t glyph) {
    return glyph < s->glyph_count ? le16(s->advances + 2 * glyph) : 0;
}

/* glyph_props: GDEF's class as the lookup flags that skip it, a mark's
 * attachment class in the high byte; the flags substitution sets. */
enum { BASE_GLYPH = 0x02, LIGATURE = 0x04, MARK = 0x08, SUBSTITUTED = 0x10, LIGATED = 0x20, MULTIPLIED = 0x40,
       PRESERVE = SUBSTITUTED | LIGATED | MULTIPLIED };

static uint16_t glyph_props(const VegaShaping *s, uint32_t glyph) {
    switch (class_of(s->gdef, s->glyph_classes, glyph)) {
    case 1: return BASE_GLYPH;
    case 2: return LIGATURE;
    case 3: return (uint16_t)(MARK | class_of(s->gdef, s->attach_classes, glyph) << 8);
    default: return 0;
    }
}

static bool mark_set_covers(const VegaShaping *s, uint32_t set, uint32_t glyph) {
    if (!s->mark_sets || get16(s->gdef, s->mark_sets) != 1 || set >= get16(s->gdef, s->mark_sets + 2)) return false;
    uint32_t at = get32(s->gdef, s->mark_sets + 4 + 4 * set);
    return at && coverage(s->gdef, s->mark_sets + at, glyph) != NOT_COVERED;
}

/* ---- The buffer (hb-buffer.cc): glyphs in, and glyphs out of the lookup
 * that is running; positions once substitution is done. */

typedef struct {
    uint32_t codepoint; /* a character, then (map_glyphs) a glyph */
    uint32_t mask, cluster, glyph_index;
    uint16_t uprops, gprops;
    uint8_t lig_props;
    union {
        uint8_t action;   /* Arabic's joining action, Hangul's jamo feature */
        uint8_t position; /* Indic's */
    };
    uint8_t category;     /* Indic's */
    uint8_t syllable;     /* serial << 4 | type, once a shaper finds syllables */
} Info;
typedef struct {
    int32_t x_advance, y_advance, x_offset, y_offset;
    int32_t chain;
    uint8_t attach;
} Position;

struct VegaShaper {
    Info *info, *out;
    Position *pos;
    uint32_t *kept; /* the glyphs a syllable machine reads (USE's) */
    size_t len, out_len, idx, allocated, max_len;
    bool have_output, successful;
    int direction;
    uint32_t scratch, serial;
    long max_ops;
    VegaShaped *glyphs;
    size_t glyphs_allocated;
};

enum { HAS_NON_ASCII = 1, HAS_DEFAULT_IGNORABLES = 2, HAS_SPACE_FALLBACK = 4, HAS_GPOS_ATTACHMENT = 8, HAS_CGJ = 16,
       HAS_VARIATION_SELECTOR_FALLBACK = 32, HAS_BROKEN_SYLLABLE = 64, HAS_STCH = 128 };

VegaShaper *vega_shaper_new(void) { return calloc(1, sizeof(VegaShaper)); }

void vega_shaper_free(VegaShaper *b) {
    if (!b) return;
    free(b->info);
    free(b->out);
    free(b->pos);
    free(b->kept);
    free(b->glyphs);
    free(b);
}

static bool ensure(VegaShaper *b, size_t size) {
    if (size <= b->allocated) return true;
    if (!b->successful) return false;
    if (size > b->max_len) {
        b->successful = false;
        return false;
    }
    size_t n = b->allocated ? b->allocated : 32;
    while (n < size) n *= 2;
    Info *info = realloc(b->info, n * sizeof(*info));
    if (info) b->info = info;
    Info *out = info ? realloc(b->out, n * sizeof(*out)) : NULL;
    if (out) b->out = out;
    Position *pos = out ? realloc(b->pos, n * sizeof(*pos)) : NULL;
    if (pos) b->pos = pos;
    uint32_t *kept = pos ? realloc(b->kept, n * sizeof(*kept)) : NULL;
    if (!kept) {
        b->successful = false;
        return false;
    }
    b->kept = kept;
    b->allocated = n;
    return true;
}

/* The glyphs before idx: out while a lookup writes, else info itself. */
static Info *out_info(VegaShaper *b) { return b->have_output ? b->out : b->info; }
static size_t backtrack_len(const VegaShaper *b) { return b->have_output ? b->out_len : b->idx; }
static size_t lookahead_len(const VegaShaper *b) { return b->len - b->idx; }

static void clear_output(VegaShaper *b) {
    b->have_output = true;
    b->idx = b->out_len = 0;
}

static void next_glyph(VegaShaper *b) {
    if (b->have_output) {
        if (!ensure(b, b->out_len + 1)) return;
        b->out[b->out_len++] = b->info[b->idx];
    }
    b->idx++;
}

static void next_glyphs(VegaShaper *b, size_t n) {
    if (b->have_output) {
        if (!ensure(b, b->out_len + n)) return;
        memmove(b->out + b->out_len, b->info + b->idx, n * sizeof(Info));
        b->out_len += n;
    }
    b->idx += n;
}

static void skip_glyph(VegaShaper *b) { b->idx++; }

static void replace_glyph(VegaShaper *b, uint32_t glyph) {
    if (!ensure(b, b->out_len + 1)) return;
    b->out[b->out_len] = b->info[b->idx];
    b->out[b->out_len++].codepoint = glyph;
    b->idx++;
}

static void output_glyph(VegaShaper *b, uint32_t glyph) {
    if (!ensure(b, b->out_len + 1)) return;
    if (b->idx == b->len && !b->out_len) return;
    b->out[b->out_len] = b->idx < b->len ? b->info[b->idx] : b->out[b->out_len - 1];
    b->out[b->out_len++].codepoint = glyph;
}

static void output_info(VegaShaper *b, Info info) {
    if (!ensure(b, b->out_len + 1)) return;
    b->out[b->out_len++] = info;
}

/* num_in glyphs become num_out, each a copy of the first with its own
 * codepoint; their clusters merge. */
static void merge_clusters(VegaShaper *b, size_t start, size_t end);
static void replace_glyphs(VegaShaper *b, size_t num_in, size_t num_out, const uint32_t *glyphs) {
    if (!ensure(b, b->out_len + num_out)) return;
    merge_clusters(b, b->idx, b->idx + num_in);
    Info original = b->idx < b->len ? b->info[b->idx] : b->out[b->out_len - 1];
    for (size_t i = 0; i < num_out; ++i) {
        b->out[b->out_len + i] = original;
        b->out[b->out_len + i].codepoint = glyphs[i];
    }
    b->idx += num_in;
    b->out_len += num_out;
}

static void sync(VegaShaper *b) {
    if (!b->successful) {
        b->have_output = false;
        b->out_len = b->idx = 0;
        return;
    }
    next_glyphs(b, b->len - b->idx);
    Info *swap = b->info;
    b->info = b->out;
    b->out = swap;
    b->len = b->out_len;
    b->have_output = false;
    b->out_len = b->idx = 0;
}

static bool shift_forward(VegaShaper *b, size_t count) {
    if (!ensure(b, b->len + count)) return false;
    memmove(b->info + b->idx + count, b->info + b->idx, (b->len - b->idx) * sizeof(Info));
    if (b->idx + count > b->len) memset(b->info + b->len, 0, (b->idx + count - b->len) * sizeof(Info));
    b->len += count;
    b->idx += count;
    return true;
}

static bool move_to(VegaShaper *b, size_t i) {
    if (!b->have_output) {
        b->idx = i;
        return true;
    }
    if (!b->successful) return false;
    if (b->out_len < i) {
        size_t count = i - b->out_len;
        if (!ensure(b, b->out_len + count)) return false;
        memmove(b->out + b->out_len, b->info + b->idx, count * sizeof(Info));
        b->idx += count;
        b->out_len += count;
    } else if (b->out_len > i) {
        size_t count = b->out_len - i;
        if (b->idx < count && !shift_forward(b, count - b->idx)) return false;
        b->idx -= count;
        b->out_len -= count;
        memmove(b->info + b->idx, b->out + b->out_len, count * sizeof(Info));
    }
    return true;
}

static void merge_clusters(VegaShaper *b, size_t start, size_t end) {
    if (end - start < 2) return;
    uint32_t cluster = b->info[start].cluster;
    for (size_t i = start + 1; i < end; ++i)
        if (b->info[i].cluster < cluster) cluster = b->info[i].cluster;
    if (cluster != b->info[end - 1].cluster)
        while (end < b->len && b->info[end - 1].cluster == b->info[end].cluster) end++;
    if (cluster != b->info[start].cluster)
        while (b->idx < start && b->info[start - 1].cluster == b->info[start].cluster) start--;
    if (b->idx == start && b->info[start].cluster != cluster)
        for (size_t i = b->out_len; i && b->out[i - 1].cluster == b->info[start].cluster; i--)
            b->out[i - 1].cluster = cluster;
    for (size_t i = start; i < end; ++i) b->info[i].cluster = cluster;
}

static void merge_out_clusters(VegaShaper *b, size_t start, size_t end) {
    if (end - start < 2) return;
    uint32_t cluster = b->out[start].cluster;
    for (size_t i = start + 1; i < end; ++i)
        if (b->out[i].cluster < cluster) cluster = b->out[i].cluster;
    while (start && b->out[start - 1].cluster == b->out[start].cluster) start--;
    while (end < b->out_len && b->out[end - 1].cluster == b->out[end].cluster) end++;
    if (end == b->out_len)
        for (size_t i = b->idx; i < b->len && b->info[i].cluster == b->out[end - 1].cluster; ++i)
            b->info[i].cluster = cluster;
    for (size_t i = start; i < end; ++i) b->out[i].cluster = cluster;
}

/* Delete the current glyph, keeping its cluster on a neighbour. */
static void delete_glyph(VegaShaper *b) {
    uint32_t cluster = b->info[b->idx].cluster;
    if ((b->idx + 1 < b->len && cluster == b->info[b->idx + 1].cluster) ||
        (b->out_len && cluster == b->out[b->out_len - 1].cluster)) {
        skip_glyph(b);
        return;
    }
    if (b->out_len) {
        if (cluster < b->out[b->out_len - 1].cluster) {
            uint32_t old = b->out[b->out_len - 1].cluster;
            for (size_t i = b->out_len; i && b->out[i - 1].cluster == old; i--) b->out[i - 1].cluster = cluster;
        }
        skip_glyph(b);
        return;
    }
    if (b->idx + 1 < b->len) merge_clusters(b, b->idx, b->idx + 2);
    skip_glyph(b);
}

static void reverse_range(VegaShaper *b, size_t start, size_t end, bool positions) {
    for (size_t i = start, j = end; i + 1 < j; ++i, --j) {
        Info t = b->info[i];
        b->info[i] = b->info[j - 1];
        b->info[j - 1] = t;
        if (positions) {
            Position p = b->pos[i];
            b->pos[i] = b->pos[j - 1];
            b->pos[j - 1] = p;
        }
    }
}

static uint8_t allocate_lig_id(VegaShaper *b) {
    for (;;) {
        if (!++b->serial) ++b->serial;
        uint8_t id = b->serial & 7;
        if (id) return id;
    }
}

/* ---- Characters' properties (unicode_props) */

enum { UPROPS_IGNORABLE = 0x20, UPROPS_HIDDEN = 0x40, UPROPS_CONTINUATION = 0x80, UPROPS_ZWJ = 0x100,
       UPROPS_ZWNJ = 0x200, UPROPS_VS = 0x400 };

static unsigned gen_cat(const Info *i) { return i->uprops & 0x1F; }
static bool is_mark_category(unsigned c) { return c == VEGA_MN || c == VEGA_MC || c == VEGA_ME; }
static bool is_unicode_mark(const Info *i) { return is_mark_category(gen_cat(i)); }
static unsigned info_cc(const Info *i) { return is_unicode_mark(i) ? i->uprops >> 8 : 0; }
static void set_info_cc(Info *i, unsigned cc) {
    if (is_unicode_mark(i)) i->uprops = (uint16_t)(cc << 8 | (i->uprops & 0xFF));
}
static bool is_unicode_space(const Info *i) { return gen_cat(i) == VEGA_ZS; }
static bool substituted(const Info *i) { return i->gprops & SUBSTITUTED; }
static bool is_default_ignorable(const Info *i) { return (i->uprops & UPROPS_IGNORABLE) && !substituted(i); }
static bool is_zwnj(const Info *i) { return gen_cat(i) == VEGA_CF && (i->uprops & UPROPS_ZWNJ); }
static bool is_zwj(const Info *i) { return gen_cat(i) == VEGA_CF && (i->uprops & UPROPS_ZWJ); }
static bool is_continuation(const Info *i) { return i->uprops & UPROPS_CONTINUATION; }
static bool is_letter(unsigned c) { return c >= VEGA_LU && c <= VEGA_LO; }
/* The categories set_unicode_props passes over (not Lm). */
static bool cased_or_other_letter_or_space(unsigned c) {
    return c == VEGA_LU || c == VEGA_LL || c == VEGA_LT || c == VEGA_LO || c == VEGA_ZS;
}

/* HarfBuzz's Default_Ignorable: Unicode's, less the Hangul fillers and a
 * few others that fonts draw. */
bool vega_default_ignorable(uint32_t ch) {
    uint32_t plane = ch >> 16;
    if (!plane) {
        switch (ch >> 8) {
        case 0x00: return ch == 0x00AD;
        case 0x03: return ch == 0x034F;
        case 0x06: return ch == 0x061C;
        case 0x17: return ch >= 0x17B4 && ch <= 0x17B5;
        case 0x18: return ch >= 0x180B && ch <= 0x180E; /* HarfBuzz 10.2 leaves out U+180F */
        case 0x20: return (ch >= 0x200B && ch <= 0x200F) || (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2060 && ch <= 0x206F);
        case 0xFE: return (ch >= 0xFE00 && ch <= 0xFE0F) || ch == 0xFEFF;
        case 0xFF: return ch >= 0xFFF0 && ch <= 0xFFF8;
        default: return false;
        }
    }
    if (plane == 1) return ch >= 0x1D173 && ch <= 0x1D17A;
    return plane == 0x0E && ch <= 0xE0FFF;
}

static bool variation_selector(uint32_t ch) { return (ch >= 0xFE00 && ch <= 0xFE0F) || (ch >= 0xE0100 && ch <= 0xE01EF); }

/* Canonical_Combining_Class as HarfBuzz reorders by: Hebrew's fixed
 * positions in the SBL order, Arabic's shadda before the other marks,
 * Telugu's length marks before the virama (hb-unicode.hh). */
static unsigned modified_combining_class(uint32_t u) {
    if (u == 0x1A60 || u == 0x0FC6) return 254;
    if (u == 0x0F39) return 127;
    static const uint8_t hebrew_arabic[27] = {22, 15, 16, 17, 23, 18, 19, 20, 21, 14, 24, 12, 25, 13,
                                              10, 11, 26, 28, 29, 30, 31, 32, 33, 27, 34, 35, 36};
    unsigned c = vega_combining_class(u);
    if (c >= 10 && c <= 36) return hebrew_arabic[c - 10];
    switch (c) {
    case 84: return 4;
    case 91: return 5;
    case 103: return 3;
    case 130: return 132;
    case 132: return 131;
    default: return c;
    }
}

enum { NOT_SPACE = 0, SPACE_EM = 1, SPACE_EM_2 = 2, SPACE_EM_3 = 3, SPACE_EM_4 = 4, SPACE_EM_5 = 5, SPACE_EM_6 = 6,
       SPACE_EM_16 = 16, SPACE_4_EM_18 = 17, SPACE = 18, SPACE_FIGURE = 19, SPACE_PUNCTUATION = 20, SPACE_NARROW = 21 };

static unsigned space_fallback(uint32_t u) {
    switch (u) {
    case 0x0020: case 0x00A0: return SPACE;
    case 0x2000: case 0x2002: return SPACE_EM_2;
    case 0x2001: case 0x2003: case 0x3000: return SPACE_EM;
    case 0x2004: return SPACE_EM_3;
    case 0x2005: return SPACE_EM_4;
    case 0x2006: return SPACE_EM_6;
    case 0x2007: return SPACE_FIGURE;
    case 0x2008: return SPACE_PUNCTUATION;
    case 0x2009: return SPACE_EM_5;
    case 0x200A: return SPACE_EM_16;
    case 0x202F: return SPACE_NARROW;
    case 0x205F: return SPACE_4_EM_18;
    default: return NOT_SPACE;
    }
}

static void init_unicode_props(Info *info, uint32_t *scratch) {
    uint32_t u = info->codepoint;
    unsigned gen = vega_category(u), props = gen;
    if (u >= 0x80) {
        *scratch |= HAS_NON_ASCII;
        if (vega_default_ignorable(u)) {
            *scratch |= HAS_DEFAULT_IGNORABLES;
            props |= UPROPS_IGNORABLE;
            if (u == 0x200C) props |= UPROPS_ZWNJ;
            else if (u == 0x200D) props |= UPROPS_ZWJ;
            else if ((u >= 0x180B && u <= 0x180D) || u == 0x180F) props |= UPROPS_HIDDEN;
            else if (u >= 0xE0020 && u <= 0xE007F) props |= UPROPS_HIDDEN;
            else if (u == 0x034F) {
                *scratch |= HAS_CGJ;
                props |= UPROPS_HIDDEN;
            }
        }
        if (is_mark_category(gen)) props |= UPROPS_CONTINUATION | modified_combining_class(u) << 8;
    }
    info->uprops = (uint16_t)props;
}

static bool regional_indicator(uint32_t u) { return u >= 0x1F1E6 && u <= 0x1F1FF; }

/* Enough of Unicode's graphemes that shaping backwards keeps them whole:
 * marks, ZWJ (and the pictograph after it), emoji modifiers, pairs of
 * regional indicators, and a few others continue the character before. */
static void set_unicode_props(VegaShaper *b) {
    for (size_t i = 0; i < b->len; ++i) {
        Info *info = &b->info[i];
        init_unicode_props(info, &b->scratch);
        unsigned gen = gen_cat(info);
        if (cased_or_other_letter_or_space(gen)) continue;
        uint32_t u = info->codepoint;
        if (gen == VEGA_SK && u >= 0x1F3FB && u <= 0x1F3FF) info->uprops |= UPROPS_CONTINUATION;
        else if (i && regional_indicator(u)) {
            if (regional_indicator(b->info[i - 1].codepoint) && !is_continuation(&b->info[i - 1]))
                info->uprops |= UPROPS_CONTINUATION;
        } else if (is_zwj(info)) {
            info->uprops |= UPROPS_CONTINUATION;
            if (i + 1 < b->len && vega_extended_pictographic(b->info[i + 1].codepoint)) {
                ++i;
                init_unicode_props(&b->info[i], &b->scratch);
                b->info[i].uprops |= UPROPS_CONTINUATION;
            }
        } else if ((u >= 0xFF9E && u <= 0xFF9F) || (u >= 0xE0020 && u <= 0xE007F))
            info->uprops |= UPROPS_CONTINUATION;
    }
}

static size_t grapheme_end(const VegaShaper *b, size_t start) {
    size_t end = start + 1;
    while (end < b->len && is_continuation(&b->info[end])) end++;
    return end;
}

static void form_clusters(VegaShaper *b) {
    if (!(b->scratch & HAS_NON_ASCII)) return;
    for (size_t start = 0, end; start < b->len; start = end) {
        end = grapheme_end(b, start);
        merge_clusters(b, start, end);
    }
}

/* ---- The plan */

typedef struct { uint32_t first, count; } Stage;
typedef struct {
    uint32_t script;
    int direction, native, shaper, flags;
    uint32_t global_mask, kern_mask, masks[VEGA_PLAN_MASKS];
    const unsigned char *gsub, *gpos;
    uint32_t gsub_count, gpos_count;
    unsigned shaper_flags;
    Stage stages[VEGA_PLAN_STAGES];
} Plan;

static bool find_plan(const VegaShaping *s, uint32_t script, int direction, Plan *plan) {
    const unsigned char *found = NULL;
    for (int pass = 0; pass < 2 && !found; ++pass)
        for (uint32_t i = 0; i < s->plan_count && !found; ++i) {
            const unsigned char *p = s->plans + (size_t)i * VEGA_PLAN_BYTES;
            if (le32(p) == (pass ? 0x5A797979u : script) && p[4] == direction) found = p;
        }
    if (!found) return false;
    *plan = (Plan){le32(found), found[4], found[5], found[6], found[7], le32(found + 8), le32(found + 12), {0},
                   s->steps + (size_t)le32(found + 80) * VEGA_STEP_BYTES,
                   s->steps + (size_t)le32(found + 88) * VEGA_STEP_BYTES, le32(found + 84), le32(found + 92), found[96],
                   {{0, 0}}};
    for (int k = 0; k < VEGA_PLAN_MASKS; ++k) plan->masks[k] = le32(found + 16 + 4 * k);
    for (int k = 0; k < VEGA_PLAN_STAGES; ++k)
        plan->stages[k] = (Stage){le16(found + 100 + 8 * k), le16(found + 102 + 8 * k)};
    return true;
}

/* Runs of digits in right-to-left scripts are left-to-right; a run whose
 * direction is not its script's is shaped in the script's, graphemes
 * reversed. */
static void ensure_native_direction(VegaShaper *b, const Plan *plan) {
    int horizontal = plan->native;
    if (horizontal == VEGA_SHAPE_RTL && b->direction == VEGA_SHAPE_LTR) {
        bool number = false, letter = false, indicator = false;
        for (size_t i = 0; i < b->len; ++i) {
            unsigned gen = gen_cat(&b->info[i]);
            if (gen == VEGA_ND) number = true;
            else if (is_letter(gen)) {
                letter = true;
                break;
            } else if (regional_indicator(b->info[i].codepoint)) indicator = true;
        }
        if ((number || indicator) && !letter) horizontal = VEGA_SHAPE_LTR;
    }
    if (horizontal && b->direction != horizontal) {
        for (size_t start = 0, end; start < b->len; start = end) {
            end = grapheme_end(b, start);
            reverse_range(b, start, end, false);
        }
        reverse_range(b, 0, b->len, false);
        b->direction = horizontal;
    }
}

/* ---- Normalization (hb-ot-shape-normalize.cc): decompose what the font
 * lacks, order marks, compose what the font has. */

typedef struct {
    const VegaShaping *s;
    const Plan *plan;
    VegaShaper *b;
} Normalizer;

enum { SBASE = 0xAC00, LBASE = 0x1100, VBASE = 0x1161, TBASE = 0x11A7, LCOUNT = 19, VCOUNT = 21, TCOUNT = 28,
       NCOUNT = VCOUNT * TCOUNT, SCOUNT = LCOUNT * NCOUNT };

static bool decompose_unicode(uint32_t ab, uint32_t *a, uint32_t *b) {
    uint32_t si = ab - SBASE;
    if (si < SCOUNT) {
        if (si % TCOUNT) *a = SBASE + si / TCOUNT * TCOUNT, *b = TBASE + si % TCOUNT;
        else *a = LBASE + si / NCOUNT, *b = VBASE + si % NCOUNT / TCOUNT;
        return true;
    }
    return vega_decomposition_step(ab, a, b);
}

static bool compose_unicode(uint32_t a, uint32_t b, uint32_t *ab) {
    if (a >= SBASE && a < SBASE + SCOUNT && b > TBASE && b < TBASE + TCOUNT && !((a - SBASE) % TCOUNT)) {
        *ab = a + (b - TBASE);
        return true;
    }
    if (a >= LBASE && a < LBASE + LCOUNT && b >= VBASE && b < VBASE + VCOUNT) {
        *ab = SBASE + (a - LBASE) * NCOUNT + (b - VBASE) * TCOUNT;
        return true;
    }
    return (*ab = vega_composition(a, b)) != 0;
}

/* Hebrew's presentation forms, for fonts that position no marks. */
static bool compose_hebrew(const Normalizer *c, uint32_t a, uint32_t b, uint32_t *ab) {
    static const uint16_t dagesh[27] = {0xFB30, 0xFB31, 0xFB32, 0xFB33, 0xFB34, 0xFB35, 0xFB36, 0, 0xFB38,
                                        0xFB39, 0xFB3A, 0xFB3B, 0xFB3C, 0, 0xFB3E, 0, 0xFB40, 0xFB41,
                                        0, 0xFB43, 0xFB44, 0, 0xFB46, 0xFB47, 0xFB48, 0xFB49, 0xFB4A};
    if (compose_unicode(a, b, ab)) return true;
    if (c->plan->flags & VEGA_PLAN_GPOS_MARK) return false;
    *ab = 0;
    switch (b) {
    case 0x05B4: if (a == 0x05D9) *ab = 0xFB1D; break;
    case 0x05B7: if (a == 0x05F2) *ab = 0xFB1F; else if (a == 0x05D0) *ab = 0xFB2E; break;
    case 0x05B8: if (a == 0x05D0) *ab = 0xFB2F; break;
    case 0x05B9: if (a == 0x05D5) *ab = 0xFB4B; break;
    case 0x05BC:
        if (a >= 0x05D0 && a <= 0x05EA) *ab = dagesh[a - 0x05D0];
        else if (a == 0xFB2A) *ab = 0xFB2C;
        else if (a == 0xFB2B) *ab = 0xFB2D;
        break;
    case 0x05BF: if (a == 0x05D1) *ab = 0xFB4C; else if (a == 0x05DB) *ab = 0xFB4D; else if (a == 0x05E4) *ab = 0xFB4E; break;
    case 0x05C1: if (a == 0x05E9) *ab = 0xFB2A; else if (a == 0xFB49) *ab = 0xFB2C; break;
    case 0x05C2: if (a == 0x05E9) *ab = 0xFB2B; else if (a == 0xFB49) *ab = 0xFB2D; break;
    }
    return *ab != 0;
}

/* Indic's: no split matra composes again, and Bengali YA with nukta does,
 * though Unicode excludes it. */
static bool compose_indic(uint32_t a, uint32_t b, uint32_t *ab) {
    if (is_mark_category(vega_category(a))) return false;
    if (a == 0x09AF && b == 0x09BC) {
        *ab = 0x09DF;
        return true;
    }
    return compose_unicode(a, b, ab);
}

static bool compose(const Normalizer *c, uint32_t a, uint32_t b, uint32_t *ab) {
    switch (c->plan->shaper) {
    case VEGA_SHAPER_HEBREW: return compose_hebrew(c, a, b, ab);
    case VEGA_SHAPER_INDIC: return compose_indic(a, b, ab);
    case VEGA_SHAPER_KHMER: case VEGA_SHAPER_USE: return !is_mark_category(vega_category(a)) && compose_unicode(a, b, ab);
    default: return compose_unicode(a, b, ab);
    }
}

static bool nominal(const VegaShaping *s, uint32_t u, uint32_t *glyph) { return (*glyph = vega_shaping_glyph(s, u)) != 0; }

/* The glyph the font gives base followed by selector (cmap format 14). */
static bool variation_glyph(const VegaShaping *s, uint32_t base, uint32_t selector, uint32_t *glyph) {
    uint32_t low = 0, high = s->sequence_count;
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        const unsigned char *q = s->sequences + 12 * (size_t)middle;
        uint32_t b = le32(q), v = le32(q + 4);
        if (b < base || (b == base && v < selector)) low = middle + 1;
        else if (b > base || v > selector) high = middle;
        else {
            *glyph = le32(q + 8);
            return true;
        }
    }
    return false;
}

static void output_char(VegaShaper *b, uint32_t unichar, uint32_t glyph) {
    b->info[b->idx].glyph_index = glyph;
    size_t before = b->out_len;
    output_glyph(b, unichar);
    if (b->out_len > before) init_unicode_props(&b->out[b->out_len - 1], &b->scratch);
}

static void next_char(VegaShaper *b, uint32_t glyph) {
    b->info[b->idx].glyph_index = glyph;
    next_glyph(b);
}

/* The shaper's decomposition: Indic keeps a few letters whole. */
static bool decompose_char(const Normalizer *c, uint32_t ab, uint32_t *a, uint32_t *b) {
    if (c->plan->shaper == VEGA_SHAPER_INDIC && (ab == 0x0931 || ab == 0x09DC || ab == 0x09DD || ab == 0x0B94))
        return false;
    /* Khmer's split matras, which Unicode does not decompose */
    if (c->plan->shaper == VEGA_SHAPER_KHMER &&
        (ab == 0x17BE || ab == 0x17BF || ab == 0x17C0 || ab == 0x17C4 || ab == 0x17C5)) {
        *a = 0x17C1;
        *b = ab;
        return true;
    }
    return decompose_unicode(ab, a, b);
}

/* Returns how many characters ab became, 0 if it did not decompose. */
static unsigned decompose(const Normalizer *c, bool shortest, uint32_t ab) {
    uint32_t a, bb, a_glyph = 0, b_glyph = 0;
    if (!decompose_char(c, ab, &a, &bb) || (bb && !nominal(c->s, bb, &b_glyph))) return 0;
    bool has_a = nominal(c->s, a, &a_glyph);
    if (shortest && has_a) {
        output_char(c->b, a, a_glyph);
        if (bb) {
            output_char(c->b, bb, b_glyph);
            return 2;
        }
        return 1;
    }
    unsigned ret = decompose(c, shortest, a);
    if (ret) {
        if (bb) {
            output_char(c->b, bb, b_glyph);
            return ret + 1;
        }
        return ret;
    }
    if (has_a) {
        output_char(c->b, a, a_glyph);
        if (bb) {
            output_char(c->b, bb, b_glyph);
            return 2;
        }
        return 1;
    }
    return 0;
}

static void decompose_current_character(const Normalizer *c, bool shortest) {
    VegaShaper *b = c->b;
    uint32_t u = b->info[b->idx].codepoint, glyph = 0;
    bool has = nominal(c->s, u, &glyph);
    if (shortest && has) {
        next_char(b, glyph);
        return;
    }
    if (decompose(c, shortest, u)) {
        skip_glyph(b);
        return;
    }
    if (!shortest && has) {
        next_char(b, glyph);
        return;
    }
    if (is_unicode_space(&b->info[b->idx])) {
        uint32_t space_glyph;
        unsigned type = space_fallback(u);
        if (type != NOT_SPACE && nominal(c->s, 0x0020, &space_glyph)) {
            b->info[b->idx].uprops = (uint16_t)(type << 8 | (b->info[b->idx].uprops & 0xFF));
            next_char(b, space_glyph);
            b->scratch |= HAS_SPACE_FALLBACK;
            return;
        }
    }
    if (u == 0x2011) {
        uint32_t other;
        if (nominal(c->s, 0x2010, &other)) {
            next_char(b, other);
            return;
        }
    }
    next_char(b, 0);
}

/* A variation selector's cluster: normalization gives up on it. A
 * sequence the font maps (cmap format 14) becomes its glyph; otherwise
 * each character passes on alone, the selector hidden. */
static void handle_variation_selector_cluster(const Normalizer *c, size_t end) {
    VegaShaper *b = c->b;
    uint32_t glyph;
    while (b->idx + 1 < end && b->successful) {
        if (variation_selector(b->info[b->idx + 1].codepoint)) {
            if (variation_glyph(c->s, b->info[b->idx].codepoint, b->info[b->idx + 1].codepoint, &glyph)) {
                b->info[b->idx].glyph_index = glyph;
                uint32_t unicode = b->info[b->idx].codepoint;
                replace_glyphs(b, 2, 1, &unicode);
            } else {
                b->info[b->idx].glyph_index = nominal(c->s, b->info[b->idx].codepoint, &glyph) ? glyph : 0;
                next_glyph(b);
                b->scratch |= HAS_VARIATION_SELECTOR_FALLBACK;
                Info *vs = &b->info[b->idx];
                vs->uprops = (uint16_t)((vs->uprops & ~0x1Fu & 0xFF) | VEGA_CF | UPROPS_VS);
                vs->glyph_index = nominal(c->s, vs->codepoint, &glyph) ? glyph : 0;
                next_glyph(b);
            }
            while (b->idx < end && b->successful && variation_selector(b->info[b->idx].codepoint)) {
                b->info[b->idx].glyph_index = nominal(c->s, b->info[b->idx].codepoint, &glyph) ? glyph : 0;
                next_glyph(b);
            }
        } else {
            b->info[b->idx].glyph_index = nominal(c->s, b->info[b->idx].codepoint, &glyph) ? glyph : 0;
            next_glyph(b);
        }
    }
    if (b->idx < end) {
        b->info[b->idx].glyph_index = nominal(c->s, b->info[b->idx].codepoint, &glyph) ? glyph : 0;
        next_glyph(b);
    }
}

static void decompose_multi_char_cluster(const Normalizer *c, size_t end, bool short_circuit) {
    VegaShaper *b = c->b;
    for (size_t i = b->idx; i < end && b->successful; ++i)
        if (variation_selector(b->info[i].codepoint)) {
            handle_variation_selector_cluster(c, end);
            return;
        }
    while (b->idx < end && b->successful) decompose_current_character(c, short_circuit);
}

enum { MAX_COMBINING_MARKS = 32 };

/* Stable: a mark moves before the marks of a higher class, and the
 * clusters it crosses merge. */
static void sort_marks(VegaShaper *b, size_t start, size_t end) {
    for (size_t i = start + 1; i < end; ++i) {
        size_t j = i;
        while (j > start && info_cc(&b->info[j - 1]) > info_cc(&b->info[i])) j--;
        if (i == j) continue;
        merge_clusters(b, j, i + 1);
        Info t = b->info[i];
        memmove(&b->info[j + 1], &b->info[j], (i - j) * sizeof(Info));
        b->info[j] = t;
    }
}

/* Arabic marks that modify the letter (UTR #53) go first among their class. */
static bool modifier_combining_mark(uint32_t u) {
    switch (u) {
    case 0x0654: case 0x0655: case 0x0658: case 0x06DC: case 0x06E3: case 0x06E7: case 0x06E8: case 0x08CA:
    case 0x08CB: case 0x08CD: case 0x08CE: case 0x08CF: case 0x08D3: case 0x08F3:
        return true;
    default:
        return false;
    }
}

static void reorder_marks_arabic(VegaShaper *b, size_t start, size_t end) {
    size_t i = start;
    for (unsigned cc = 220; cc <= 230; cc += 10) {
        while (i < end && info_cc(&b->info[i]) < cc) i++;
        if (i == end) break;
        if (info_cc(&b->info[i]) > cc) continue;
        size_t j = i;
        while (j < end && info_cc(&b->info[j]) == cc && modifier_combining_mark(b->info[j].codepoint)) j++;
        if (i == j) continue;
        Info moved[MAX_COMBINING_MARKS];
        merge_clusters(b, start, j);
        memcpy(moved, &b->info[i], (j - i) * sizeof(Info));
        memmove(&b->info[start + j - i], &b->info[start], (i - start) * sizeof(Info));
        memcpy(&b->info[start], moved, (j - i) * sizeof(Info));
        /* Renumbered so the sequence stays sorted: 22 and 26 are below every
         * Arabic class. */
        size_t new_start = start + j - i;
        unsigned new_cc = cc == 220 ? 25 : 26;
        for (; start < new_start; ++start) set_info_cc(&b->info[start], new_cc);
        i = j;
    }
}

static void reorder_marks_hebrew(VegaShaper *b, size_t start, size_t end) {
    for (size_t i = start + 2; i < end; ++i) {
        unsigned c0 = info_cc(&b->info[i - 2]), c1 = info_cc(&b->info[i - 1]), c2 = info_cc(&b->info[i]);
        if ((c0 == 20 || c0 == 21) && (c1 == 22 || c1 == 23) && (c2 == 25 || c2 == 220)) {
            merge_clusters(b, i - 1, i + 1);
            Info t = b->info[i - 1];
            b->info[i - 1] = b->info[i];
            b->info[i] = t;
            break;
        }
    }
}

static void normalize(const VegaShaping *s, const Plan *plan, VegaShaper *b) {
    if (!b->len) return;
    Normalizer c = {s, plan, b};
    /* The shapers' preference is "auto" (composed diacritics); Hangul's is
     * none: what the font has stays, and nothing is composed again;
     * Indic's decomposes what decomposes even when the font has it whole. */
    const bool none = plan->shaper == VEGA_SHAPER_HANGUL;
    const bool syllabic = plan->shaper == VEGA_SHAPER_INDIC || plan->shaper == VEGA_SHAPER_KHMER ||
                          plan->shaper == VEGA_SHAPER_MYANMAR || plan->shaper == VEGA_SHAPER_USE;
    const bool always_short_circuit = none, might_short_circuit = none || !syllabic;
    bool all_simple = true;
    clear_output(b);
    size_t count = b->len;
    do {
        size_t end = b->idx + 1;
        while (end < count && !is_unicode_mark(&b->info[end])) end++;
        if (end < count) end--; /* one base left for the marks */
        if (might_short_circuit) {
            size_t done = 0;
            uint32_t glyph;
            while (b->idx + done < end && nominal(s, b->info[b->idx + done].codepoint, &glyph))
                b->info[b->idx + done++].glyph_index = glyph;
            next_glyphs(b, done);
        }
        while (b->idx < end && b->successful) decompose_current_character(&c, might_short_circuit);
        if (b->idx == count || !b->successful) break;
        all_simple = false;
        for (end = b->idx + 1; end < count; end++)
            if (!is_unicode_mark(&b->info[end])) break;
        decompose_multi_char_cluster(&c, end, always_short_circuit);
    } while (b->idx < count && b->successful);
    sync(b);

    if (!all_simple) {
        count = b->len;
        for (size_t i = 0; i < count; i++) {
            if (!info_cc(&b->info[i])) continue;
            size_t end = i + 1;
            while (end < count && info_cc(&b->info[end])) end++;
            if (end - i <= MAX_COMBINING_MARKS) {
                sort_marks(b, i, end);
                if (plan->shaper == VEGA_SHAPER_ARABIC) reorder_marks_arabic(b, i, end);
                else if (plan->shaper == VEGA_SHAPER_HEBREW) reorder_marks_hebrew(b, i, end);
            }
            i = end;
        }
    }
    if (b->scratch & HAS_CGJ)
        for (size_t i = 1; i + 1 < b->len; ++i)
            if (b->info[i].codepoint == 0x034F &&
                (!info_cc(&b->info[i + 1]) || info_cc(&b->info[i - 1]) <= info_cc(&b->info[i + 1])))
                b->info[i].uprops &= (uint16_t)~UPROPS_HIDDEN;

    if (!all_simple && b->successful && !none) {
        clear_output(b);
        count = b->len;
        size_t starter = 0;
        next_glyph(b);
        while (b->idx < count && b->successful) {
            Info *cur = &b->info[b->idx];
            uint32_t composed, glyph;
            if (is_unicode_mark(cur) &&
                (starter == b->out_len - 1 || info_cc(&b->out[b->out_len - 1]) < info_cc(cur)) &&
                compose(&c, b->out[starter].codepoint, cur->codepoint, &composed) && nominal(s, composed, &glyph)) {
                next_glyph(b);
                if (!b->successful) return;
                merge_out_clusters(b, starter, b->out_len);
                b->out_len--;
                b->out[starter].codepoint = composed;
                b->out[starter].glyph_index = glyph;
                init_unicode_props(&b->out[starter], &b->scratch);
                continue;
            }
            next_glyph(b);
            if (!info_cc(&b->out[b->out_len - 1])) starter = b->out_len - 1;
        }
        sync(b);
    }
}

/* ---- Masks: fractions, and Arabic's joining forms (hb-ot-shaper-arabic.cc) */

static void setup_masks_fraction(VegaShaper *b, const Plan *plan) {
    if (!(b->scratch & HAS_NON_ASCII) || !(plan->flags & VEGA_PLAN_FRACTIONS)) return;
    uint32_t frac = plan->masks[VEGA_MASK_FRAC], numr = plan->masks[VEGA_MASK_NUMR], dnom = plan->masks[VEGA_MASK_DNOM];
    bool forward = b->direction == VEGA_SHAPE_LTR;
    uint32_t pre = forward ? numr | frac : frac | dnom, post = forward ? frac | dnom : numr | frac;
    for (size_t i = 0; i < b->len;) {
        if (b->info[i].codepoint != 0x2044) {
            i++;
            continue;
        }
        size_t start = i, end = i + 1;
        while (start && gen_cat(&b->info[start - 1]) == VEGA_ND) start--;
        while (end < b->len && gen_cat(&b->info[end]) == VEGA_ND) end++;
        if (start == i || end == i + 1) {
            i++;
            continue;
        }
        for (size_t k = start; k < i; ++k) b->info[k].mask |= pre;
        b->info[i].mask |= frac;
        for (size_t k = i + 1; k < end; ++k) b->info[k].mask |= post;
        i = end;
    }
}

enum { ISOL, FINA, FIN2, FIN3, MEDI, MED2, INIT, NONE, STCH_FIXED, STCH_REPEATING };
enum { JT_U, JT_L, JT_R, JT_D, JG_ALAPH, JG_DALATH_RISH, JT_T = 7 };

/* HarfBuzz's joining type (hb-ot-shaper-arabic-table.hh); where its table
 * has none, T for a non-spacing or enclosing mark or a format character. */
static unsigned joining_type(const Info *info) {
    unsigned type = vega_arabic_joining(info->codepoint);
    if (type != 8) return type;
    unsigned c = gen_cat(info);
    return c == VEGA_MN || c == VEGA_ME || c == VEGA_CF ? JT_T : JT_U;
}

static void setup_masks_arabic(VegaShaper *b, const Plan *plan) {
    static const uint8_t table[7][6][3] = {
        {{NONE, NONE, 0}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 6}},
        {{NONE, NONE, 0}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 2}, {NONE, FIN2, 5}, {NONE, ISOL, 6}},
        {{NONE, NONE, 0}, {NONE, ISOL, 2}, {INIT, FINA, 1}, {INIT, FINA, 3}, {INIT, FINA, 4}, {INIT, FINA, 6}},
        {{NONE, NONE, 0}, {NONE, ISOL, 2}, {MEDI, FINA, 1}, {MEDI, FINA, 3}, {MEDI, FINA, 4}, {MEDI, FINA, 6}},
        {{NONE, NONE, 0}, {NONE, ISOL, 2}, {MED2, ISOL, 1}, {MED2, ISOL, 2}, {MED2, FIN2, 5}, {MED2, ISOL, 6}},
        {{NONE, NONE, 0}, {NONE, ISOL, 2}, {ISOL, ISOL, 1}, {ISOL, ISOL, 2}, {ISOL, FIN2, 5}, {ISOL, ISOL, 6}},
        {{NONE, NONE, 0}, {NONE, ISOL, 2}, {NONE, ISOL, 1}, {NONE, ISOL, 2}, {NONE, FIN3, 5}, {NONE, ISOL, 6}}};
    size_t prev = (size_t)-1;
    unsigned state = 0;
    for (size_t i = 0; i < b->len; ++i) {
        unsigned type = joining_type(&b->info[i]);
        if (type == JT_T) {
            b->info[i].action = NONE;
            continue;
        }
        const uint8_t *entry = table[state][type];
        if (entry[0] != NONE && prev != (size_t)-1) b->info[prev].action = entry[0];
        b->info[i].action = entry[1];
        prev = i;
        state = entry[2];
    }
    if (plan->script == TAG4('M', 'o', 'n', 'g'))
        for (size_t i = 1; i < b->len; ++i)
            if ((b->info[i].codepoint >= 0x180B && b->info[i].codepoint <= 0x180D) || b->info[i].codepoint == 0x180F)
                b->info[i].action = b->info[i - 1].action;
    for (size_t i = 0; i < b->len; ++i)
        if (b->info[i].action < NONE) b->info[i].mask |= plan->masks[VEGA_MASK_ISOL + b->info[i].action];
}

/* ---- Hangul (hb-ot-shaper-hangul.cc): a syllable the font has whole is
 * composed; one it lacks is taken apart into jamo, which the ljmo, vjmo and
 * tjmo features then shape; a tone mark goes before its syllable. */

enum { LJMO = 1, VJMO = 2, TJMO = 3 };
static bool combining_l(uint32_t u) { return u >= LBASE && u < LBASE + LCOUNT; }
static bool combining_v(uint32_t u) { return u >= VBASE && u < VBASE + VCOUNT; }
static bool combining_t(uint32_t u) { return u > TBASE && u < TBASE + TCOUNT; }
static bool jamo_l(uint32_t u) { return (u >= 0x1100 && u <= 0x115F) || (u >= 0xA960 && u <= 0xA97C); }
static bool jamo_v(uint32_t u) { return (u >= 0x1160 && u <= 0x11A7) || (u >= 0xD7B0 && u <= 0xD7C6); }
static bool jamo_t(uint32_t u) { return (u >= 0x11A8 && u <= 0x11FF) || (u >= 0xD7CB && u <= 0xD7FB); }
static bool zero_width(const VegaShaping *s, uint32_t u) {
    uint32_t glyph;
    return nominal(s, u, &glyph) && advance_of(s, glyph) == 0;
}

static void preprocess_hangul(const VegaShaping *s, VegaShaper *b) {
    clear_output(b);
    size_t start = 0, end = 0; /* the last syllable, when start < end */
    uint32_t glyph;
    while (b->idx < b->len && b->successful) {
        uint32_t u = b->info[b->idx].codepoint;
        if (u == 0x302E || u == 0x302F) {
            if (start < end && end == b->out_len) {
                /* a tone mark goes before its syllable, unless it overstrikes */
                next_glyph(b);
                if (!zero_width(s, u)) {
                    merge_out_clusters(b, start, end + 1);
                    Info tone = b->out[end];
                    memmove(&b->out[start + 1], &b->out[start], (end - start) * sizeof(Info));
                    b->out[start] = tone;
                }
            } else if (nominal(s, 0x25CC, &glyph)) {
                uint32_t chars[2] = {u, 0x25CC};
                if (zero_width(s, u)) chars[0] = 0x25CC, chars[1] = u;
                replace_glyphs(b, 1, 2, chars);
            } else
                next_glyph(b);
            start = end = b->out_len;
            continue;
        }
        start = b->out_len;
        if (jamo_l(u) && b->idx + 1 < b->len) {
            uint32_t l = u, v = b->info[b->idx + 1].codepoint;
            if (jamo_v(v)) {
                uint32_t t = 0, tindex = 0;
                if (b->idx + 2 < b->len) {
                    t = b->info[b->idx + 2].codepoint;
                    if (jamo_t(t)) tindex = t - TBASE;
                    else t = 0;
                }
                if (combining_l(l) && combining_v(v) && (!t || combining_t(t))) {
                    uint32_t syllable = SBASE + (l - LBASE) * NCOUNT + (v - VBASE) * TCOUNT + tindex;
                    if (nominal(s, syllable, &glyph)) {
                        replace_glyphs(b, t ? 3 : 2, 1, &syllable);
                        end = start + 1;
                        continue;
                    }
                }
                b->info[b->idx].action = LJMO;
                next_glyph(b);
                b->info[b->idx].action = VJMO;
                next_glyph(b);
                if (t) {
                    b->info[b->idx].action = TJMO;
                    next_glyph(b);
                    end = start + 3;
                } else
                    end = start + 2;
                merge_out_clusters(b, start, end);
                continue;
            }
        } else if (u - SBASE < SCOUNT) {
            bool has = nominal(s, u, &glyph);
            uint32_t lindex = (u - SBASE) / NCOUNT, nindex = (u - SBASE) % NCOUNT, vindex = nindex / TCOUNT,
                     tindex = nindex % TCOUNT;
            if (!tindex && b->idx + 1 < b->len && combining_t(b->info[b->idx + 1].codepoint)) {
                uint32_t whole = u + (b->info[b->idx + 1].codepoint - TBASE);
                if (nominal(s, whole, &glyph)) {
                    replace_glyphs(b, 2, 1, &whole);
                    end = start + 1;
                    continue;
                }
            }
            if (!has || (!tindex && b->idx + 1 < b->len && jamo_t(b->info[b->idx + 1].codepoint))) {
                uint32_t parts[3] = {LBASE + lindex, VBASE + vindex, TBASE + tindex};
                if (nominal(s, parts[0], &glyph) && nominal(s, parts[1], &glyph) && (!tindex || nominal(s, parts[2], &glyph))) {
                    size_t n = tindex ? 3 : 2;
                    replace_glyphs(b, 1, n, parts);
                    if (has && !tindex) { /* the T that did not combine is part of it */
                        next_glyph(b);
                        ++n;
                    }
                    end = start + n;
                    b->out[start].action = LJMO;
                    b->out[start + 1].action = VJMO;
                    if (start + 2 < end) b->out[start + 2].action = TJMO;
                    merge_out_clusters(b, start, end);
                    continue;
                }
            }
            if (has) {
                end = start + 1;
                next_glyph(b);
                continue;
            }
        }
        next_glyph(b);
    }
    sync(b);
}

/* ---- Applying lookups (hb-ot-layout-gsubgpos.hh) */

typedef struct {
    const VegaShaping *s;
    VegaShaper *b;
    int table; /* 0 GSUB, 1 GPOS */
    VegaTable t;
    uint32_t lookup_mask, lookup_props;
    bool auto_zwnj, auto_zwj, random, has_glyph_classes;
    int nesting_left;
    bool per_syllable; /* GSUB matches within the syllable it starts in */
    uint32_t random_state;
    long last_base;
    size_t last_base_until;
    VegaDigest digest; /* the buffer's glyphs, and every glyph substitution made since */
} Apply;

enum { MAX_NESTING_LEVEL = 64, MAX_CONTEXT_LENGTH = 64 };

static bool check_glyph_property(const Apply *c, const Info *info, uint32_t match_props) {
    unsigned props = info->gprops, flags = match_props & 0xFFFF;
    if (props & flags & (BASE_GLYPH | LIGATURE | MARK)) return false;
    if (props & MARK) {
        if (flags & 0x10) return mark_set_covers(c->s, match_props >> 16, info->codepoint);
        if (flags & 0xFF00) return (flags & 0xFF00) == (props & 0xFF00);
    }
    return true;
}

/* What a glyph matches against: a glyph id, a class of a ClassDef, or a
 * Coverage (by its offset from the subtable), one value per position. */
enum { MATCH_ANY, MATCH_GLYPH, MATCH_CLASS, MATCH_COVERAGE };
typedef struct {
    int kind;
    uint32_t data;   /* the ClassDef, or the subtable the coverages' offsets start from */
    uint32_t values; /* where the values are */
} Matcher;

typedef struct {
    Apply *c;
    uint32_t lookup_props, mask;
    bool ignore_zwnj, ignore_zwj, ignore_hidden;
    Matcher matcher;
    uint32_t glyph_data;
    size_t idx, end;
    uint8_t syllable; /* per syllable, from where the lookup is: only its glyphs match */
} Skippy;

enum { SKIP_NO, SKIP_YES, SKIP_MAYBE };
enum { MATCH_NO, MATCH_YES, MATCH_MAYBE };
enum { MATCH, NOT_MATCH, SKIP };

static void skippy_init(Skippy *it, Apply *c, bool context_match, size_t start) {
    VegaShaper *b = c->b;
    *it = (Skippy){c, c->lookup_props, context_match ? 0xFFFFFFFFu : c->lookup_mask,
                   c->table == 1 || (context_match && c->auto_zwnj), context_match || c->auto_zwj, c->table == 1,
                   {MATCH_ANY, 0, 0}, 0, start, b->len,
                   !c->table && c->per_syllable && start == b->idx && b->idx < b->len ? b->info[b->idx].syllable : 0};
}

static int may_skip(const Skippy *it, const Info *info) {
    if (!check_glyph_property(it->c, info, it->lookup_props)) return SKIP_YES;
    if (is_default_ignorable(info) && (it->ignore_zwnj || !is_zwnj(info)) && (it->ignore_zwj || !is_zwj(info)) &&
        (it->ignore_hidden || !(info->uprops & UPROPS_HIDDEN)))
        return SKIP_MAYBE;
    return SKIP_NO;
}

static bool matches(const Apply *c, const Matcher *m, uint32_t index, uint32_t glyph) {
    uint32_t value = get16(c->t, m->values + 2 * index);
    switch (m->kind) {
    case MATCH_GLYPH: return glyph == value;
    case MATCH_CLASS: return class_of(c->t, m->data, glyph) == value;
    case MATCH_COVERAGE: return coverage(c->t, value ? m->data + value : 0, glyph) != NOT_COVERED;
    default: return true;
    }
}

static int may_match(const Skippy *it, const Info *info) {
    if (!(info->mask & it->mask) || (it->syllable && it->syllable != info->syllable)) return MATCH_NO;
    if (it->matcher.kind != MATCH_ANY) return matches(it->c, &it->matcher, it->glyph_data, info->codepoint) ? MATCH_YES : MATCH_NO;
    return MATCH_MAYBE;
}

static int skippy_match(const Skippy *it, const Info *info) {
    int skip = may_skip(it, info);
    if (skip == SKIP_YES) return SKIP;
    int match = may_match(it, info);
    if (match == MATCH_YES || (match == MATCH_MAYBE && skip == SKIP_NO)) return MATCH;
    return skip == SKIP_NO ? NOT_MATCH : SKIP;
}

static bool skippy_next(Skippy *it, size_t *unsafe_to) {
    while (it->idx + 1 < it->end) {
        it->idx++;
        switch (skippy_match(it, &it->c->b->info[it->idx])) {
        case MATCH: it->glyph_data++; return true;
        case NOT_MATCH: if (unsafe_to) *unsafe_to = it->idx + 1; return false;
        default: continue;
        }
    }
    if (unsafe_to) *unsafe_to = it->end;
    return false;
}

static bool skippy_prev(Skippy *it) {
    const Info *outs = out_info(it->c->b);
    while (it->idx > 0) {
        it->idx--;
        switch (skippy_match(it, &outs[it->idx])) {
        case MATCH: it->glyph_data++; return true;
        case NOT_MATCH: return false;
        default: continue;
        }
    }
    return false;
}

static uint8_t lig_id(const Info *i) { return i->lig_props >> 5; }
static bool ligated_internal(const Info *i) { return i->lig_props & 0x10; }
static uint8_t lig_comp(const Info *i) { return ligated_internal(i) ? 0 : i->lig_props & 0x0F; }
static uint8_t lig_num_comps(const Info *i) { return (i->gprops & LIGATURE) && ligated_internal(i) ? i->lig_props & 0x0F : 1; }
static void set_lig_props_for_ligature(Info *i, uint8_t id, uint8_t comps) { i->lig_props = (uint8_t)(id << 5 | 0x10 | (comps & 0x0F)); }
static void set_lig_props_for_mark(Info *i, uint8_t id, uint8_t comp) { i->lig_props = (uint8_t)(id << 5 | (comp & 0x0F)); }

/* Match count - 1 more glyphs after the current one against matcher;
 * their positions go to positions[1..], the first's to positions[0]. */
static bool match_input(Apply *c, unsigned count, Matcher matcher, size_t *end_position, size_t *positions,
                        unsigned *total_component_count) {
    if (count > MAX_CONTEXT_LENGTH) return false;
    VegaShaper *b = c->b;
    Skippy it;
    skippy_init(&it, c, false, b->idx);
    it.matcher = matcher;
    unsigned first_lig_id = lig_id(&b->info[b->idx]), first_lig_comp = lig_comp(&b->info[b->idx]), total = 0;
    enum { NOT_CHECKED, MAY_NOT_SKIP, MAY_SKIP } ligbase = NOT_CHECKED;
    for (unsigned i = 1; i < count; ++i) {
        size_t unsafe_to;
        if (!skippy_next(&it, &unsafe_to)) {
            *end_position = unsafe_to;
            return false;
        }
        positions[i] = it.idx;
        const Info *this = &b->info[it.idx];
        unsigned this_lig_id = lig_id(this), this_lig_comp = lig_comp(this);
        if (first_lig_id && first_lig_comp) {
            /* Attached to a component of an earlier ligature: all must be
             * attached to the same one, unless that ligature is skipped. */
            if (first_lig_id != this_lig_id || first_lig_comp != this_lig_comp) {
                if (ligbase == NOT_CHECKED) {
                    const Info *out = out_info(b);
                    size_t j = b->have_output ? b->out_len : 0;
                    bool found = false;
                    while (j && lig_id(&out[j - 1]) == first_lig_id) {
                        if (!lig_comp(&out[j - 1])) {
                            j--;
                            found = true;
                            break;
                        }
                        j--;
                    }
                    ligbase = found && may_skip(&it, &out[j]) == SKIP_YES ? MAY_SKIP : MAY_NOT_SKIP;
                }
                if (ligbase == MAY_NOT_SKIP) return false;
            }
        } else if (this_lig_id && this_lig_comp && this_lig_id != first_lig_id)
            return false;
        total += lig_num_comps(this);
    }
    *end_position = it.idx + 1;
    if (total_component_count) *total_component_count = total + lig_num_comps(&b->info[b->idx]);
    positions[0] = b->idx;
    return true;
}

static bool match_backtrack(Apply *c, unsigned count, Matcher matcher) {
    Skippy it;
    skippy_init(&it, c, true, backtrack_len(c->b));
    it.matcher = matcher;
    for (unsigned i = 0; i < count; ++i)
        if (!skippy_prev(&it)) return false;
    return true;
}

static bool match_lookahead(Apply *c, unsigned count, Matcher matcher, size_t start) {
    Skippy it;
    skippy_init(&it, c, true, start - 1);
    it.matcher = matcher;
    for (unsigned i = 0; i < count; ++i)
        if (!skippy_next(&it, NULL)) return false;
    return true;
}

static void set_glyph_class(Apply *c, uint32_t glyph, unsigned class_guess, bool ligature, bool component) {
    digest_add_range(&c->digest, glyph, glyph);
    Info *cur = &c->b->info[c->b->idx];
    unsigned props = cur->gprops | SUBSTITUTED;
    if (ligature) props = (props | LIGATED) & ~(unsigned)MULTIPLIED;
    if (component) props |= MULTIPLIED;
    if (c->has_glyph_classes) props = (props & PRESERVE) | glyph_props(c->s, glyph);
    else if (class_guess) props = (props & PRESERVE) | class_guess;
    cur->gprops = (uint16_t)props;
}

static void apply_replace_glyph(Apply *c, uint32_t glyph) {
    set_glyph_class(c, glyph, 0, false, false);
    replace_glyph(c->b, glyph);
}

static void replace_glyph_inplace(Apply *c, uint32_t glyph) {
    set_glyph_class(c, glyph, 0, false, false);
    c->b->info[c->b->idx].codepoint = glyph;
}

static bool apply_lookup_index(Apply *c, uint32_t lookup_index);

static bool recurse(Apply *c, uint32_t lookup_index) {
    if (!c->nesting_left || c->b->max_ops-- <= 0) return false;
    c->nesting_left--;
    uint32_t saved = c->lookup_props;
    bool ret = apply_lookup_index(c, lookup_index);
    c->lookup_props = saved;
    c->nesting_left++;
    return ret;
}

/* Run a (chain) context rule's lookups at the positions matched. */
static void apply_lookup(Apply *c, unsigned count, size_t *positions, uint32_t records, unsigned record_count,
                         size_t match_end) {
    VegaShaper *b = c->b;
    size_t bl = backtrack_len(b);
    long end = (long)bl + (long)match_end - (long)b->idx, delta = (long)bl - (long)b->idx;
    for (unsigned j = 0; j < count; ++j) positions[j] = (size_t)((long)positions[j] + delta);
    for (unsigned i = 0; i < record_count && b->successful; ++i) {
        unsigned idx = get16(c->t, records + 4 * i);
        uint32_t lookup = get16(c->t, records + 4 * i + 2);
        if (idx >= count) continue;
        size_t orig_len = backtrack_len(b) + lookahead_len(b);
        if (positions[idx] >= orig_len) continue;
        if (!move_to(b, positions[idx])) break;
        if (b->max_ops <= 0) break;
        if (!recurse(c, lookup)) continue;
        size_t new_len = backtrack_len(b) + lookahead_len(b);
        long d = (long)new_len - (long)orig_len;
        if (!d) continue;
        end += d;
        if (end < (long)positions[idx]) {
            d += (long)positions[idx] - end;
            end = (long)positions[idx];
        }
        long next = idx + 1;
        if (d > 0) {
            if (d + count > MAX_CONTEXT_LENGTH) break;
        } else {
            if (d < next - (long)count) d = next - (long)count;
            next -= d;
        }
        memmove(positions + next + d, positions + next, (size_t)((long)count - next) * sizeof(*positions));
        next += d;
        count = (unsigned)((long)count + d);
        for (long j = idx + 1; j < next; ++j) positions[j] = positions[j - 1] + 1;
        for (; next < (long)count; ++next) positions[next] = (size_t)((long)positions[next] + d);
    }
    move_to(b, (size_t)end);
}

static void ligate_input(Apply *c, unsigned count, const size_t *positions, size_t match_end, uint32_t lig_glyph,
                         unsigned total_component_count) {
    VegaShaper *b = c->b;
    merge_clusters(b, b->idx, match_end);
    bool is_base_ligature = b->info[positions[0]].gprops & BASE_GLYPH;
    bool is_mark_ligature = b->info[positions[0]].gprops & MARK;
    for (unsigned i = 1; i < count; ++i)
        if (!(b->info[positions[i]].gprops & MARK)) {
            is_base_ligature = is_mark_ligature = false;
            break;
        }
    bool is_ligature = !is_base_ligature && !is_mark_ligature;
    uint8_t id = is_ligature ? allocate_lig_id(b) : 0;
    Info *cur = &b->info[b->idx];
    unsigned last_lig_id = lig_id(cur), last_num_components = lig_num_comps(cur), components_so_far = last_num_components;
    if (is_ligature) {
        set_lig_props_for_ligature(cur, id, (uint8_t)total_component_count);
        if (gen_cat(cur) == VEGA_MN) cur->uprops = (uint16_t)((cur->uprops & ~0x1Fu) | VEGA_LO);
    }
    set_glyph_class(c, lig_glyph, is_ligature ? LIGATURE : 0, true, false);
    replace_glyph(b, lig_glyph);
    for (unsigned i = 1; i < count; ++i) {
        while (b->idx < positions[i] && b->successful) {
            if (is_ligature) {
                Info *mark = &b->info[b->idx];
                unsigned this_comp = lig_comp(mark);
                if (!this_comp) this_comp = last_num_components;
                unsigned comp = components_so_far - last_num_components +
                                (this_comp < last_num_components ? this_comp : last_num_components);
                set_lig_props_for_mark(mark, id, (uint8_t)comp);
            }
            next_glyph(b);
        }
        last_lig_id = lig_id(&b->info[b->idx]);
        last_num_components = lig_num_comps(&b->info[b->idx]);
        components_so_far += last_num_components;
        b->idx++; /* the component itself */
    }
    if (!is_mark_ligature && last_lig_id)
        for (size_t i = b->idx; i < b->len; ++i) {
            Info *mark = &b->info[i];
            if (last_lig_id != lig_id(mark)) break;
            unsigned this_comp = lig_comp(mark);
            if (!this_comp) break;
            unsigned comp = components_so_far - last_num_components +
                            (this_comp < last_num_components ? this_comp : last_num_components);
            set_lig_props_for_mark(mark, id, (uint8_t)comp);
        }
}

/* Context and chain context rules, for GSUB and GPOS alike. A rule's
 * sequences are glyphs (format 1) or classes (format 2). */
static bool context_rule(Apply *c, uint32_t rule, Matcher input) {
    unsigned input_count = get16(c->t, rule), record_count = get16(c->t, rule + 2);
    input.values = rule + 4;
    size_t positions[MAX_CONTEXT_LENGTH], match_end = 0;
    if (!input_count || input_count > MAX_CONTEXT_LENGTH) return false;
    if (!match_input(c, input_count, input, &match_end, positions, NULL)) return false;
    apply_lookup(c, input_count, positions, rule + 4 + 2 * (input_count - 1), record_count, match_end);
    return true;
}

static bool chain_rule(Apply *c, uint32_t rule, Matcher back, Matcher input, Matcher ahead) {
    VegaTable t = c->t;
    unsigned back_count = get16(t, rule);
    uint32_t at = rule + 2 + 2 * back_count;
    unsigned input_count = get16(t, at);
    if (!input_count || input_count > MAX_CONTEXT_LENGTH) return false;
    uint32_t input_values = at + 2, ahead_at = input_values + 2 * (input_count - 1);
    unsigned ahead_count = get16(t, ahead_at);
    uint32_t records_at = ahead_at + 2 + 2 * ahead_count;
    unsigned record_count = get16(t, records_at);
    back.values = rule + 2;
    input.values = input_values;
    ahead.values = ahead_at + 2;
    size_t positions[MAX_CONTEXT_LENGTH], match_end = 0;
    if (!match_input(c, input_count, input, &match_end, positions, NULL) || !match_lookahead(c, ahead_count, ahead, match_end))
        return false;
    if (!match_backtrack(c, back_count, back)) return false;
    apply_lookup(c, input_count, positions, records_at + 2, record_count, match_end);
    return true;
}

static bool context_apply(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    uint32_t glyph = c->b->info[c->b->idx].codepoint, format = get16(t, at);
    if (format == 1 || format == 2) {
        uint32_t index = coverage(t, offset16(t, at, at + 2), glyph);
        if (index == NOT_COVERED) return false;
        uint32_t classes = format == 2 ? offset16(t, at, at + 4) : 0;
        uint32_t sets_at = format == 2 ? at + 6 : at + 4;
        if (format == 2) index = class_of(t, classes, glyph);
        if (index >= get16(t, sets_at)) return false;
        uint32_t set = offset16(t, at, sets_at + 2 + 2 * index);
        if (!set) return false;
        Matcher input = {format == 2 ? MATCH_CLASS : MATCH_GLYPH, classes, 0};
        for (uint32_t r = 0, n = get16(t, set); r < n; ++r)
            if (context_rule(c, offset16(t, set, set + 2 + 2 * r), input)) return true;
        return false;
    }
    if (format == 3) {
        unsigned input_count = get16(t, at + 2), record_count = get16(t, at + 4);
        if (!input_count || coverage(t, offset16(t, at, at + 6), glyph) == NOT_COVERED) return false;
        if (input_count > MAX_CONTEXT_LENGTH) return false;
        Matcher input = {MATCH_COVERAGE, at, at + 8};
        size_t positions[MAX_CONTEXT_LENGTH], match_end = 0;
        if (!match_input(c, input_count, input, &match_end, positions, NULL)) return false;
        apply_lookup(c, input_count, positions, at + 6 + 2 * input_count, record_count, match_end);
        return true;
    }
    return false;
}

static bool chain_context_apply(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    uint32_t glyph = c->b->info[c->b->idx].codepoint, format = get16(t, at);
    if (format == 1 || format == 2) {
        uint32_t index = coverage(t, offset16(t, at, at + 2), glyph);
        if (index == NOT_COVERED) return false;
        Matcher back = {MATCH_GLYPH, 0, 0}, input = back, ahead = back;
        uint32_t sets_at = at + 4;
        if (format == 2) {
            back = (Matcher){MATCH_CLASS, offset16(t, at, at + 4), 0};
            input = (Matcher){MATCH_CLASS, offset16(t, at, at + 6), 0};
            ahead = (Matcher){MATCH_CLASS, offset16(t, at, at + 8), 0};
            sets_at = at + 10;
            index = class_of(t, input.data, glyph);
        }
        if (index >= get16(t, sets_at)) return false;
        uint32_t set = offset16(t, at, sets_at + 2 + 2 * index);
        if (!set) return false;
        for (uint32_t r = 0, n = get16(t, set); r < n; ++r)
            if (chain_rule(c, offset16(t, set, set + 2 + 2 * r), back, input, ahead)) return true;
        return false;
    }
    if (format == 3) {
        unsigned back_count = get16(t, at + 2);
        uint32_t input_at = at + 4 + 2 * back_count;
        unsigned input_count = get16(t, input_at);
        if (!input_count || input_count > MAX_CONTEXT_LENGTH) return false;
        if (coverage(t, offset16(t, at, input_at + 2), glyph) == NOT_COVERED) return false;
        uint32_t ahead_at = input_at + 2 + 2 * input_count;
        unsigned ahead_count = get16(t, ahead_at);
        uint32_t records_at = ahead_at + 2 + 2 * ahead_count;
        Matcher back = {MATCH_COVERAGE, at, at + 4}, input = {MATCH_COVERAGE, at, input_at + 4},
                ahead = {MATCH_COVERAGE, at, ahead_at + 2};
        size_t positions[MAX_CONTEXT_LENGTH], match_end = 0;
        if (!match_input(c, input_count, input, &match_end, positions, NULL) ||
            !match_lookahead(c, ahead_count, ahead, match_end))
            return false;
        if (!match_backtrack(c, back_count, back)) return false;
        apply_lookup(c, input_count, positions, records_at + 2, get16(t, records_at), match_end);
        return true;
    }
    return false;
}

/* ---- GSUB */

static bool single_subst(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    uint32_t glyph = c->b->info[c->b->idx].codepoint, format = get16(t, at);
    uint32_t index = coverage(t, offset16(t, at, at + 2), glyph);
    if (index == NOT_COVERED) return false;
    if (format == 1) apply_replace_glyph(c, (glyph + get16(t, at + 4)) & 0xFFFF);
    else if (format == 2 && index < get16(t, at + 4)) apply_replace_glyph(c, get16(t, at + 6 + 2 * index));
    else return false;
    return true;
}

static bool multiple_subst(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (get16(t, at) != 1 || index == NOT_COVERED || index >= get16(t, at + 4)) return false;
    uint32_t sequence = offset16(t, at, at + 6 + 2 * index);
    unsigned count = get16(t, sequence);
    if (count == 1) {
        apply_replace_glyph(c, get16(t, sequence + 2));
        return true;
    }
    if (!count) {
        delete_glyph(b);
        return true;
    }
    unsigned klass = b->info[b->idx].gprops & LIGATURE ? BASE_GLYPH : 0;
    unsigned id = lig_id(&b->info[b->idx]);
    for (unsigned i = 0; i < count; ++i) {
        if (!id) set_lig_props_for_mark(&b->info[b->idx], 0, (uint8_t)i);
        uint32_t glyph = get16(t, sequence + 2 + 2 * i);
        set_glyph_class(c, glyph, klass, false, true);
        output_glyph(b, glyph);
    }
    skip_glyph(b);
    return true;
}

static bool alternate_subst(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (get16(t, at) != 1 || index == NOT_COVERED || index >= get16(t, at + 4)) return false;
    uint32_t set = offset16(t, at, at + 6 + 2 * index);
    unsigned count = get16(t, set);
    if (!count) return false;
    uint32_t mask = c->lookup_mask, shift = 0;
    while (shift < 31 && !(mask >> shift & 1)) shift++;
    uint32_t alt = (mask & b->info[b->idx].mask) >> shift;
    if (alt == 255 && c->random) {
        c->random_state = c->random_state * 48271u % 2147483647u;
        alt = c->random_state % count + 1;
    }
    if (alt > count || !alt) return false;
    apply_replace_glyph(c, get16(t, set + 2 + 2 * (alt - 1)));
    return true;
}

static bool ligature_subst(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (get16(t, at) != 1 || index == NOT_COVERED || index >= get16(t, at + 4)) return false;
    uint32_t set = offset16(t, at, at + 6 + 2 * index);
    for (uint32_t k = 0, n = get16(t, set); k < n; ++k) {
        uint32_t lig = offset16(t, set, set + 2 + 2 * k);
        uint32_t glyph = get16(t, lig);
        unsigned count = get16(t, lig + 2);
        if (!count) continue;
        if (count == 1) {
            apply_replace_glyph(c, glyph);
            return true;
        }
        if (count > MAX_CONTEXT_LENGTH) continue;
        size_t positions[MAX_CONTEXT_LENGTH], match_end = 0;
        unsigned total = 0;
        if (!match_input(c, count, (Matcher){MATCH_GLYPH, 0, lig + 4}, &match_end, positions, &total)) continue;
        ligate_input(c, count, positions, match_end, glyph, total);
        return true;
    }
    return false;
}

static bool reverse_chain_single_subst(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (get16(t, at) != 1 || index == NOT_COVERED || c->nesting_left != MAX_NESTING_LEVEL) return false;
    unsigned back_count = get16(t, at + 4);
    uint32_t ahead_at = at + 6 + 2 * back_count;
    unsigned ahead_count = get16(t, ahead_at);
    uint32_t subst_at = ahead_at + 2 + 2 * ahead_count;
    if (index >= get16(t, subst_at)) return false;
    Matcher back = {MATCH_COVERAGE, at, at + 6}, ahead = {MATCH_COVERAGE, at, ahead_at + 2};
    if (match_backtrack(c, back_count, back) && match_lookahead(c, ahead_count, ahead, b->idx + 1)) {
        replace_glyph_inplace(c, get16(t, subst_at + 2 + 2 * index));
        return true;
    }
    return false;
}

/* ---- GPOS */

typedef struct { int32_t x_placement, y_placement, x_advance; } Value;

static unsigned value_length(unsigned format) { return (unsigned)__builtin_popcount(format); }

/* Apply a ValueRecord to a position; whether it moved anything. Device
 * tables apply only at a size in pixels, and positions here are in the
 * font's units: they are passed over. */
static bool apply_value(Apply *c, unsigned format, uint32_t at, Position *pos) {
    bool worked = false;
    int32_t v;
    if (format & 1) {
        pos->x_offset += v = gets16(c->t, at), at += 2;
        worked |= v != 0;
    }
    if (format & 2) {
        pos->y_offset += v = gets16(c->t, at), at += 2;
        worked |= v != 0;
    }
    if (format & 4) {
        pos->x_advance += v = gets16(c->t, at), at += 2;
        worked |= v != 0;
    }
    return worked;
}

static bool single_pos(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (index == NOT_COVERED) return false;
    unsigned format = get16(t, at + 4);
    if (get16(t, at) == 1) apply_value(c, format, at + 6, &b->pos[b->idx]);
    else if (get16(t, at) == 2 && index < get16(t, at + 6))
        apply_value(c, format, at + 8 + 2 * value_length(format) * index, &b->pos[b->idx]);
    else return false;
    b->idx++;
    return true;
}

static bool pair_pos(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (index == NOT_COVERED) return false;
    Skippy it;
    skippy_init(&it, c, false, b->idx);
    if (!skippy_next(&it, NULL)) return false;
    size_t pos = it.idx;
    unsigned format1 = get16(t, at + 4), format2 = get16(t, at + 6), len1 = value_length(format1),
             len2 = value_length(format2);
    uint32_t values;
    if (get16(t, at) == 1) {
        if (index >= get16(t, at + 8)) return false;
        uint32_t set = offset16(t, at, at + 10 + 2 * index);
        unsigned count = get16(t, set), size = 2 * (1 + len1 + len2);
        uint32_t second = b->info[pos].codepoint, low = 0, high = count, found = NOT_COVERED;
        while (low < high) {
            uint32_t middle = (low + high) / 2, g = get16(t, set + 2 + size * middle);
            if (g < second) low = middle + 1;
            else if (g > second) high = middle;
            else {
                found = middle;
                break;
            }
        }
        if (found == NOT_COVERED) return false;
        values = set + 2 + size * found + 2;
    } else if (get16(t, at) == 2) {
        /* Class 0 of the second glyph applies too (its values often 0): the
         * subtables after it are not tried. */
        uint32_t klass2 = class_of(t, offset16(t, at, at + 10), b->info[pos].codepoint);
        uint32_t klass1 = class_of(t, offset16(t, at, at + 8), b->info[b->idx].codepoint);
        unsigned class1_count = get16(t, at + 12), class2_count = get16(t, at + 14);
        if (klass1 >= class1_count || klass2 >= class2_count) return false;
        values = at + 16 + 2 * (len1 + len2) * (klass1 * class2_count + klass2);
    } else
        return false;
    if (len1) apply_value(c, format1, values, &b->pos[b->idx]);
    if (len2) apply_value(c, format2, values + 2 * len1, &b->pos[pos]);
    if (len2) pos++;
    b->idx = pos;
    return true;
}

static bool anchor(Apply *c, uint32_t at, int32_t *x, int32_t *y) {
    if (!at) return false;
    unsigned format = get16(c->t, at);
    if (format < 1 || format > 3) {
        *x = *y = 0;
        return true;
    }
    *x = gets16(c->t, at + 2);
    *y = gets16(c->t, at + 4);
    return true;
}

enum { ATTACH_MARK = 1, ATTACH_CURSIVE = 2 };

static void reverse_cursive_minor_offset(Position *pos, size_t i, int direction, size_t new_parent) {
    int32_t chain = pos[i].chain;
    uint8_t type = pos[i].attach;
    if (!chain || !(type & ATTACH_CURSIVE)) return;
    pos[i].chain = 0;
    size_t j = (size_t)((long)i + chain);
    if (j == new_parent) return;
    reverse_cursive_minor_offset(pos, j, direction, new_parent);
    pos[j].y_offset = -pos[i].y_offset;
    pos[j].chain = -chain;
    pos[j].attach = type;
    (void)direction;
}

static bool cursive_pos(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t cov = offset16(t, at, at + 2), count = get16(t, at + 4);
    uint32_t this_index = coverage(t, cov, b->info[b->idx].codepoint);
    uint32_t entry_this = this_index < count ? offset16(t, at, at + 6 + 4 * this_index) : 0;
    if (!entry_this || get16(t, at) != 1) return false;
    Skippy it;
    skippy_init(&it, c, false, b->idx);
    if (!skippy_prev(&it)) return false;
    size_t i = it.idx, j = b->idx;
    uint32_t prev_index = coverage(t, cov, b->info[i].codepoint);
    uint32_t exit_prev = prev_index < count ? offset16(t, at, at + 6 + 4 * prev_index + 2) : 0;
    if (!exit_prev) return false;
    int32_t entry_x, entry_y, exit_x, exit_y;
    anchor(c, exit_prev, &exit_x, &exit_y);
    anchor(c, entry_this, &entry_x, &entry_y);
    Position *pos = b->pos;
    int32_t d;
    if (b->direction == VEGA_SHAPE_LTR) {
        pos[i].x_advance = exit_x + pos[i].x_offset;
        d = entry_x + pos[j].x_offset;
        pos[j].x_advance -= d;
        pos[j].x_offset -= d;
    } else {
        d = exit_x + pos[i].x_offset;
        pos[i].x_advance -= d;
        pos[i].x_offset -= d;
        pos[j].x_advance = entry_x + pos[j].x_offset;
    }
    size_t child = i, parent = j;
    int32_t y_offset = entry_y - exit_y;
    if (!(c->lookup_props & 1)) {
        child = j, parent = i;
        y_offset = -y_offset;
    }
    reverse_cursive_minor_offset(pos, child, b->direction, parent);
    pos[child].attach = ATTACH_CURSIVE;
    pos[child].chain = (int32_t)((long)parent - (long)child);
    b->scratch |= HAS_GPOS_ATTACHMENT;
    pos[child].y_offset = y_offset;
    if (pos[parent].chain == -pos[child].chain) {
        pos[parent].chain = 0;
        pos[parent].y_offset = 0;
    }
    b->idx++;
    return true;
}

/* A mark's record: its class and anchor; the anchor it attaches to is in
 * the matrix at (row, class). */
static bool mark_array_apply(Apply *c, uint32_t mark_array, uint32_t mark_index, uint32_t matrix, uint32_t row,
                             unsigned class_count, size_t glyph_pos) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    /* Past the array, HarfBuzz reads an empty record: class 0, no anchor. */
    bool inside = mark_index < get16(t, mark_array);
    uint32_t record = mark_array + 2 + 4 * mark_index;
    unsigned mark_class = inside ? get16(t, record) : 0;
    uint32_t mark_anchor = inside ? offset16(t, mark_array, record + 2) : 0;
    if (row >= get16(t, matrix) || mark_class >= class_count) return false;
    uint32_t glyph_anchor = offset16(t, matrix, matrix + 2 + 2 * (row * class_count + mark_class));
    if (!glyph_anchor) return false;
    int32_t mark_x = 0, mark_y = 0, base_x, base_y;
    anchor(c, mark_anchor, &mark_x, &mark_y);
    anchor(c, glyph_anchor, &base_x, &base_y);
    Position *o = &b->pos[b->idx];
    o->x_offset = base_x - mark_x;
    o->y_offset = base_y - mark_y;
    o->attach = ATTACH_MARK;
    o->chain = (int32_t)((long)glyph_pos - (long)b->idx);
    b->scratch |= HAS_GPOS_ATTACHMENT;
    b->idx++;
    return true;
}

static bool multiplied(const Info *i) { return i->gprops & MULTIPLIED; }

/* Marks attach to the first glyph of what a multiple substitution made. */
static bool accept_base(const VegaShaper *b, size_t idx) {
    const Info *info = b->info;
    return !multiplied(&info[idx]) || !lig_comp(&info[idx]) || idx == 0 || (info[idx - 1].gprops & MARK) ||
           !multiplied(&info[idx - 1]) || lig_id(&info[idx]) != lig_id(&info[idx - 1]) ||
           lig_comp(&info[idx]) != lig_comp(&info[idx - 1]) + 1;
}

/* The nearest glyph before that is not a mark (cached along the lookup). */
static bool find_base(Apply *c, uint32_t base_coverage, bool mark_base) {
    VegaShaper *b = c->b;
    Skippy it;
    skippy_init(&it, c, false, b->idx);
    it.lookup_props = 0x08; /* IgnoreMarks */
    if (c->last_base_until > b->idx) {
        c->last_base_until = 0;
        c->last_base = -1;
    }
    for (size_t j = b->idx; j > c->last_base_until; --j) {
        int match = skippy_match(&it, &b->info[j - 1]);
        if (match == MATCH && mark_base && !accept_base(b, j - 1) &&
            coverage(c->t, base_coverage, b->info[j - 1].codepoint) == NOT_COVERED)
            match = SKIP;
        if (match == MATCH) {
            c->last_base = (long)j - 1;
            break;
        }
    }
    c->last_base_until = b->idx;
    return c->last_base != -1;
}

static bool mark_base_pos(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t mark_index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (get16(t, at) != 1 || mark_index == NOT_COVERED) return false;
    uint32_t base_coverage = offset16(t, at, at + 4);
    if (!find_base(c, base_coverage, true)) return false;
    size_t idx = (size_t)c->last_base;
    uint32_t base_index = coverage(t, base_coverage, b->info[idx].codepoint);
    if (base_index == NOT_COVERED) return false;
    return mark_array_apply(c, offset16(t, at, at + 8), mark_index, offset16(t, at, at + 10), base_index,
                            get16(t, at + 6), idx);
}

static bool mark_lig_pos(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t mark_index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (get16(t, at) != 1 || mark_index == NOT_COVERED) return false;
    if (!find_base(c, 0, false)) return false;
    size_t idx = (size_t)c->last_base;
    uint32_t lig_index = coverage(t, offset16(t, at, at + 4), b->info[idx].codepoint);
    if (lig_index == NOT_COVERED) return false;
    uint32_t lig_array = offset16(t, at, at + 10);
    if (lig_index >= get16(t, lig_array)) return false;
    uint32_t attach = offset16(t, lig_array, lig_array + 2 + 2 * lig_index);
    unsigned comp_count = get16(t, attach);
    if (!comp_count) return false;
    unsigned id = lig_id(&b->info[idx]), mark_id = lig_id(&b->info[b->idx]), mark_comp = lig_comp(&b->info[b->idx]);
    unsigned comp_index = id && id == mark_id && mark_comp > 0 ? (mark_comp < comp_count ? mark_comp : comp_count) - 1
                                                               : comp_count - 1;
    return mark_array_apply(c, offset16(t, at, at + 8), mark_index, attach, comp_index, get16(t, at + 6), idx);
}

static bool mark_mark_pos(Apply *c, uint32_t at) {
    VegaTable t = c->t;
    VegaShaper *b = c->b;
    uint32_t mark1_index = coverage(t, offset16(t, at, at + 2), b->info[b->idx].codepoint);
    if (get16(t, at) != 1 || mark1_index == NOT_COVERED) return false;
    Skippy it;
    skippy_init(&it, c, false, b->idx);
    it.lookup_props = c->lookup_props & ~0x0Eu;
    if (!skippy_prev(&it)) return false;
    size_t j = it.idx;
    if (!(b->info[j].gprops & MARK)) return false;
    unsigned id1 = lig_id(&b->info[b->idx]), id2 = lig_id(&b->info[j]), comp1 = lig_comp(&b->info[b->idx]),
             comp2 = lig_comp(&b->info[j]);
    bool good = id1 == id2 ? !id1 || comp1 == comp2 : (id1 && !comp1) || (id2 && !comp2);
    if (!good) return false;
    uint32_t mark2_index = coverage(t, offset16(t, at, at + 4), b->info[j].codepoint);
    if (mark2_index == NOT_COVERED) return false;
    return mark_array_apply(c, offset16(t, at, at + 8), mark1_index, offset16(t, at, at + 10), mark2_index,
                            get16(t, at + 6), j);
}

/* ---- Lookups */

static bool apply_subtable(Apply *c, const VegaSubtable *st) {
    if (!st->at) return false;
    if (!c->table) switch (st->type) {
        case 1: return single_subst(c, st->at);
        case 2: return multiple_subst(c, st->at);
        case 3: return alternate_subst(c, st->at);
        case 4: return ligature_subst(c, st->at);
        case 5: return context_apply(c, st->at);
        case 6: return chain_context_apply(c, st->at);
        case 8: return reverse_chain_single_subst(c, st->at);
        default: return false;
        }
    switch (st->type) {
    case 1: return single_pos(c, st->at);
    case 2: return pair_pos(c, st->at);
    case 3: return cursive_pos(c, st->at);
    case 4: return mark_base_pos(c, st->at);
    case 5: return mark_lig_pos(c, st->at);
    case 6: return mark_mark_pos(c, st->at);
    case 7: return context_apply(c, st->at);
    case 8: return chain_context_apply(c, st->at);
    default: return false;
    }
}

static bool apply_subtables(Apply *c, const VegaLookup *l) {
    uint32_t glyph = c->b->info[c->b->idx].codepoint;
    for (uint32_t k = 0; k < l->count; ++k) {
        const VegaSubtable *st = &c->s->subtables[l->first + k];
        if (digest_may_have(&st->digest, glyph) && apply_subtable(c, st)) return true;
    }
    return false;
}

static bool apply_lookup_index(Apply *c, uint32_t lookup_index) {
    if (lookup_index >= c->s->lookup_count[c->table]) return false;
    const VegaLookup *l = &c->s->lookups[c->table][lookup_index];
    c->lookup_props = l->props;
    return c->b->idx < c->b->len && digest_may_have(&l->digest, c->b->info[c->b->idx].codepoint) && apply_subtables(c, l);
}

static void apply_string(Apply *c, const VegaLookup *l) {
    VegaShaper *b = c->b;
    if (!b->len || !c->lookup_mask) return;
    c->lookup_props = l->props;
    if (!l->reverse) {
        if (!c->table) clear_output(b);
        b->idx = 0;
        while (b->idx < b->len && b->successful) {
            const Info *cur = &b->info[b->idx];
            if (!(digest_may_have(&l->digest, cur->codepoint) && (cur->mask & c->lookup_mask) &&
                  check_glyph_property(c, cur, c->lookup_props) && apply_subtables(c, l)))
                next_glyph(b);
        }
        if (!c->table) sync(b);
    } else {
        b->idx = b->len - 1;
        for (;;) {
            const Info *cur = &b->info[b->idx];
            if (digest_may_have(&l->digest, cur->codepoint) && (cur->mask & c->lookup_mask) &&
                check_glyph_property(c, cur, c->lookup_props))
                apply_subtables(c, l);
            if (!b->idx) break;
            b->idx--;
        }
    }
}

/* The glyphs of the buffer, as a digest. */
static VegaDigest buffer_digest(const VegaShaper *b) {
    VegaDigest d = {{0, 0, 0}};
    for (size_t i = 0; i < b->len; ++i) digest_add_range(&d, b->info[i].codepoint, b->info[i].codepoint);
    return d;
}

static bool shaper_pause(const VegaShaping *s, const Plan *plan, VegaShaper *b, unsigned pause);

/* hb_ot_map_t::apply: the plan's lookups for a table in order; at a pause,
 * the shaper's part (when it changed the buffer, the digest starts over). */
static void apply_table(const VegaShaping *s, const Plan *plan, VegaShaper *b, int table) {
    const unsigned char *steps = table ? plan->gpos : plan->gsub;
    uint32_t count = table ? plan->gpos_count : plan->gsub_count;
    /* A lookup can apply only where the buffer has a glyph it starts from. */
    Apply c = {.s = s, .b = b, .table = table, .t = table ? s->gpos : s->gsub, .has_glyph_classes = s->glyph_classes != 0,
               .nesting_left = MAX_NESTING_LEVEL, .random_state = 1, .last_base = -1, .digest = buffer_digest(b)};
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *step = steps + (size_t)i * VEGA_STEP_BYTES;
        uint32_t index = le16(step);
        if (index == VEGA_STEP_PAUSE) {
            if (shaper_pause(s, plan, b, step[2])) c.digest = buffer_digest(b);
            continue;
        }
        if (!digest_may_intersect(&s->lookups[table][index].digest, &c.digest)) continue;
        c.lookup_mask = le32(step + 4);
        c.last_base = -1;
        c.last_base_until = 0;
        c.auto_zwnj = step[2] & VEGA_STEP_AUTO_ZWNJ;
        c.auto_zwj = step[2] & VEGA_STEP_AUTO_ZWJ;
        c.random = step[2] & VEGA_STEP_RANDOM;
        c.per_syllable = step[2] & VEGA_STEP_PER_SYLLABLE;
        apply_string(&c, &s->lookups[table][index]);
    }
}

/* ---- The kern table (hb-ot-kern-table.hh, hb-kern.hh): OpenType's (version
 * 0) and Apple's (1.0), their pair subtables (format 0), each a pass over
 * the glyphs with marks skipped, the kerning split between the pair. (The
 * build tool refuses the other formats, and cross-stream kerning, which no
 * font installed here has.) */

static uint32_t get8(VegaTable t, uint32_t at) { return at < t.size ? t.p[at] : 0; }

static int32_t kern_pair(VegaTable t, uint32_t pairs, uint32_t left, uint32_t right) {
    /* hb_bsearch's probes, so a table out of order answers as it does */
    int low = 0, high = (int)get16(t, pairs) - 1;
    uint32_t key = left << 16 | right;
    while (low <= high) {
        int middle = (int)(((unsigned)low + (unsigned)high) / 2);
        uint32_t at = pairs + 8 + 6 * (uint32_t)middle, pair = get16(t, at) << 16 | get16(t, at + 2);
        if (key < pair) high = middle - 1;
        else if (key > pair) low = middle + 1;
        else return gets16(t, at + 4);
    }
    return 0;
}

static void kern_table(const VegaShaping *s, VegaShaper *b, const Plan *plan) {
    VegaTable t = s->kern;
    uint32_t major = get16(t, 0);
    if (major > 1 || !plan->kern_mask) return;
    bool apple = major == 1;
    uint32_t count = apple ? get32(t, 4) : get16(t, 2), at = apple ? 8 : 4;
    for (uint32_t k = 0; k < count && at < t.size; ++k) {
        uint32_t length = apple ? get32(t, at) : get16(t, at + 2);
        unsigned format = get8(t, at + (apple ? 5 : 4)), coverage = get8(t, at + (apple ? 4 : 5));
        bool horizontal = apple ? !(coverage & 0x80) : coverage & 1;
        uint32_t pairs = at + (apple ? 8 : 6);
        at += length ? length : t.size;
        if ((apple && (coverage & 0x20)) || !horizontal || format != 0) continue;
        bool reverse = b->direction == VEGA_SHAPE_RTL;
        if (reverse) reverse_range(b, 0, b->len, true);
        Apply c = {.s = s, .b = b, .table = 1, .t = s->gpos, .lookup_mask = plan->kern_mask, .lookup_props = 0x08,
                   .auto_zwnj = true, .auto_zwj = true, .has_glyph_classes = s->glyph_classes != 0,
                   .nesting_left = MAX_NESTING_LEVEL, .random_state = 1, .last_base = -1};
        for (size_t idx = 0; idx < b->len;) {
            if (!(b->info[idx].mask & plan->kern_mask)) {
                idx++;
                continue;
            }
            Skippy it;
            skippy_init(&it, &c, false, idx);
            if (!skippy_next(&it, NULL)) {
                idx++;
                continue;
            }
            size_t i = idx, j = it.idx;
            int32_t kern = kern_pair(t, pairs, b->info[i].codepoint, b->info[j].codepoint);
            if (kern) {
                int32_t first = kern >> 1, second = kern - first;
                b->pos[i].x_advance += first;
                b->pos[j].x_advance += second;
                b->pos[j].x_offset += second;
            }
            idx = j;
        }
        if (reverse) reverse_range(b, 0, b->len, true);
    }
}

/* ---- Marks without GPOS (hb-ot-shape-fallback.cc): each mark placed around
 * its base by its combining class and the glyphs' ink boxes. */

typedef struct { int32_t x_bearing, y_bearing, width, height; } Extents;

static bool extents_of(const VegaShaping *s, uint32_t glyph, Extents *e) {
    if (!s->extents || glyph >= s->glyph_count) return false;
    const unsigned char *p = s->extents + 8 * (size_t)glyph;
    int16_t x = (int16_t)le16(p);
    if (x == INT16_MIN) return false;
    *e = (Extents){x, (int16_t)le16(p + 2), (int16_t)le16(p + 4), (int16_t)le16(p + 6)};
    return true;
}

enum { CC_ATTACHED_BELOW_LEFT = 200, CC_ATTACHED_BELOW = 202, CC_ATTACHED_ABOVE = 214, CC_ATTACHED_ABOVE_RIGHT = 216,
       CC_BELOW_LEFT = 218, CC_BELOW = 220, CC_BELOW_RIGHT = 222, CC_ABOVE_LEFT = 228, CC_ABOVE = 230,
       CC_ABOVE_RIGHT = 232, CC_DOUBLE_BELOW = 233, CC_DOUBLE_ABOVE = 234 };

/* Where the classes put marks, for those HarfBuzz reorders by other classes. */
static unsigned recategorize(uint32_t u, unsigned klass) {
    if (klass >= 200) return klass;
    if ((u & ~0xFFu) == 0x0E00) {
        if (!klass) {
            switch (u) {
            case 0x0E31: case 0x0E34: case 0x0E35: case 0x0E36: case 0x0E37: case 0x0E47: case 0x0E4C: case 0x0E4D:
            case 0x0E4E: klass = CC_ABOVE_RIGHT; break;
            case 0x0EB1: case 0x0EB4: case 0x0EB5: case 0x0EB6: case 0x0EB7: case 0x0EBB: case 0x0ECC: case 0x0ECD:
                klass = CC_ABOVE; break;
            case 0x0EBC: klass = CC_BELOW; break;
            }
        } else if (u == 0x0E3A)
            klass = CC_BELOW_RIGHT;
    }
    switch (klass) {
    case 22: case 15: case 16: case 17: case 23: case 18: case 19: case 20: case 21: case 24: case 25:
        return CC_BELOW;                           /* Hebrew points below, meteg */
    case 13: return CC_ATTACHED_ABOVE;             /* rafe */
    case 10: return CC_ABOVE_RIGHT;                /* shin dot */
    case 11: case 14: return CC_ABOVE_LEFT;        /* sin dot, holam */
    case 26: return CC_ABOVE;                      /* point varika */
    case 12: return klass;                         /* dagesh */
    case 28: case 29: case 31: case 32: case 27: case 34: case 35: case 36:
        return CC_ABOVE;                           /* Arabic and Syriac above */
    case 30: case 33: return CC_BELOW;             /* kasratan, kasra */
    case 3: return CC_BELOW_RIGHT;                 /* Thai sara u, uu */
    case 107: return CC_ABOVE_RIGHT;               /* Thai mai */
    case 118: return CC_BELOW;                     /* Lao sign u, uu */
    case 122: return CC_ABOVE;                     /* Lao mai */
    case 129: return CC_BELOW;                     /* Tibetan sign aa */
    case 132: return CC_ABOVE;                     /* sign i */
    case 131: return CC_BELOW;                     /* sign u */
    default: return klass;
    }
}

static void position_mark(const VegaShaping *s, int direction, uint32_t glyph, Position *pos, Extents *base,
                          unsigned klass) {
    Extents mark;
    if (!extents_of(s, glyph, &mark)) return;
    int32_t y_gap = s->upem / 16;
    pos->x_offset = pos->y_offset = 0;
    switch (klass) {
    case CC_DOUBLE_BELOW:
    case CC_DOUBLE_ABOVE:
        if (direction == VEGA_SHAPE_LTR) pos->x_offset += base->x_bearing + base->width - mark.width / 2 - mark.x_bearing;
        else pos->x_offset += base->x_bearing - mark.width / 2 - mark.x_bearing;
        break;
    case CC_ATTACHED_BELOW_LEFT: case CC_BELOW_LEFT: case CC_ABOVE_LEFT:
        pos->x_offset += base->x_bearing - mark.x_bearing;
        break;
    case CC_ATTACHED_ABOVE_RIGHT: case CC_BELOW_RIGHT: case CC_ABOVE_RIGHT:
        pos->x_offset += base->x_bearing + base->width - mark.width - mark.x_bearing;
        break;
    default:
        pos->x_offset += base->x_bearing + (base->width - mark.width) / 2 - mark.x_bearing;
        break;
    }
    switch (klass) {
    case CC_DOUBLE_BELOW: case CC_BELOW_LEFT: case CC_BELOW: case CC_BELOW_RIGHT:
        base->height -= y_gap;
        /* fall through */
    case CC_ATTACHED_BELOW_LEFT: case CC_ATTACHED_BELOW:
        pos->y_offset = base->y_bearing + base->height - mark.y_bearing;
        if ((y_gap > 0) == (pos->y_offset > 0)) { /* below marks never go up */
            base->height -= pos->y_offset;
            pos->y_offset = 0;
        }
        base->height += mark.height;
        break;
    case CC_DOUBLE_ABOVE: case CC_ABOVE_LEFT: case CC_ABOVE: case CC_ABOVE_RIGHT:
        base->y_bearing += y_gap;
        base->height -= y_gap;
        /* fall through */
    case CC_ATTACHED_ABOVE: case CC_ATTACHED_ABOVE_RIGHT: {
        pos->y_offset = base->y_bearing - (mark.y_bearing + mark.height);
        if ((y_gap > 0) != (pos->y_offset > 0)) { /* nor above marks too far down */
            int32_t correction = -pos->y_offset / 2;
            base->y_bearing += correction;
            base->height -= correction;
            pos->y_offset += correction;
        }
        base->y_bearing -= mark.height;
        base->height += mark.height;
        break;
    }
    default: break;
    }
}

static void zero_mark_advances(VegaShaper *b, size_t start, size_t end, bool adjust) {
    for (size_t i = start; i < end; ++i)
        if (gen_cat(&b->info[i]) == VEGA_MN) {
            if (adjust) {
                b->pos[i].x_offset -= b->pos[i].x_advance;
                b->pos[i].y_offset -= b->pos[i].y_advance;
            }
            b->pos[i].x_advance = b->pos[i].y_advance = 0;
        }
}

static void position_around_base(const VegaShaping *s, const Plan *plan, VegaShaper *b, size_t base, size_t end,
                                 bool adjust) {
    Extents base_extents;
    if (!extents_of(s, b->info[base].codepoint, &base_extents)) {
        zero_mark_advances(b, base + 1, end, adjust);
        return;
    }
    base_extents.y_bearing += b->pos[base].y_offset;
    base_extents.x_bearing = 0;
    base_extents.width = advance_of(s, b->info[base].codepoint);
    unsigned id = lig_id(&b->info[base]);
    int components = lig_num_comps(&b->info[base]);
    int32_t x_offset = 0, y_offset = 0;
    bool forward = b->direction == VEGA_SHAPE_LTR;
    if (forward) {
        x_offset -= b->pos[base].x_advance;
        y_offset -= b->pos[base].y_advance;
    }
    Extents component = base_extents, cluster = base_extents;
    int last_component = -1;
    unsigned last_class = 255;
    for (size_t i = base + 1; i < end; ++i) {
        unsigned klass = info_cc(&b->info[i]);
        if (klass) {
            if (components > 1) {
                unsigned this_id = lig_id(&b->info[i]);
                int this_component = lig_comp(&b->info[i]) - 1;
                if (!id || id != this_id || this_component >= components) this_component = components - 1;
                if (last_component != this_component) {
                    last_component = this_component;
                    last_class = 255;
                    component = base_extents;
                    if (plan->direction == VEGA_SHAPE_LTR)
                        component.x_bearing += this_component * component.width / components;
                    else
                        component.x_bearing += (components - 1 - this_component) * component.width / components;
                    component.width /= components;
                }
            }
            if (last_class != klass) {
                last_class = klass;
                cluster = component;
            }
            position_mark(s, b->direction, b->info[i].codepoint, &b->pos[i], &cluster, klass);
            b->pos[i].x_advance = b->pos[i].y_advance = 0;
            b->pos[i].x_offset += x_offset;
            b->pos[i].y_offset += y_offset;
        } else if (forward) {
            x_offset -= b->pos[i].x_advance;
            y_offset -= b->pos[i].y_advance;
        } else {
            x_offset += b->pos[i].x_advance;
            y_offset += b->pos[i].y_advance;
        }
    }
}

static void position_cluster(const VegaShaping *s, const Plan *plan, VegaShaper *b, size_t start, size_t end, bool adjust) {
    if (end - start < 2) return;
    for (size_t i = start; i < end; ++i)
        if (!is_unicode_mark(&b->info[i])) {
            size_t j = i + 1;
            while (j < end && is_unicode_mark(&b->info[j])) j++;
            position_around_base(s, plan, b, i, j, adjust);
            i = j - 1;
        }
}

static void fallback_marks(const VegaShaping *s, const Plan *plan, VegaShaper *b, bool adjust) {
    size_t start = 0;
    for (size_t i = 1; i < b->len; ++i)
        if (!is_unicode_mark(&b->info[i])) {
            position_cluster(s, plan, b, start, i, adjust);
            start = i;
        }
    position_cluster(s, plan, b, start, b->len, adjust);
}

/* ---- Would a lookup substitute glyphs as they are, alone
 * (hb_ot_layout_lookup_would_substitute): the Indic shaper's question of
 * its basic features. */

static bool would_match_input(const Apply *c, Matcher m, const uint32_t *glyphs, unsigned len, unsigned count) {
    if (count != len) return false;
    for (unsigned i = 1; i < count; ++i)
        if (!matches(c, &m, i - 1, glyphs[i])) return false;
    return true;
}

static bool would_apply_rules(const Apply *c, int type, uint32_t set, Matcher input, const uint32_t *glyphs,
                              unsigned len, bool zero_context) {
    VegaTable t = c->t;
    for (uint32_t r = 0, n = set ? get16(t, set) : 0; r < n; ++r) {
        uint32_t rule = offset16(t, set, set + 2 + 2 * r);
        if (type == 5) {
            input.values = rule + 4;
            if (would_match_input(c, input, glyphs, len, get16(t, rule))) return true;
            continue;
        }
        unsigned back_count = get16(t, rule);
        uint32_t input_at = rule + 2 + 2 * back_count;
        unsigned input_count = get16(t, input_at);
        unsigned ahead_count = input_count ? get16(t, input_at + 2 + 2 * (input_count - 1)) : 0;
        if (zero_context && (back_count || ahead_count)) continue;
        input.values = input_at + 2;
        if (would_match_input(c, input, glyphs, len, input_count)) return true;
    }
    return false;
}

static bool would_apply_subtable(const Apply *c, const VegaSubtable *st, const uint32_t *glyphs, unsigned len,
                                 bool zero_context) {
    VegaTable t = c->t;
    uint32_t at = st->at, format = get16(t, at);
    switch (st->type) {
    case 1: case 2: case 3: case 8:
        if (format != 1 && !(st->type == 1 && format == 2)) return false;
        return len == 1 && coverage(t, offset16(t, at, at + 2), glyphs[0]) != NOT_COVERED;
    case 4: {
        uint32_t index = coverage(t, offset16(t, at, at + 2), glyphs[0]);
        if (format != 1 || index == NOT_COVERED || index >= get16(t, at + 4)) return false;
        uint32_t set = offset16(t, at, at + 6 + 2 * index);
        for (uint32_t k = 0, n = set ? get16(t, set) : 0; k < n; ++k) {
            uint32_t lig = offset16(t, set, set + 2 + 2 * k);
            if (would_match_input(c, (Matcher){MATCH_GLYPH, 0, lig + 4}, glyphs, len, get16(t, lig + 2))) return true;
        }
        return false;
    }
    case 5: case 6: {
        bool chain = st->type == 6;
        if (format == 1 || format == 2) {
            /* format 2 takes the first glyph's class whatever the coverage */
            uint32_t classes = format == 2 ? offset16(t, at, at + (chain ? 6 : 4)) : 0;
            uint32_t index = format == 1 ? coverage(t, offset16(t, at, at + 2), glyphs[0]) : class_of(t, classes, glyphs[0]);
            uint32_t sets_at = format == 1 ? at + 4 : at + (chain ? 10 : 6);
            if (index == NOT_COVERED || index >= get16(t, sets_at)) return false;
            Matcher input = {format == 2 ? MATCH_CLASS : MATCH_GLYPH, classes, 0};
            return would_apply_rules(c, st->type, offset16(t, at, sets_at + 2 + 2 * index), input, glyphs, len,
                                     zero_context);
        }
        if (format != 3) return false;
        if (!chain) return would_match_input(c, (Matcher){MATCH_COVERAGE, at, at + 8}, glyphs, len, get16(t, at + 2));
        unsigned back_count = get16(t, at + 2);
        uint32_t input_at = at + 4 + 2 * back_count;
        unsigned input_count = get16(t, input_at);
        unsigned ahead_count = get16(t, input_at + 2 + 2 * input_count);
        if (zero_context && (back_count || ahead_count)) return false;
        return would_match_input(c, (Matcher){MATCH_COVERAGE, at, input_at + 4}, glyphs, len, input_count);
    }
    default: return false;
    }
}

/* Whether the lookups of a feature's stage (all of them, whatever their
 * masks) would substitute glyphs, len of them. */
static bool would_substitute(const VegaShaping *s, const Plan *plan, int stage, const uint32_t *glyphs, unsigned len,
                             bool zero_context) {
    const Stage *st = &plan->stages[stage];
    Apply c = {.s = s, .t = s->gsub};
    for (uint32_t k = 0; k < st->count; ++k) {
        const unsigned char *step = plan->gsub + (size_t)(st->first + k) * VEGA_STEP_BYTES;
        uint32_t index = le16(step);
        if (index == VEGA_STEP_PAUSE) continue;
        const VegaLookup *l = &s->lookups[0][index];
        if (!len || !digest_may_have(&l->digest, glyphs[0])) continue;
        for (uint32_t i = 0; i < l->count; ++i)
            if (would_apply_subtable(&c, &s->subtables[l->first + i], glyphs, len, zero_context)) return true;
    }
    return false;
}

/* ---- Syllables (hb-ot-shaper-*-machine.rl, hb-ot-shaper-syllabic.cc) */

/* A shaper's machine over the buffer's categories: from each position the
 * longest syllable it accepts; each numbered, 1 to 15 over again, beside
 * its type. */
static void find_syllables(VegaShaper *b, const VegaMachine *m) {
    unsigned serial = 1;
    for (size_t p = 0; p < b->len;) {
        unsigned state = 0, type = 0;
        size_t end = p + 1;
        for (size_t q = p; q < b->len; ++q) {
            size_t cell = (size_t)state * m->classes + m->class_of[b->info[q].category < 63 ? b->info[q].category : 63];
            state = m->width == 1 ? ((const uint8_t *)m->next)[cell] : ((const uint16_t *)m->next)[cell];
            if (state >= m->states) break;
            if (m->accept[state]) type = m->accept[state], end = q + 1;
        }
        if (type == m->broken) b->scratch |= HAS_BROKEN_SYLLABLE;
        for (size_t i = p; i < end; ++i) b->info[i].syllable = (uint8_t)(serial << 4 | (type ? type - 1 : 0));
        if (++serial == 16) serial = 1;
        p = end;
    }
}

static size_t next_syllable(const VegaShaper *b, size_t start) {
    size_t end = start + 1;
    while (end < b->len && b->info[end].syllable == b->info[start].syllable) end++;
    return end;
}

/* A dotted circle at the start of each broken syllable (after a repha that
 * starts it), for its marks to sit on; false when none went in. */
static bool insert_dotted_circles(const VegaShaping *s, VegaShaper *b, unsigned broken, unsigned category, int repha,
                                  int position) {
    uint32_t glyph;
    if (!(b->scratch & HAS_BROKEN_SYLLABLE) || !nominal(s, 0x25CC, &glyph)) return false;
    /* All of it zero but these: HarfBuzz's is a fresh glyph_info. */
    Info circle = {.codepoint = glyph, .uprops = VEGA_CC, .position = (uint8_t)(position >= 0 ? position : 0),
                   .category = (uint8_t)category};
    clear_output(b);
    unsigned last = 0;
    while (b->idx < b->len && b->successful) {
        unsigned syllable = b->info[b->idx].syllable;
        if (last != syllable && (syllable & 0x0F) == broken) {
            last = syllable;
            Info c = circle;
            c.cluster = b->info[b->idx].cluster;
            c.mask = b->info[b->idx].mask;
            c.syllable = (uint8_t)syllable;
            if (repha >= 0)
                while (b->idx < b->len && b->successful && b->info[b->idx].syllable == last &&
                       b->info[b->idx].category == (unsigned)repha)
                    next_glyph(b);
            output_info(b, c);
        } else
            next_glyph(b);
    }
    sync(b);
    return true;
}

/* Vowel sequences that read as another vowel get a dotted circle between
 * (hb-ot-shaper-vowel-constraints.cc, from the Universal Shaping Engine's
 * IndicShapingInvalidCluster.txt): each first letter with what may not
 * follow it. */
static const struct { uint32_t script, first, second[12]; } vowel_constraints[] = {
    {TAG4('D', 'e', 'v', 'a'), 0x0905, {0x093A, 0x093B, 0x093E, 0x0945, 0x0946, 0x0949, 0x094A, 0x094B, 0x094C, 0x094F, 0x0956, 0x0957}},
    {TAG4('D', 'e', 'v', 'a'), 0x0906, {0x093A, 0x0945, 0x0946, 0x0947, 0x0948}},
    {TAG4('D', 'e', 'v', 'a'), 0x0909, {0x0941}},
    {TAG4('D', 'e', 'v', 'a'), 0x090F, {0x0945, 0x0946, 0x0947}},
    {TAG4('B', 'e', 'n', 'g'), 0x0985, {0x09BE}},
    {TAG4('B', 'e', 'n', 'g'), 0x098B, {0x09C3}},
    {TAG4('B', 'e', 'n', 'g'), 0x098C, {0x09E2}},
    {TAG4('G', 'u', 'r', 'u'), 0x0A05, {0x0A3E, 0x0A48, 0x0A4C}},
    {TAG4('G', 'u', 'r', 'u'), 0x0A72, {0x0A3F, 0x0A40, 0x0A47}},
    {TAG4('G', 'u', 'r', 'u'), 0x0A73, {0x0A41, 0x0A42, 0x0A4B}},
    {TAG4('G', 'u', 'j', 'r'), 0x0A85, {0x0ABE, 0x0AC5, 0x0AC7, 0x0AC8, 0x0AC9, 0x0ACB, 0x0ACC}},
    {TAG4('G', 'u', 'j', 'r'), 0x0AC5, {0x0ABE}},
    {TAG4('O', 'r', 'y', 'a'), 0x0B05, {0x0B3E}},
    {TAG4('O', 'r', 'y', 'a'), 0x0B0F, {0x0B57}},
    {TAG4('O', 'r', 'y', 'a'), 0x0B13, {0x0B57}},
    {TAG4('T', 'a', 'm', 'l'), 0x0B85, {0x0BC2}},
    {TAG4('T', 'e', 'l', 'u'), 0x0C12, {0x0C4C, 0x0C55}},
    {TAG4('T', 'e', 'l', 'u'), 0x0C3F, {0x0C55}},
    {TAG4('T', 'e', 'l', 'u'), 0x0C46, {0x0C55}},
    {TAG4('T', 'e', 'l', 'u'), 0x0C4A, {0x0C55}},
    {TAG4('K', 'n', 'd', 'a'), 0x0C89, {0x0CBE}},
    {TAG4('K', 'n', 'd', 'a'), 0x0C8B, {0x0CBE}},
    {TAG4('K', 'n', 'd', 'a'), 0x0C92, {0x0CCC}},
    {TAG4('M', 'l', 'y', 'm'), 0x0D07, {0x0D57}},
    {TAG4('M', 'l', 'y', 'm'), 0x0D09, {0x0D57}},
    {TAG4('M', 'l', 'y', 'm'), 0x0D0E, {0x0D46}},
    {TAG4('M', 'l', 'y', 'm'), 0x0D12, {0x0D3E, 0x0D57}},
    {TAG4('S', 'i', 'n', 'h'), 0x0D85, {0x0DCF, 0x0DD0, 0x0DD1}},
    {TAG4('S', 'i', 'n', 'h'), 0x0D8B, {0x0DDF}},
    {TAG4('S', 'i', 'n', 'h'), 0x0D8D, {0x0DD8}},
    {TAG4('S', 'i', 'n', 'h'), 0x0D8F, {0x0DDF}},
    {TAG4('S', 'i', 'n', 'h'), 0x0D91, {0x0DCA, 0x0DD9, 0x0DDA, 0x0DDC, 0x0DDD, 0x0DDE}},
    {TAG4('S', 'i', 'n', 'h'), 0x0D94, {0x0DDF}},
    {TAG4('B', 'r', 'a', 'h'), 0x11005, {0x11038}},
    {TAG4('B', 'r', 'a', 'h'), 0x1100B, {0x1103E}},
    {TAG4('B', 'r', 'a', 'h'), 0x1100F, {0x11042}},
    {TAG4('K', 'h', 'o', 'j'), 0x11200, {0x1122C, 0x11231, 0x11233}},
    {TAG4('K', 'h', 'o', 'j'), 0x11206, {0x1122C}},
    {TAG4('K', 'h', 'o', 'j'), 0x1122C, {0x11230, 0x11231}},
    {TAG4('K', 'h', 'o', 'j'), 0x11240, {0x1122E}},
    {TAG4('S', 'i', 'n', 'd'), 0x112B0, {0x112E0, 0x112E5, 0x112E6, 0x112E7, 0x112E8}},
    {TAG4('T', 'i', 'r', 'h'), 0x11481, {0x114B0}},
    {TAG4('T', 'i', 'r', 'h'), 0x1148B, {0x114BA}},
    {TAG4('T', 'i', 'r', 'h'), 0x1148D, {0x114BA}},
    {TAG4('T', 'i', 'r', 'h'), 0x114AA, {0x114B5, 0x114B6}},
    {TAG4('M', 'o', 'd', 'i'), 0x11600, {0x11639, 0x1163A}},
    {TAG4('M', 'o', 'd', 'i'), 0x11601, {0x11639, 0x1163A}},
    {TAG4('T', 'a', 'k', 'r'), 0x11680, {0x116AD, 0x116B4, 0x116B5}},
    {TAG4('T', 'a', 'k', 'r'), 0x11686, {0x116B2}},
};

/* A dotted circle out before the current character, as a copy of it (its
 * properties, less continuing the character before). */
static void output_dotted_circle(VegaShaper *b) {
    output_glyph(b, 0x25CC);
    if (b->out_len) b->out[b->out_len - 1].uprops &= (uint16_t)~UPROPS_CONTINUATION;
}

static void preprocess_vowel_constraints(const Plan *plan, VegaShaper *b) {
    bool any = false;
    for (size_t k = 0; k < sizeof(vowel_constraints) / sizeof(*vowel_constraints) && !any; ++k)
        any = vowel_constraints[k].script == plan->script;
    if (!any && plan->script != TAG4('D', 'e', 'v', 'a')) return;
    clear_output(b);
    while (b->idx + 1 < b->len && b->successful) {
        uint32_t u = b->info[b->idx].codepoint, next = b->info[b->idx + 1].codepoint;
        bool matched = false;
        for (size_t k = 0; k < sizeof(vowel_constraints) / sizeof(*vowel_constraints) && !matched; ++k)
            if (vowel_constraints[k].script == plan->script && vowel_constraints[k].first == u)
                for (int j = 0; j < 12 && vowel_constraints[k].second[j] && !matched; ++j)
                    matched = vowel_constraints[k].second[j] == next;
        /* Devanagari's RA, virama, I: the circle goes before the I */
        if (plan->script == TAG4('D', 'e', 'v', 'a') && u == 0x0930 && next == 0x094D && b->idx + 2 < b->len &&
            b->info[b->idx + 2].codepoint == 0x0907) {
            next_glyph(b);
            next_glyph(b);
            output_dotted_circle(b);
        }
        next_glyph(b);
        if (matched) {
            output_dotted_circle(b);
            next_glyph(b);
        }
    }
    sync(b);
}

/* ---- Indic (hb-ot-shaper-indic.cc): each consonant syllable's base found,
 * its pre-base matras, reph and the like moved around the base and masked
 * for the basic features; after those, the matras, reph and
 * pre-base-reordering consonants placed again by what the features made. */

enum { REPH_IMPLICIT, REPH_EXPLICIT, REPH_LOG_REPHA };
typedef struct { uint32_t script, virama; uint8_t reph_pos, reph_mode; bool blwf_post_only; } IndicConfig;
static const IndicConfig indic_configs[] = {
    {0, 0, VEGA_POS_BEFORE_POST, REPH_IMPLICIT, false},
    {TAG4('D', 'e', 'v', 'a'), 0x094D, VEGA_POS_BEFORE_POST, REPH_IMPLICIT, false},
    {TAG4('B', 'e', 'n', 'g'), 0x09CD, VEGA_POS_AFTER_SUB, REPH_IMPLICIT, false},
    {TAG4('G', 'u', 'r', 'u'), 0x0A4D, VEGA_POS_BEFORE_SUB, REPH_IMPLICIT, false},
    {TAG4('G', 'u', 'j', 'r'), 0x0ACD, VEGA_POS_BEFORE_POST, REPH_IMPLICIT, false},
    {TAG4('O', 'r', 'y', 'a'), 0x0B4D, VEGA_POS_AFTER_MAIN, REPH_IMPLICIT, false},
    {TAG4('T', 'a', 'm', 'l'), 0x0BCD, VEGA_POS_AFTER_POST, REPH_IMPLICIT, false},
    {TAG4('T', 'e', 'l', 'u'), 0x0C4D, VEGA_POS_AFTER_POST, REPH_EXPLICIT, true},
    {TAG4('K', 'n', 'd', 'a'), 0x0CCD, VEGA_POS_AFTER_POST, REPH_IMPLICIT, true},
    {TAG4('M', 'l', 'y', 'm'), 0x0D4D, VEGA_POS_AFTER_MAIN, REPH_LOG_REPHA, false},
};

typedef struct {
    const VegaShaping *s;
    const Plan *plan;
    const IndicConfig *config;
    bool old_spec, zero_context;
    uint32_t virama; /* its glyph; 0 when the font has none */
} Indic;

static Indic indic_of(const VegaShaping *s, const Plan *plan) {
    Indic d = {s, plan, &indic_configs[0], plan->shaper_flags & VEGA_INDIC_OLD_SPEC, false, 0};
    for (size_t i = 1; i < sizeof(indic_configs) / sizeof(*indic_configs); ++i)
        if (indic_configs[i].script == plan->script) d.config = &indic_configs[i];
    /* New specifications match the basic features without context; so do
     * scripts with one, but Malayalam's both take context. */
    d.zero_context = !d.old_spec && plan->script != TAG4('M', 'l', 'y', 'm');
    if (d.config->virama) nominal(s, d.config->virama, &d.virama);
    return d;
}

#define FLAG(x) (1u << (x))
#define FLAG_UNSAFE(x) ((x) < 32 ? 1u << (x) : 0u)
static bool is_one_of(const Info *i, uint32_t flags) {
    return !(i->gprops & LIGATED) && (FLAG_UNSAFE(i->category) & flags); /* a ligature is none of them */
}
static bool is_consonant(const Info *i) {
    return is_one_of(i, FLAG(VEGA_OT_C) | FLAG(VEGA_OT_CS) | FLAG(VEGA_OT_RA) | FLAG(VEGA_OT_CM) | FLAG(VEGA_OT_V) |
                            FLAG(VEGA_OT_PLACEHOLDER) | FLAG(VEGA_OT_DOTTEDCIRCLE));
}
static bool is_joiner(const Info *i) { return is_one_of(i, FLAG(VEGA_OT_ZWJ) | FLAG(VEGA_OT_ZWNJ)); }
static bool is_halant(const Info *i) { return is_one_of(i, FLAG(VEGA_OT_H)); }
static bool ligated_and_didnt_multiply(const Info *i) { return (i->gprops & LIGATED) && !(i->gprops & MULTIPLIED); }

static bool indic_would(const Indic *d, int stage, const uint32_t *glyphs, unsigned len) {
    return would_substitute(d->s, d->plan, stage, glyphs, len, d->zero_context);
}

/* Below-base, post-base or base: what the font's features would make of
 * the consonant with a virama, either way round (fonts copied old lookups
 * into new scripts). */
static uint8_t consonant_position(const Indic *d, uint32_t consonant) {
    uint32_t glyphs[3] = {d->virama, consonant, d->virama};
    if (indic_would(d, VEGA_STAGE_BLWF, glyphs, 2) || indic_would(d, VEGA_STAGE_BLWF, glyphs + 1, 2) ||
        indic_would(d, VEGA_STAGE_VATU, glyphs, 2) || indic_would(d, VEGA_STAGE_VATU, glyphs + 1, 2))
        return VEGA_POS_BELOW_C;
    if (indic_would(d, VEGA_STAGE_PSTF, glyphs, 2) || indic_would(d, VEGA_STAGE_PSTF, glyphs + 1, 2) ||
        indic_would(d, VEGA_STAGE_PREF, glyphs, 2) || indic_would(d, VEGA_STAGE_PREF, glyphs + 1, 2))
        return VEGA_POS_POST_C;
    return VEGA_POS_BASE_C;
}

static void setup_masks_indic(VegaShaper *b) {
    for (size_t i = 0; i < b->len; ++i) {
        uint16_t categories = vega_indic_categories(b->info[i].codepoint);
        b->info[i].category = categories & 0xFF;
        b->info[i].position = categories >> 8;
    }
}

static void indic_reorder_consonant_syllable(const Indic *d, VegaShaper *b, size_t start, size_t end) {
    Info *info = b->info;
    const uint32_t *masks = d->plan->masks;
    uint32_t script = d->plan->script;
    /* Kannada's Ra, halant, ZWJ is Ra, ZWJ, halant. */
    if (script == TAG4('K', 'n', 'd', 'a') && start + 3 <= end && is_one_of(&info[start], FLAG(VEGA_OT_RA)) &&
        is_one_of(&info[start + 1], FLAG(VEGA_OT_H)) && is_one_of(&info[start + 2], FLAG(VEGA_OT_ZWJ))) {
        merge_clusters(b, start + 1, start + 3);
        Info t = info[start + 1];
        info[start + 1] = info[start + 2];
        info[start + 2] = t;
    }

    /* 1. The base: from the end back, the first consonant without a
     * below-base or post-base form (those follow below-base ones), or the
     * first consonant. An initial Ra, halant that the font makes a reph
     * is no candidate. */
    size_t base = end, limit = start;
    bool has_reph = false;
    if (masks[VEGA_MASK_RPHF] && start + 3 <= end &&
        ((d->config->reph_mode == REPH_IMPLICIT && !is_joiner(&info[start + 2])) ||
         (d->config->reph_mode == REPH_EXPLICIT && info[start + 2].category == VEGA_OT_ZWJ))) {
        uint32_t glyphs[3] = {info[start].codepoint, info[start + 1].codepoint,
                              d->config->reph_mode == REPH_EXPLICIT ? info[start + 2].codepoint : 0};
        if (indic_would(d, VEGA_STAGE_RPHF, glyphs, 2) ||
            (d->config->reph_mode == REPH_EXPLICIT && indic_would(d, VEGA_STAGE_RPHF, glyphs, 3))) {
            limit += 2;
            while (limit < end && is_joiner(&info[limit])) limit++;
            base = start;
            has_reph = true;
        }
    } else if (d->config->reph_mode == REPH_LOG_REPHA && info[start].category == VEGA_OT_REPHA) {
        limit += 1;
        while (limit < end && is_joiner(&info[limit])) limit++;
        base = start;
        has_reph = true;
    }
    {
        size_t i = end;
        bool seen_below = false;
        do {
            i--;
            if (is_consonant(&info[i])) {
                if (info[i].position != VEGA_POS_BELOW_C && (info[i].position != VEGA_POS_POST_C || seen_below)) {
                    base = i;
                    break;
                }
                if (info[i].position == VEGA_POS_BELOW_C) seen_below = true;
                base = i;
            } else if (start < i && info[i].category == VEGA_OT_ZWJ && info[i - 1].category == VEGA_OT_H)
                break; /* a ZWJ after a halant asks for a half form: the search stops */
        } while (i > limit);
    }
    if (has_reph && base == start && limit - base <= 2) has_reph = false; /* Ra alone is the base */

    /* 2 and 3 (matras decomposed, marks in canonical order) normalization
     * did. Reorder: */
    for (size_t i = start; i < base; ++i)
        if (info[i].position > VEGA_POS_PRE_C) info[i].position = VEGA_POS_PRE_C;
    if (base < end) info[base].position = VEGA_POS_BASE_C;
    if (has_reph) info[start].position = VEGA_POS_RA_TO_BECOME_REPH;

    /* Old specifications: the first post-base halant after the last
     * consonant (Kannada not past a halant already there). */
    if (d->old_spec) {
        bool disallow_double_halants = script == TAG4('K', 'n', 'd', 'a');
        for (size_t i = base + 1; i < end; ++i)
            if (info[i].category == VEGA_OT_H) {
                size_t j;
                for (j = end - 1; j > i; j--)
                    if (is_consonant(&info[j]) || (disallow_double_halants && info[j].category == VEGA_OT_H)) break;
                if (info[j].category != VEGA_OT_H && j > i) {
                    Info t = info[i];
                    memmove(&info[i], &info[i + 1], (j - i) * sizeof(Info));
                    info[j] = t;
                }
                break;
            }
    }

    /* Joiners, nuktas, register shifters, medials and halants go with what
     * is before them (a halant not with a left matra). */
    {
        uint8_t last_pos = VEGA_POS_START;
        for (size_t i = start; i < end; ++i) {
            if (FLAG_UNSAFE(info[i].category) & (FLAG(VEGA_OT_ZWJ) | FLAG(VEGA_OT_ZWNJ) | FLAG(VEGA_OT_N) |
                                                 FLAG(VEGA_OT_RS) | FLAG(VEGA_OT_CM) | FLAG(VEGA_OT_H))) {
                info[i].position = last_pos;
                if (info[i].category == VEGA_OT_H && info[i].position == VEGA_POS_PRE_M)
                    for (size_t j = i; j > start; j--)
                        if (info[j - 1].position != VEGA_POS_PRE_M) {
                            info[i].position = info[j - 1].position;
                            break;
                        }
            } else if (info[i].position != VEGA_POS_SMVD) {
                if (info[i].category == VEGA_OT_MPST && i > start && info[i - 1].category == VEGA_OT_SM)
                    info[i - 1].position = info[i].position;
                last_pos = info[i].position;
            }
        }
    }
    /* A post-base consonant owns what is before it since the last
     * consonant or matra. */
    {
        size_t last = base;
        for (size_t i = base + 1; i < end; ++i)
            if (is_consonant(&info[i])) {
                for (size_t j = last + 1; j < i; ++j)
                    if (info[j].position < VEGA_POS_SMVD) info[j].position = info[i].position;
                last = i;
            } else if (FLAG_UNSAFE(info[i].category) & (FLAG(VEGA_OT_M) | FLAG(VEGA_OT_MPST)))
                last = i;
    }

    {
        /* Sorted by position, stably; syllable holds each glyph's place
         * before, to merge the clusters of what moved past base. */
        uint8_t syllable = info[start].syllable;
        for (size_t i = start; i < end; ++i) info[i].syllable = (uint8_t)(i - start);
        for (size_t i = start + 1; i < end; ++i) {
            size_t j = i;
            while (j > start && info[j - 1].position > info[i].position) j--;
            if (j == i) continue;
            Info t = info[i];
            memmove(&info[j + 1], &info[j], (i - j) * sizeof(Info));
            info[j] = t;
        }
        /* The base again; a run of left matras back in their order. */
        size_t first_left_matra = end, last_left_matra = end;
        base = end;
        for (size_t i = start; i < end; ++i) {
            if (info[i].position == VEGA_POS_BASE_C) {
                base = i;
                break;
            }
            if (info[i].position == VEGA_POS_PRE_M) {
                if (first_left_matra == end) first_left_matra = i;
                last_left_matra = i;
            }
        }
        if (first_left_matra < last_left_matra) {
            reverse_range(b, first_left_matra, last_left_matra + 1, false);
            size_t i = first_left_matra;
            for (size_t j = i; j <= last_left_matra; ++j)
                if (FLAG_UNSAFE(info[j].category) & (FLAG(VEGA_OT_M) | FLAG(VEGA_OT_MPST))) {
                    reverse_range(b, i, j + 1, false);
                    i = j + 1;
                }
        }
        if (d->old_spec || end - start > 127)
            merge_clusters(b, base, end);
        else
            for (size_t i = base; i < end; ++i)
                if (info[i].syllable != 255) {
                    size_t min = i, max = i, j = start + info[i].syllable;
                    while (j != i) {
                        if (j < min) min = j;
                        if (j > max) max = j;
                        size_t next = start + info[j].syllable;
                        info[j].syllable = 255;
                        j = next;
                    }
                    merge_clusters(b, base > min ? base : min, max + 1);
                }
        for (size_t i = start; i < end; ++i) info[i].syllable = syllable;
    }

    /* Masks: reph; pre-base (half, and blwf in new specifications that
     * have it there); post-base. */
    for (size_t i = start; i < end && info[i].position == VEGA_POS_RA_TO_BECOME_REPH; ++i)
        info[i].mask |= masks[VEGA_MASK_RPHF];
    uint32_t mask = masks[VEGA_MASK_HALF];
    if (!d->old_spec && !d->config->blwf_post_only) mask |= masks[VEGA_MASK_BLWF];
    for (size_t i = start; i < base; ++i) info[i].mask |= mask;
    mask = masks[VEGA_MASK_BLWF] | masks[VEGA_MASK_ABVF] | masks[VEGA_MASK_PSTF];
    for (size_t i = base + 1; i < end; ++i) info[i].mask |= mask;

    /* Old-spec Devanagari: an eyelash Ra below a half form too, but not
     * with a ZWJ. */
    if (d->old_spec && script == TAG4('D', 'e', 'v', 'a'))
        for (size_t i = start; i + 1 < base; ++i)
            if (info[i].category == VEGA_OT_RA && info[i + 1].category == VEGA_OT_H &&
                (i + 2 == base || info[i + 2].category != VEGA_OT_ZWJ)) {
                info[i].mask |= masks[VEGA_MASK_BLWF];
                info[i + 1].mask |= masks[VEGA_MASK_BLWF];
            }

    /* A halant, Ra the font reorders before the base: marked for pref. */
    if (masks[VEGA_MASK_PREF] && base + 2 < end)
        for (size_t i = base + 1; i + 1 < end; ++i) {
            uint32_t glyphs[2] = {info[i].codepoint, info[i + 1].codepoint};
            if (indic_would(d, VEGA_STAGE_PREF, glyphs, 2)) {
                info[i].mask |= masks[VEGA_MASK_PREF];
                info[i + 1].mask |= masks[VEGA_MASK_PREF];
                break;
            }
        }

    /* A ZWNJ keeps what is before it back to a consonant from half forms. */
    for (size_t i = start + 1; i < end; ++i)
        if (is_joiner(&info[i])) {
            bool non_joiner = info[i].category == VEGA_OT_ZWNJ;
            size_t j = i;
            do {
                j--;
                if (non_joiner) info[j].mask &= ~masks[VEGA_MASK_HALF];
            } while (j > start && !is_consonant(&info[j]));
        }
}

static bool indic_initial_reordering(const Indic *d, VegaShaper *b) {
    if (d->virama)
        for (size_t i = 0; i < b->len; ++i)
            if (b->info[i].position == VEGA_POS_BASE_C) b->info[i].position = consonant_position(d, b->info[i].codepoint);
    bool changed = insert_dotted_circles(d->s, b, 4, VEGA_OT_DOTTEDCIRCLE, VEGA_OT_REPHA, VEGA_POS_END);
    for (size_t start = 0, end; start < b->len; start = end) {
        end = next_syllable(b, start);
        /* Vowel syllables look like consonant ones, and broken ones have
         * their dotted circle, like a standalone cluster's. */
        unsigned type = b->info[start].syllable & 0x0F;
        if (type <= 2 || type == 4) indic_reorder_consonant_syllable(d, b, start, end);
    }
    return changed;
}

/* Categories of the preceding character that keep a left matra from
 * 'init': HarfBuzz's Cf to Mn. */
static bool word_character(unsigned c) {
    switch (c) {
    case VEGA_CF: case VEGA_CN: case VEGA_CO: case VEGA_CS: case VEGA_LL: case VEGA_LM: case VEGA_LO: case VEGA_LT:
    case VEGA_LU: case VEGA_MC: case VEGA_ME: case VEGA_MN:
        return true;
    default:
        return false;
    }
}

static void indic_final_syllable(const Indic *d, VegaShaper *b, size_t start, size_t end) {
    Info *info = b->info;
    const uint32_t *masks = d->plan->masks;
    uint32_t script = d->plan->script;
    bool malayalam_or_tamil = script == TAG4('M', 'l', 'y', 'm') || script == TAG4('T', 'a', 'm', 'l');
    /* A virama that ligated and multiplied back out is a halant again. */
    if (d->virama)
        for (size_t i = start; i < end; ++i)
            if (info[i].codepoint == d->virama && (info[i].gprops & LIGATED) && (info[i].gprops & MULTIPLIED)) {
                info[i].category = VEGA_OT_H;
                info[i].gprops &= (uint16_t)~(LIGATED | MULTIPLIED);
            }

    /* 4. The base again: where a pref candidate formed nothing, or (in
     * Malayalam) past unformed below-base forms. */
    bool try_pref = masks[VEGA_MASK_PREF] != 0;
    size_t base;
    for (base = start; base < end; base++)
        if (info[base].position >= VEGA_POS_BASE_C) {
            if (try_pref && base + 1 < end)
                for (size_t i = base + 1; i < end; ++i)
                    if (info[i].mask & masks[VEGA_MASK_PREF]) {
                        if (!((info[i].gprops & SUBSTITUTED) && ligated_and_didnt_multiply(&info[i]))) {
                            base = i;
                            while (base < end && is_halant(&info[base])) base++;
                            if (base < end) info[base].position = VEGA_POS_BASE_C;
                            try_pref = false;
                        }
                        break;
                    }
            if (script == TAG4('M', 'l', 'y', 'm'))
                for (size_t i = base + 1; i < end; ++i) {
                    while (i < end && is_joiner(&info[i])) i++;
                    if (i == end || !is_halant(&info[i])) break;
                    i++;
                    while (i < end && is_joiner(&info[i])) i++;
                    if (i < end && is_consonant(&info[i]) && info[i].position == VEGA_POS_BELOW_C) {
                        base = i;
                        info[base].position = VEGA_POS_BASE_C;
                    }
                }
            if (start < base && info[base].position > VEGA_POS_BASE_C) base--;
            break;
        }
    if (base == end && start < base && is_one_of(&info[base - 1], FLAG(VEGA_OT_ZWJ))) base--;
    if (base < end)
        while (start < base && is_one_of(&info[base], FLAG(VEGA_OT_N) | FLAG(VEGA_OT_H))) base--;

    /* Pre-base matras: after the last halant before the base (not one a
     * ZWJ follows, nor the matra's own), else where they are. */
    if (start + 1 < end && start < base) {
        size_t new_pos = base == end ? base - 2 : base - 1;
        if (!malayalam_or_tamil) {
            for (;;) {
                while (new_pos > start &&
                       !is_one_of(&info[new_pos], FLAG(VEGA_OT_M) | FLAG(VEGA_OT_MPST) | FLAG(VEGA_OT_H)))
                    new_pos--;
                if (is_halant(&info[new_pos]) && info[new_pos].position != VEGA_POS_PRE_M) {
                    if (new_pos + 1 < end && info[new_pos + 1].category == VEGA_OT_ZWJ && new_pos > start) {
                        new_pos--;
                        continue; /* keep searching */
                    }
                } else
                    new_pos = start;
                break;
            }
        }
        if (start < new_pos && info[new_pos].position != VEGA_POS_PRE_M) {
            for (size_t i = new_pos; i > start; i--)
                if (info[i - 1].position == VEGA_POS_PRE_M) {
                    size_t old_pos = i - 1;
                    if (old_pos < base && base <= new_pos) base--;
                    Info t = info[old_pos];
                    memmove(&info[old_pos], &info[old_pos + 1], (new_pos - old_pos) * sizeof(Info));
                    info[new_pos] = t;
                    merge_clusters(b, new_pos, end < base + 1 ? end : base + 1);
                    new_pos--;
                }
        } else
            for (size_t i = start; i < base; ++i)
                if (info[i].position == VEGA_POS_PRE_M) {
                    merge_clusters(b, i, end < base + 1 ? end : base + 1);
                    break;
                }
    }

    /* Reph: moved only if it formed (or, encoded as a repha, if it did
     * not ligate), to where the script puts it. */
    if (start + 1 < end && info[start].position == VEGA_POS_RA_TO_BECOME_REPH &&
        ((info[start].category == VEGA_OT_REPHA) ^ ligated_and_didnt_multiply(&info[start]))) {
        size_t new_reph_pos = start;
        unsigned reph_pos = d->config->reph_pos;
        bool found = false;
        if (reph_pos != VEGA_POS_AFTER_POST) {
            /* 2. after the first explicit halant before the base (and a
             * joiner after it) */
            new_reph_pos = start + 1;
            while (new_reph_pos < base && !is_halant(&info[new_reph_pos])) new_reph_pos++;
            if (new_reph_pos < base && is_halant(&info[new_reph_pos])) {
                if (new_reph_pos + 1 < base && is_joiner(&info[new_reph_pos + 1])) new_reph_pos++;
                found = true;
            }
            /* 3. after the main consonant */
            if (!found && reph_pos == VEGA_POS_AFTER_MAIN) {
                new_reph_pos = base;
                while (new_reph_pos + 1 < end && info[new_reph_pos + 1].position <= VEGA_POS_AFTER_MAIN) new_reph_pos++;
                found = new_reph_pos < end;
            }
            /* 4. before the first post-base consonant or matra */
            if (!found && reph_pos == VEGA_POS_AFTER_SUB) {
                new_reph_pos = base;
                while (new_reph_pos + 1 < end &&
                       !(FLAG_UNSAFE(info[new_reph_pos + 1].position) &
                         (FLAG(VEGA_POS_POST_C) | FLAG(VEGA_POS_AFTER_POST) | FLAG(VEGA_POS_SMVD))))
                    new_reph_pos++;
                found = new_reph_pos < end;
            }
        }
        if (!found) {
            /* 5. as step 2 */
            new_reph_pos = start + 1;
            while (new_reph_pos < base && !is_halant(&info[new_reph_pos])) new_reph_pos++;
            if (new_reph_pos < base && is_halant(&info[new_reph_pos])) {
                if (new_reph_pos + 1 < base && is_joiner(&info[new_reph_pos + 1])) new_reph_pos++;
                found = true;
            }
        }
        if (!found) {
            /* 6. the end, before any syllable modifiers; before a halant
             * that follows a matra */
            new_reph_pos = end - 1;
            while (new_reph_pos > start && info[new_reph_pos].position == VEGA_POS_SMVD) new_reph_pos--;
            if (is_halant(&info[new_reph_pos]))
                for (size_t i = base + 1; i < new_reph_pos; ++i)
                    if (FLAG_UNSAFE(info[i].category) & (FLAG(VEGA_OT_M) | FLAG(VEGA_OT_MPST))) new_reph_pos--;
        }
        merge_clusters(b, start, new_reph_pos + 1);
        Info reph = info[start];
        memmove(&info[start], &info[start + 1], (new_reph_pos - start) * sizeof(Info));
        info[new_reph_pos] = reph;
        if (start < base && base <= new_reph_pos) base--;
    }

    /* A pre-base-reordering consonant that pref formed: before the base,
     * where a pre-base matra would go. */
    if (try_pref && base + 1 < end)
        for (size_t i = base + 1; i < end; ++i)
            if (info[i].mask & masks[VEGA_MASK_PREF]) {
                if (ligated_and_didnt_multiply(&info[i])) {
                    size_t new_pos = base;
                    if (!malayalam_or_tamil)
                        while (new_pos > start &&
                               !is_one_of(&info[new_pos - 1], FLAG(VEGA_OT_M) | FLAG(VEGA_OT_MPST) | FLAG(VEGA_OT_H)))
                            new_pos--;
                    if (new_pos > start && is_halant(&info[new_pos - 1]) && new_pos < end && is_joiner(&info[new_pos]))
                        new_pos++;
                    size_t old_pos = i;
                    merge_clusters(b, new_pos, old_pos + 1);
                    Info t = info[old_pos];
                    memmove(&info[new_pos + 1], &info[new_pos], (old_pos - new_pos) * sizeof(Info));
                    info[new_pos] = t;
                    if (new_pos <= base && base < old_pos) base++;
                }
                break;
            }

    /* 'init' on a left matra that starts a word. */
    if (info[start].position == VEGA_POS_PRE_M && (!start || !word_character(gen_cat(&info[start - 1]))))
        info[start].mask |= masks[VEGA_MASK_INDIC_INIT];
}

static void indic_final_reordering(const Indic *d, VegaShaper *b) {
    for (size_t start = 0, end; start < b->len; start = end) {
        end = next_syllable(b, start);
        indic_final_syllable(d, b, start, end);
    }
}

/* ---- Khmer (hb-ot-shaper-khmer.cc): a coeng and Ro moved before the base
 * and masked for pref, what follows them for cfar; a left matra moved
 * first; the post-base features on all but the first glyph. */

static void setup_masks_category(VegaShaper *b) {
    for (size_t i = 0; i < b->len; ++i) b->info[i].category = vega_indic_categories(b->info[i].codepoint) & 0xFF;
}

static void khmer_reorder_syllable(const Plan *plan, VegaShaper *b, size_t start, size_t end) {
    Info *info = b->info;
    const uint32_t *masks = plan->masks;
    uint32_t post = masks[VEGA_MASK_KHMER_BLWF] | masks[VEGA_MASK_KHMER_ABVF] | masks[VEGA_MASK_KHMER_PSTF];
    for (size_t i = start + 1; i < end; ++i) info[i].mask |= post;
    unsigned coengs = 0;
    for (size_t i = start + 1; i < end; ++i)
        if (info[i].category == VEGA_OT_H && coengs <= 2 && i + 1 < end) {
            coengs++;
            if (info[i + 1].category == VEGA_OT_RA) {
                info[i].mask |= masks[VEGA_MASK_KHMER_PREF];
                info[i + 1].mask |= masks[VEGA_MASK_KHMER_PREF];
                merge_clusters(b, start, i + 2);
                Info t0 = info[i], t1 = info[i + 1];
                memmove(&info[start + 2], &info[start], (i - start) * sizeof(Info));
                info[start] = t0;
                info[start + 1] = t1;
                if (masks[VEGA_MASK_KHMER_CFAR])
                    for (size_t j = i + 2; j < end; ++j) info[j].mask |= masks[VEGA_MASK_KHMER_CFAR];
                coengs = 2;
            }
        } else if (info[i].category == VEGA_OT_VPRE) {
            merge_clusters(b, start, i + 1);
            Info t = info[i];
            memmove(&info[start + 1], &info[start], (i - start) * sizeof(Info));
            info[start] = t;
        }
}

static bool khmer_reorder(const VegaShaping *s, const Plan *plan, VegaShaper *b) {
    bool changed = insert_dotted_circles(s, b, 1, VEGA_OT_DOTTEDCIRCLE, -1, -1);
    for (size_t start = 0, end; start < b->len; start = end) {
        end = next_syllable(b, start);
        if ((b->info[start].syllable & 0x0F) <= 1) khmer_reorder_syllable(plan, b, start, end);
    }
    return changed;
}

/* ---- Myanmar (hb-ot-shaper-myanmar.cc): the base (after a kinzi), each
 * glyph given its place around it, and the syllable sorted by place. */

#define CONSONANT_FLAGS_MYANMAR \
    (FLAG(VEGA_OT_C) | FLAG(VEGA_OT_CS) | FLAG(VEGA_OT_RA) | FLAG(VEGA_OT_V) | FLAG(VEGA_OT_PLACEHOLDER) | \
     FLAG(VEGA_OT_DOTTEDCIRCLE))

/* hb_buffer_t::sort by position: stable; a glyph that moves back merges
 * the clusters it passes. */
static void sort_by_position(VegaShaper *b, size_t start, size_t end) {
    for (size_t i = start + 1; i < end; ++i) {
        size_t j = i;
        while (j > start && b->info[j - 1].position > b->info[i].position) j--;
        if (i == j) continue;
        merge_clusters(b, j, i + 1);
        Info t = b->info[i];
        memmove(&b->info[j + 1], &b->info[j], (i - j) * sizeof(Info));
        b->info[j] = t;
    }
}

static void myanmar_reorder_syllable(VegaShaper *b, size_t start, size_t end) {
    Info *info = b->info;
    size_t base = end, limit = start;
    bool has_reph = false;
    if (start + 3 <= end && info[start].category == VEGA_OT_RA && info[start + 1].category == VEGA_OT_AS &&
        info[start + 2].category == VEGA_OT_H) {
        limit += 3;
        base = start;
        has_reph = true;
    }
    if (!has_reph) base = limit;
    for (size_t i = limit; i < end; ++i)
        if (is_one_of(&info[i], CONSONANT_FLAGS_MYANMAR)) {
            base = i;
            break;
        }
    size_t i = start;
    for (; i < start + (has_reph ? 3 : 0); ++i) info[i].position = VEGA_POS_AFTER_MAIN;
    for (; i < base; ++i) info[i].position = VEGA_POS_PRE_C;
    if (i < end) info[i++].position = VEGA_POS_BASE_C;
    uint8_t pos = VEGA_POS_AFTER_MAIN;
    for (; i < end; ++i) {
        unsigned c = info[i].category;
        if (c == VEGA_OT_MR) info[i].position = VEGA_POS_PRE_C;       /* pre-base reordering */
        else if (c == VEGA_OT_VPRE) info[i].position = VEGA_POS_PRE_M; /* left matra */
        else if (c == VEGA_OT_VS) info[i].position = info[i - 1].position;
        else if (pos == VEGA_POS_AFTER_MAIN && c == VEGA_OT_VBLW) info[i].position = pos = VEGA_POS_BELOW_C;
        else if (pos == VEGA_POS_BELOW_C && c == VEGA_OT_A) info[i].position = VEGA_POS_BEFORE_SUB;
        else if (pos == VEGA_POS_BELOW_C && c == VEGA_OT_VBLW) info[i].position = pos;
        else if (pos == VEGA_POS_BELOW_C) info[i].position = pos = VEGA_POS_AFTER_SUB;
        else info[i].position = pos;
    }
    sort_by_position(b, start, end);
    /* A run of left matras back in their order (variation selectors after
     * theirs). */
    size_t first_left_matra = end, last_left_matra = end;
    for (size_t k = start; k < end; ++k)
        if (info[k].position == VEGA_POS_PRE_M) {
            if (first_left_matra == end) first_left_matra = k;
            last_left_matra = k;
        }
    if (first_left_matra < last_left_matra) {
        reverse_range(b, first_left_matra, last_left_matra + 1, false);
        size_t k = first_left_matra;
        for (size_t j = k; j <= last_left_matra; ++j)
            if (info[j].category == VEGA_OT_VPRE) {
                reverse_range(b, k, j + 1, false);
                k = j + 1;
            }
    }
}

static bool myanmar_reorder(const VegaShaping *s, VegaShaper *b) {
    bool changed = insert_dotted_circles(s, b, 1, VEGA_OT_DOTTEDCIRCLE, -1, -1);
    for (size_t start = 0, end; start < b->len; start = end) {
        end = next_syllable(b, start);
        if ((b->info[start].syllable & 0x0F) <= 1) myanmar_reorder_syllable(b, start, end);
    }
    return changed;
}

/* ---- The Universal Shaping Engine (hb-ot-shaper-use.cc): syllables by
 * USE categories; rphf and pref, each recording what it made (a repha, a
 * pre-base glyph); then a repha moved before the first post-base glyph and
 * pre-base glyphs moved to the start (or past a halant). Scripts that join
 * like Arabic take Arabic's forms; others join syllable to syllable. */

enum { USE_VIRAMA_TERMINATED, USE_SAKOT_TERMINATED, USE_STANDARD, USE_NUMBER_JOINER_TERMINATED, USE_NUMERAL,
       USE_SYMBOL, USE_HIEROGLYPH, USE_BROKEN, USE_NON_CLUSTER };

/* The scripts with Arabic's joining data (hb-ot-shaper-arabic-joining-list.hh). */
static bool arabic_joining_script(uint32_t script) {
    switch (script) {
    case TAG4('A', 'd', 'l', 'm'): case TAG4('A', 'r', 'a', 'b'): case TAG4('C', 'h', 'r', 's'):
    case TAG4('R', 'o', 'h', 'g'): case TAG4('M', 'a', 'n', 'd'): case TAG4('M', 'a', 'n', 'i'):
    case TAG4('M', 'o', 'n', 'g'): case TAG4('N', 'k', 'o', 'o'): case TAG4('O', 'u', 'g', 'r'):
    case TAG4('P', 'h', 'a', 'g'): case TAG4('P', 'h', 'l', 'p'): case TAG4('S', 'o', 'g', 'd'):
    case TAG4('S', 'y', 'r', 'c'):
        return true;
    default:
        return false;
    }
}

static void setup_masks_use(VegaShaper *b, const Plan *plan) {
    if (arabic_joining_script(plan->script)) setup_masks_arabic(b, plan);
    for (size_t i = 0; i < b->len; ++i) b->info[i].category = vega_use_category(b->info[i].codepoint);
}

/* The machine reads the glyphs but CGJs, and a ZWNJ only when no mark
 * follows it (past CGJs); a syllable holds what it skipped after it. */
static void find_syllables_use(VegaShaper *b) {
    size_t n = 0;
    for (size_t i = 0; i < b->len; ++i) {
        if (b->info[i].category == VEGA_USE_CGJ) continue;
        if (b->info[i].category == VEGA_USE_ZWNJ) {
            size_t k = i + 1;
            while (k < b->len && b->info[k].category == VEGA_USE_CGJ) k++;
            if (k < b->len && is_unicode_mark(&b->info[k])) continue;
        }
        b->kept[n++] = (uint32_t)i;
    }
    const VegaMachine *m = &vega_use_machine;
    unsigned serial = 1;
    for (size_t p = 0; p < n;) {
        unsigned state = 0, type = 0;
        size_t end = p + 1;
        for (size_t q = p; q < n; ++q) {
            unsigned category = b->info[b->kept[q]].category;
            size_t cell = (size_t)state * m->classes + m->class_of[category < 63 ? category : 63];
            state = m->width == 1 ? ((const uint8_t *)m->next)[cell] : ((const uint16_t *)m->next)[cell];
            if (state >= m->states) break;
            if (m->accept[state]) type = m->accept[state], end = q + 1;
        }
        if (type == m->broken) b->scratch |= HAS_BROKEN_SYLLABLE;
        size_t first = b->kept[p], last = end < n ? b->kept[end] : b->len;
        for (size_t i = first; i < last; ++i) b->info[i].syllable = (uint8_t)(serial << 4 | (type ? type - 1 : 0));
        if (++serial == 16) serial = 1;
        p = end;
    }
}

static void use_setup_syllables(const Plan *plan, VegaShaper *b) {
    find_syllables_use(b);
    Info *info = b->info;
    /* rphf on what may be a repha: an encoded one, or the first three */
    uint32_t rphf = plan->masks[VEGA_MASK_USE_RPHF];
    if (rphf)
        for (size_t start = 0, end; start < b->len; start = end) {
            end = next_syllable(b, start);
            size_t limit = info[start].category == VEGA_USE_R ? 1 : end - start < 3 ? end - start : 3;
            for (size_t i = start; i < start + limit; ++i) info[i].mask |= rphf;
        }
    /* Joining forms, syllable to syllable, where Arabic's did not come. */
    if (arabic_joining_script(plan->script)) return;
    enum { FORM_ISOL, FORM_INIT, FORM_MEDI, FORM_FINA, FORM_NONE };
    uint32_t masks[4] = {plan->masks[VEGA_MASK_ISOL], plan->masks[VEGA_MASK_INIT], plan->masks[VEGA_MASK_MEDI],
                         plan->masks[VEGA_MASK_FINA]},
             all = 0;
    for (int i = 0; i < 4; ++i) {
        if (masks[i] == plan->global_mask) masks[i] = 0;
        all |= masks[i];
    }
    if (!all) return;
    uint32_t other = ~all;
    size_t last_start = 0;
    int last_form = FORM_NONE;
    for (size_t start = 0, end; start < b->len; start = end) {
        end = next_syllable(b, start);
        unsigned type = info[start].syllable & 0x0F;
        if (type == USE_HIEROGLYPH || type == USE_NON_CLUSTER)
            last_form = FORM_NONE;
        else {
            bool join = last_form == FORM_FINA || last_form == FORM_ISOL;
            if (join) {
                last_form = last_form == FORM_FINA ? FORM_MEDI : FORM_INIT;
                for (size_t i = last_start; i < start; ++i) info[i].mask = (info[i].mask & other) | masks[last_form];
            }
            last_form = join ? FORM_FINA : FORM_ISOL;
            for (size_t i = start; i < end; ++i) info[i].mask = (info[i].mask & other) | masks[last_form];
        }
        last_start = start;
    }
}

/* A repha rphf made (the first substituted glyph under its mask) is a
 * repha; a glyph pref made is a pre-base vowel. */
static void use_record(const Plan *plan, VegaShaper *b, unsigned pause) {
    uint32_t rphf = plan->masks[VEGA_MASK_USE_RPHF];
    if (pause == VEGA_PAUSE_RECORD_RPHF && !rphf) return;
    for (size_t start = 0, end; start < b->len; start = end) {
        end = next_syllable(b, start);
        for (size_t i = start; i < end && (pause != VEGA_PAUSE_RECORD_RPHF || (b->info[i].mask & rphf)); ++i)
            if (b->info[i].gprops & SUBSTITUTED) {
                b->info[i].category = pause == VEGA_PAUSE_RECORD_RPHF ? VEGA_USE_R : VEGA_USE_VPRE;
                break;
            }
    }
}

static bool is_halant_use(const Info *i) {
    return (i->category == VEGA_USE_H || i->category == VEGA_USE_HVM || i->category == VEGA_USE_IS) &&
           !(i->gprops & LIGATED);
}

#define FLAG64_UNSAFE(x) ((x) < 64 ? (uint64_t)1 << (x) : 0)
#define USE_POST_BASE                                                                                               \
    (FLAG64_UNSAFE(VEGA_USE_FABV) | FLAG64_UNSAFE(VEGA_USE_FBLW) | FLAG64_UNSAFE(VEGA_USE_FPST) |                  \
     FLAG64_UNSAFE(VEGA_USE_FMABV) | FLAG64_UNSAFE(VEGA_USE_FMBLW) | FLAG64_UNSAFE(VEGA_USE_FMPST) |               \
     FLAG64_UNSAFE(VEGA_USE_MABV) | FLAG64_UNSAFE(VEGA_USE_MBLW) | FLAG64_UNSAFE(VEGA_USE_MPST) |                  \
     FLAG64_UNSAFE(VEGA_USE_MPRE) | FLAG64_UNSAFE(VEGA_USE_VABV) | FLAG64_UNSAFE(VEGA_USE_VBLW) |                  \
     FLAG64_UNSAFE(VEGA_USE_VPST) | FLAG64_UNSAFE(VEGA_USE_VPRE) | FLAG64_UNSAFE(VEGA_USE_VMABV) |                 \
     FLAG64_UNSAFE(VEGA_USE_VMBLW) | FLAG64_UNSAFE(VEGA_USE_VMPST) | FLAG64_UNSAFE(VEGA_USE_VMPRE))

static void use_reorder_syllable(VegaShaper *b, size_t start, size_t end) {
    unsigned type = b->info[start].syllable & 0x0F;
    if (!(FLAG_UNSAFE(type) & (FLAG(USE_VIRAMA_TERMINATED) | FLAG(USE_SAKOT_TERMINATED) | FLAG(USE_STANDARD) |
                               FLAG(USE_SYMBOL) | FLAG(USE_BROKEN))))
        return;
    Info *info = b->info;
    /* A repha forward, before the first post-base glyph (or to the end). */
    if (info[start].category == VEGA_USE_R && end - start > 1)
        for (size_t i = start + 1; i < end; ++i) {
            bool post_base = (FLAG64_UNSAFE(info[i].category) & USE_POST_BASE) || is_halant_use(&info[i]);
            if (post_base || i == end - 1) {
                if (post_base) i--;
                merge_clusters(b, start, i + 1);
                Info t = info[start];
                memmove(&info[start], &info[start + 1], (i - start) * sizeof(Info));
                info[i] = t;
                break;
            }
        }
    /* Pre-base vowels back, to the start or past the last halant (only a
     * multiple substitution's first component). */
    size_t j = start;
    for (size_t i = start; i < end; ++i) {
        uint32_t flag = FLAG_UNSAFE(info[i].category);
        if (is_halant_use(&info[i]))
            j = i + 1;
        else if ((flag & (FLAG(VEGA_USE_VPRE) | FLAG(VEGA_USE_VMPRE))) && !lig_comp(&info[i]) && j < i) {
            merge_clusters(b, j, i + 1);
            Info t = info[i];
            memmove(&info[j + 1], &info[j], (i - j) * sizeof(Info));
            info[j] = t;
        }
    }
}

static bool use_reorder(const VegaShaping *s, VegaShaper *b) {
    bool changed = insert_dotted_circles(s, b, USE_BROKEN, VEGA_USE_B, VEGA_USE_R, -1);
    for (size_t start = 0, end; start < b->len; start = end) {
        end = next_syllable(b, start);
        use_reorder_syllable(b, start, end);
    }
    return changed;
}

/* ---- Thai and Lao (hb-ot-shaper-thai.cc): SARA AM taken apart into
 * NIKHAHIT and SARA AA, the NIKHAHIT moved back over the above-base marks;
 * in a font with no Thai script, marks shifted by the font's private-use
 * glyphs (Windows' or the Mac's) as the stacking around the base asks. */

enum { THAI_NC, THAI_AC, THAI_RC, THAI_DC, THAI_NOT_CONSONANT };
enum { THAI_AV, THAI_BV, THAI_T, THAI_NOT_MARK };
enum { THAI_NOP, THAI_SD, THAI_SL, THAI_SDL, THAI_RD };

static unsigned thai_consonant_type(uint32_t u) {
    if (u == 0x0E1B || u == 0x0E1D || u == 0x0E1F) return THAI_AC;
    if (u == 0x0E0D || u == 0x0E10) return THAI_RC;
    if (u == 0x0E0E || u == 0x0E0F) return THAI_DC;
    if (u >= 0x0E01 && u <= 0x0E2E) return THAI_NC;
    return THAI_NOT_CONSONANT;
}

static unsigned thai_mark_type(uint32_t u) {
    if (u == 0x0E31 || (u >= 0x0E34 && u <= 0x0E37) || u == 0x0E47 || (u >= 0x0E4D && u <= 0x0E4E)) return THAI_AV;
    if (u >= 0x0E38 && u <= 0x0E3A) return THAI_BV;
    if (u >= 0x0E48 && u <= 0x0E4C) return THAI_T;
    return THAI_NOT_MARK;
}

/* The private-use code point the font has for u shifted by action
 * (Windows' before the Mac's), or u. */
static uint32_t thai_pua_shape(const VegaShaping *s, uint32_t u, unsigned action) {
    static const uint16_t sd[][3] = {{0x0E48, 0xF70A, 0xF88B}, {0x0E49, 0xF70B, 0xF88E}, {0x0E4A, 0xF70C, 0xF891},
                                     {0x0E4B, 0xF70D, 0xF894}, {0x0E4C, 0xF70E, 0xF897}, {0x0E38, 0xF718, 0xF89B},
                                     {0x0E39, 0xF719, 0xF89C}, {0x0E3A, 0xF71A, 0xF89D}, {0, 0, 0}};
    static const uint16_t sdl[][3] = {{0x0E48, 0xF705, 0xF88C}, {0x0E49, 0xF706, 0xF88F}, {0x0E4A, 0xF707, 0xF892},
                                      {0x0E4B, 0xF708, 0xF895}, {0x0E4C, 0xF709, 0xF898}, {0, 0, 0}};
    static const uint16_t sl[][3] = {{0x0E48, 0xF713, 0xF88A}, {0x0E49, 0xF714, 0xF88D}, {0x0E4A, 0xF715, 0xF890},
                                     {0x0E4B, 0xF716, 0xF893}, {0x0E4C, 0xF717, 0xF896}, {0x0E31, 0xF710, 0xF884},
                                     {0x0E34, 0xF701, 0xF885}, {0x0E35, 0xF702, 0xF886}, {0x0E36, 0xF703, 0xF887},
                                     {0x0E37, 0xF704, 0xF888}, {0x0E47, 0xF712, 0xF889}, {0x0E4D, 0xF711, 0xF899},
                                     {0, 0, 0}};
    static const uint16_t rd[][3] = {{0x0E0D, 0xF70F, 0xF89A}, {0x0E10, 0xF700, 0xF89E}, {0, 0, 0}};
    const uint16_t(*mappings)[3] = action == THAI_SD ? sd : action == THAI_SDL ? sdl : action == THAI_SL ? sl
                                   : action == THAI_RD ? rd : NULL;
    if (!mappings) return u;
    uint32_t glyph;
    for (; mappings[0][0]; mappings++)
        if (mappings[0][0] == u) {
            if (nominal(s, mappings[0][1], &glyph)) return mappings[0][1];
            if (nominal(s, mappings[0][2], &glyph)) return mappings[0][2];
            break;
        }
    return u;
}

static void thai_pua_shaping(const VegaShaping *s, VegaShaper *b) {
    /* above: T0..T3; below: B0..B2 (hb-ot-shaper-thai.cc's state machines) */
    static const uint8_t above_start[5] = {0, 1, 0, 0, 3}, below_start[5] = {0, 0, 1, 2, 2};
    static const uint8_t above[4][3][2] = {{{THAI_NOP, 3}, {THAI_NOP, 0}, {THAI_SD, 3}},
                                           {{THAI_SL, 2}, {THAI_NOP, 1}, {THAI_SDL, 2}},
                                           {{THAI_NOP, 3}, {THAI_NOP, 2}, {THAI_SL, 3}},
                                           {{THAI_NOP, 3}, {THAI_NOP, 3}, {THAI_NOP, 3}}};
    static const uint8_t below[3][3][2] = {{{THAI_NOP, 0}, {THAI_NOP, 2}, {THAI_NOP, 0}},
                                           {{THAI_NOP, 1}, {THAI_RD, 2}, {THAI_NOP, 1}},
                                           {{THAI_NOP, 2}, {THAI_SD, 2}, {THAI_NOP, 2}}};
    unsigned above_state = above_start[THAI_NOT_CONSONANT], below_state = below_start[THAI_NOT_CONSONANT];
    size_t base = 0;
    for (size_t i = 0; i < b->len; ++i) {
        unsigned mt = thai_mark_type(b->info[i].codepoint);
        if (mt == THAI_NOT_MARK) {
            unsigned ct = thai_consonant_type(b->info[i].codepoint);
            above_state = above_start[ct];
            below_state = below_start[ct];
            base = i;
            continue;
        }
        const uint8_t *a = above[above_state][mt], *w = below[below_state][mt];
        above_state = a[1];
        below_state = w[1];
        unsigned action = a[0] != THAI_NOP ? a[0] : w[0];
        if (action == THAI_RD) b->info[base].codepoint = thai_pua_shape(s, b->info[base].codepoint, action);
        else b->info[i].codepoint = thai_pua_shape(s, b->info[i].codepoint, action);
    }
}

static bool thai_above_base_mark(uint32_t u) {
    u &= ~0x80u;
    return (u >= 0x0E34 && u <= 0x0E37) || (u >= 0x0E47 && u <= 0x0E4E) || u == 0x0E31 || u == 0x0E3B;
}

static void preprocess_thai(const VegaShaping *s, const Plan *plan, VegaShaper *b) {
    clear_output(b);
    size_t count = b->len;
    while (b->idx < count && b->successful) {
        uint32_t u = b->info[b->idx].codepoint;
        if ((u & ~0x80u) != 0x0E33) { /* not SARA AM */
            next_glyph(b);
            continue;
        }
        output_glyph(b, u - 0x0E33 + 0x0E4D); /* NIKHAHIT, continuing the character before */
        if (b->out_len) b->out[b->out_len - 1].uprops |= UPROPS_CONTINUATION;
        replace_glyph(b, u - 1); /* SARA AA */
        if (!b->successful) break;
        size_t end = b->out_len;
        /* The NIKHAHIT counts as a mark (of class 0) when widths are zeroed. */
        b->out[end - 2].uprops = (uint16_t)((b->out[end - 2].uprops & ~0x1Fu) | VEGA_MN);
        size_t start = end - 2;
        while (start > 0 && thai_above_base_mark(b->out[start - 1].codepoint)) start--;
        if (start + 2 < end) {
            merge_out_clusters(b, start, end);
            Info t = b->out[end - 2];
            memmove(b->out + start + 1, b->out + start, (end - start - 2) * sizeof(Info));
            b->out[start] = t;
        } else if (start)
            merge_out_clusters(b, start - 1, end);
    }
    sync(b);
    if (plan->shaper_flags & VEGA_THAI_PUA) thai_pua_shaping(s, b);
}

/* Arabic fallback shaping (hb-ot-shaper-arabic-fallback.hh): the lookups
 * the build tool made from the font's presentation forms, each under its
 * feature's mask, applied as HarfBuzz applies a GSUB lookup outside a plan
 * (both joiners skipped, no syllables). */
static bool arabic_fallback(const VegaShaping *s, const Plan *plan, VegaShaper *b) {
    if (!(plan->shaper_flags & VEGA_ARABIC_FALLBACK)) return false;
    static const int forms[4] = {VEGA_MASK_INIT, VEGA_MASK_MEDI, VEGA_MASK_FINA, VEGA_MASK_ISOL};
    for (uint32_t i = 0; i < s->fallback_count; ++i) {
        unsigned feature = s->fallback_features[i];
        uint32_t mask = plan->masks[feature < 4 ? forms[feature] : VEGA_MASK_ARABIC_RLIG];
        if (!mask) continue;
        Apply c = {.s = s, .b = b, .table = 0, .t = s->fallback, .lookup_mask = mask, .auto_zwnj = true,
                   .auto_zwj = true, .has_glyph_classes = s->glyph_classes != 0, .nesting_left = MAX_NESTING_LEVEL,
                   .random_state = 1, .last_base = -1};
        apply_string(&c, &s->fallback_lookups[i]);
    }
    return true;
}

/* Stretching (hb-ot-shaper-arabic.cc's apply_stch), after positioning: each
 * run of stretch pieces fills the width of the word before it, its
 * repeating pieces copied as often as fits (overlapping a little to fit
 * closer), fixed pieces once. Measured first, then cut. */
static bool arabic_word_category(unsigned c) {
    switch (c) {
    case VEGA_CN: case VEGA_CO: case VEGA_LM: case VEGA_LO: case VEGA_MC: case VEGA_ME: case VEGA_MN: case VEGA_ND:
    case VEGA_NL: case VEGA_NO: case VEGA_SC: case VEGA_SK: case VEGA_SM: case VEGA_SO:
        return true;
    default:
        return false;
    }
}
static bool stretching(const Info *i) { return i->action == STCH_FIXED || i->action == STCH_REPEATING; }

static void apply_stch(const VegaShaping *s, VegaShaper *b) {
    if (!(b->scratch & HAS_STCH)) return;
    bool rtl = b->direction == VEGA_SHAPE_RTL;
    if (!rtl) reverse_range(b, 0, b->len, true);
    size_t extra = 0;
    for (int cut = 0; cut <= 1; ++cut) {
        size_t count = b->len, new_len = count + extra, j = new_len;
        Info *info = b->info;
        Position *pos = b->pos;
        for (size_t i = count; i; i--) {
            if (!stretching(&info[i - 1])) {
                if (cut) {
                    --j;
                    info[j] = info[i - 1];
                    pos[j] = pos[i - 1];
                }
                continue;
            }
            int32_t w_total = 0, w_fixed = 0, w_repeating = 0;
            int n_repeating = 0;
            size_t end = i;
            while (i && stretching(&info[i - 1])) {
                i--;
                int32_t width = advance_of(s, info[i].codepoint);
                if (info[i].action == STCH_FIXED) w_fixed += width;
                else w_repeating += width, n_repeating++;
            }
            size_t start = i, context = i;
            while (context && !stretching(&info[context - 1]) &&
                   (is_default_ignorable(&info[context - 1]) || arabic_word_category(gen_cat(&info[context - 1])))) {
                context--;
                w_total += pos[context].x_advance;
            }
            i++;
            int n_copies = 0;
            int32_t w_remaining = w_total - w_fixed;
            if (w_remaining > w_repeating && w_repeating > 0) n_copies = w_remaining / w_repeating - 1;
            int32_t overlap = 0, shortfall = w_remaining - w_repeating * (n_copies + 1);
            if (shortfall > 0 && n_repeating > 0) {
                ++n_copies;
                int32_t excess = (n_copies + 1) * w_repeating - w_remaining;
                if (excess > 0) {
                    overlap = excess / (n_copies * n_repeating);
                    w_remaining = 0;
                }
            }
            if (!cut) {
                extra += (size_t)n_copies * (size_t)n_repeating;
                continue;
            }
            int32_t x_offset = w_remaining / 2;
            for (size_t k = end; k > start; k--) {
                int32_t width = advance_of(s, info[k - 1].codepoint);
                unsigned repeat = 1 + (info[k - 1].action == STCH_REPEATING ? (unsigned)n_copies : 0);
                pos[k - 1].x_advance = 0;
                for (unsigned n = 0; n < repeat; n++) {
                    if (rtl) {
                        x_offset -= width;
                        if (n > 0) x_offset += overlap;
                    }
                    pos[k - 1].x_offset = x_offset;
                    --j;
                    info[j] = info[k - 1];
                    pos[j] = pos[k - 1];
                    if (!rtl) {
                        x_offset += width;
                        if (n > 0) x_offset -= overlap;
                    }
                }
            }
        }
        if (!cut) {
            if (!ensure(b, count + extra)) break;
        } else
            b->len = new_len;
    }
    if (!rtl) reverse_range(b, 0, b->len, true);
}

/* The shaper's part of the work at a pause in GSUB; whether it changed the
 * buffer. */
static bool shaper_pause(const VegaShaping *s, const Plan *plan, VegaShaper *b, unsigned pause) {
    switch (plan->shaper) {
    case VEGA_SHAPER_ARABIC:
        /* stch just applied: what it multiplied is stretched after positioning */
        if (pause == VEGA_PAUSE_RECORD_STCH && (plan->shaper_flags & VEGA_ARABIC_STCH))
            for (size_t i = 0; i < b->len; ++i)
                if (b->info[i].gprops & MULTIPLIED) {
                    b->info[i].action = lig_comp(&b->info[i]) % 2 ? STCH_REPEATING : STCH_FIXED;
                    b->scratch |= HAS_STCH;
                }
        return pause == VEGA_PAUSE_ARABIC_FALLBACK && arabic_fallback(s, plan, b);
    case VEGA_SHAPER_INDIC: {
        Indic d = indic_of(s, plan);
        switch (pause) {
        case VEGA_PAUSE_SYLLABLES: find_syllables(b, &vega_indic_machine); return false;
        case VEGA_PAUSE_INITIAL_REORDERING: return indic_initial_reordering(&d, b);
        case VEGA_PAUSE_FINAL_REORDERING: indic_final_reordering(&d, b); return false;
        default: return false;
        }
    }
    case VEGA_SHAPER_KHMER:
        if (pause == VEGA_PAUSE_SYLLABLES) find_syllables(b, &vega_khmer_machine);
        return pause == VEGA_PAUSE_INITIAL_REORDERING && khmer_reorder(s, plan, b);
    case VEGA_SHAPER_MYANMAR:
        if (pause == VEGA_PAUSE_SYLLABLES) find_syllables(b, &vega_myanmar_machine);
        return pause == VEGA_PAUSE_INITIAL_REORDERING && myanmar_reorder(s, b);
    case VEGA_SHAPER_USE:
        switch (pause) {
        case VEGA_PAUSE_SYLLABLES: use_setup_syllables(plan, b); return false;
        case VEGA_PAUSE_CLEAR_SUBSTITUTED:
            for (size_t i = 0; i < b->len; ++i) b->info[i].gprops &= (uint16_t)~SUBSTITUTED;
            return false;
        case VEGA_PAUSE_RECORD_RPHF: case VEGA_PAUSE_RECORD_PREF: use_record(plan, b, pause); return false;
        case VEGA_PAUSE_INITIAL_REORDERING: return use_reorder(s, b);
        default: return false;
        }
    default: return false;
    }
}

/* ---- Positions */

static void fallback_spaces(const VegaShaping *s, VegaShaper *b) {
    for (size_t i = 0; i < b->len; ++i) {
        const Info *info = &b->info[i];
        if (!is_unicode_space(info) || (info->gprops & LIGATED)) continue;
        unsigned type = info->uprops >> 8;
        int32_t upem = s->upem;
        uint32_t glyph;
        switch (type) {
        case SPACE_EM: case SPACE_EM_2: case SPACE_EM_3: case SPACE_EM_4: case SPACE_EM_5: case SPACE_EM_6:
        case SPACE_EM_16:
            b->pos[i].x_advance = (upem + (int32_t)type / 2) / (int32_t)type;
            break;
        case SPACE_4_EM_18: b->pos[i].x_advance = (int32_t)((int64_t)upem * 4 / 18); break;
        case SPACE_FIGURE:
            for (uint32_t u = '0'; u <= '9'; ++u)
                if (nominal(s, u, &glyph)) {
                    b->pos[i].x_advance = advance_of(s, glyph);
                    break;
                }
            break;
        case SPACE_PUNCTUATION:
            if (nominal(s, '.', &glyph) || nominal(s, ',', &glyph)) b->pos[i].x_advance = advance_of(s, glyph);
            break;
        case SPACE_NARROW: b->pos[i].x_advance /= 2; break;
        default: break;
        }
    }
}

static void propagate_attachment_offsets(Position *pos, size_t len, size_t i, int direction, int nesting) {
    int32_t chain = pos[i].chain;
    uint8_t type = pos[i].attach;
    if (!chain) return;
    pos[i].chain = 0;
    size_t j = (size_t)((long)i + chain);
    if (j >= len || !nesting) return;
    propagate_attachment_offsets(pos, len, j, direction, nesting - 1);
    if (type & ATTACH_CURSIVE) {
        pos[i].y_offset += pos[j].y_offset;
        return;
    }
    pos[i].x_offset += pos[j].x_offset;
    pos[i].y_offset += pos[j].y_offset;
    if (direction == VEGA_SHAPE_LTR)
        for (size_t k = j; k < i; ++k) {
            pos[i].x_offset -= pos[k].x_advance;
            pos[i].y_offset -= pos[k].y_advance;
        }
    else
        for (size_t k = j + 1; k < i + 1; ++k) {
            pos[i].x_offset += pos[k].x_advance;
            pos[i].y_offset += pos[k].y_advance;
        }
}

static void zero_mark_widths(VegaShaper *b, bool adjust) {
    for (size_t i = 0; i < b->len; ++i)
        if (b->info[i].gprops & MARK) {
            if (adjust) {
                b->pos[i].x_offset -= b->pos[i].x_advance;
                b->pos[i].y_offset -= b->pos[i].y_advance;
            }
            b->pos[i].x_advance = b->pos[i].y_advance = 0;
        }
}

static void position(const VegaShaping *s, const Plan *plan, VegaShaper *b) {
    b->have_output = false;
    b->out_len = 0;
    memset(b->pos, 0, b->len * sizeof(Position));
    for (size_t i = 0; i < b->len; ++i) b->pos[i].x_advance = advance_of(s, b->info[i].codepoint);
    if (b->scratch & HAS_SPACE_FALLBACK) fallback_spaces(s, b);
    bool adjust = (plan->flags & VEGA_PLAN_ADJUST_ZEROING) && b->direction == VEGA_SHAPE_LTR;
    /* Marks' advances go by GDEF: before GPOS (Myanmar's shaper and USE) or after. */
    bool early = plan->shaper == VEGA_SHAPER_MYANMAR || plan->shaper == VEGA_SHAPER_USE;
    if ((plan->flags & VEGA_PLAN_ZERO_MARKS) && early) zero_mark_widths(b, adjust);
    if (plan->flags & VEGA_PLAN_GPOS) apply_table(s, plan, b, 1);
    if (plan->flags & VEGA_PLAN_KERN) kern_table(s, b, plan);
    if ((plan->flags & VEGA_PLAN_ZERO_MARKS) && !early) zero_mark_widths(b, adjust);
    if (b->scratch & HAS_DEFAULT_IGNORABLES)
        for (size_t i = 0; i < b->len; ++i)
            if (is_default_ignorable(&b->info[i]))
                b->pos[i].x_advance = b->pos[i].y_advance = b->pos[i].x_offset = b->pos[i].y_offset = 0;
    if (b->scratch & HAS_GPOS_ATTACHMENT)
        for (size_t i = 0; i < b->len; ++i) propagate_attachment_offsets(b->pos, b->len, i, b->direction, MAX_NESTING_LEVEL);
    if (plan->flags & VEGA_PLAN_FALLBACK_MARKS) fallback_marks(s, plan, b, adjust);
    if (b->direction == VEGA_SHAPE_RTL) reverse_range(b, 0, b->len, true);
}

/* ---- Shaping (hb-ot-shape.cc) */

long vega_shape(VegaShaper *b, const VegaShaping *s, const uint32_t *codes, size_t count, uint32_t script, int direction,
                const VegaShaped **out) {
    Plan plan;
    if (!find_plan(s, script, direction, &plan)) return -1;
    b->len = b->out_len = b->idx = 0;
    b->have_output = false;
    b->successful = true;
    b->scratch = b->serial = 0;
    b->direction = direction;
    b->max_len = count * 64 > 16384 ? count * 64 : 16384;
    b->max_ops = (long)(count * 1024 > 16384 ? count * 1024 : 16384);
    if (!ensure(b, count + 1)) return -1;
    for (size_t i = 0; i < count; ++i) b->info[i] = (Info){.codepoint = codes[i], .mask = plan.global_mask, .cluster = (uint32_t)i};
    b->len = count;

    set_unicode_props(b);
    form_clusters(b);
    ensure_native_direction(b, &plan);
    if (plan.shaper == VEGA_SHAPER_HANGUL) preprocess_hangul(s, b);
    if (plan.shaper == VEGA_SHAPER_INDIC || plan.shaper == VEGA_SHAPER_USE) preprocess_vowel_constraints(&plan, b);
    if (plan.shaper == VEGA_SHAPER_THAI) preprocess_thai(s, &plan, b);

    /* Mirror what the font mirrors; mark the rest for rtlm. */
    if (direction == VEGA_SHAPE_RTL)
        for (size_t i = 0; i < b->len; ++i) {
            uint32_t mirror = vega_mirror(b->info[i].codepoint), glyph;
            if (mirror && nominal(s, mirror, &glyph)) b->info[i].codepoint = mirror;
            else b->info[i].mask |= plan.masks[VEGA_MASK_RTLM];
        }
    normalize(s, &plan, b);
    setup_masks_fraction(b, &plan);
    if (plan.shaper == VEGA_SHAPER_ARABIC) setup_masks_arabic(b, &plan);
    if (plan.shaper == VEGA_SHAPER_INDIC) setup_masks_indic(b);
    if (plan.shaper == VEGA_SHAPER_KHMER || plan.shaper == VEGA_SHAPER_MYANMAR) setup_masks_category(b);
    if (plan.shaper == VEGA_SHAPER_USE) setup_masks_use(b, &plan);
    if (plan.shaper == VEGA_SHAPER_HANGUL)
        for (size_t i = 0; i < b->len; ++i)
            if (b->info[i].action >= LJMO && b->info[i].action <= TJMO)
                b->info[i].mask |= plan.masks[VEGA_MASK_LJMO + b->info[i].action - LJMO];
    if (plan.flags & VEGA_PLAN_FALLBACK_MARKS)
        for (size_t i = 0; i < b->len; ++i)
            if (gen_cat(&b->info[i]) == VEGA_MN) set_info_cc(&b->info[i], recategorize(b->info[i].codepoint, info_cc(&b->info[i])));
    for (size_t i = 0; i < b->len; ++i) {
        Info *info = &b->info[i];
        info->codepoint = info->glyph_index;
        info->gprops = glyph_props(s, info->codepoint);
        info->lig_props = 0;
        if (plan.flags & VEGA_PLAN_GUESS_CLASSES)
            info->gprops = gen_cat(info) != VEGA_MN || is_default_ignorable(info) ? BASE_GLYPH : MARK;
    }
    apply_table(s, &plan, b, 0);
    if (!b->successful || !ensure(b, b->len)) return -1;
    position(s, &plan, b);

    /* Hide what shows nothing: the space glyph, with nothing to advance. */
    if (b->scratch & HAS_DEFAULT_IGNORABLES) {
        uint32_t invisible;
        if (nominal(s, ' ', &invisible)) {
            for (size_t i = 0; i < b->len; ++i)
                if (is_default_ignorable(&b->info[i])) b->info[i].codepoint = invisible;
        } else {
            size_t j = 0;
            for (size_t i = 0; i < b->len; ++i) {
                if (is_default_ignorable(&b->info[i])) {
                    /* Its cluster goes to a neighbour (hb_buffer_t::delete_glyphs_inplace). */
                    uint32_t cluster = b->info[i].cluster;
                    if (i + 1 < b->len && cluster == b->info[i + 1].cluster) continue;
                    if (j) {
                        if (cluster < b->info[j - 1].cluster) {
                            uint32_t old = b->info[j - 1].cluster;
                            for (size_t k = j; k && b->info[k - 1].cluster == old; k--) b->info[k - 1].cluster = cluster;
                        }
                        continue;
                    }
                    if (i + 1 < b->len) {
                        uint32_t next = b->info[i + 1].cluster;
                        for (size_t k = i + 1; k < b->len && b->info[k].cluster == next; ++k)
                            b->info[k].cluster = cluster < next ? cluster : next;
                    }
                    continue;
                }
                if (j != i) {
                    b->info[j] = b->info[i];
                    b->pos[j] = b->pos[i];
                }
                j++;
            }
            b->len = j;
        }
    }

    if (plan.shaper == VEGA_SHAPER_ARABIC) apply_stch(s, b);
    if (!b->successful) return -1;

    if (b->glyphs_allocated < b->len) {
        VegaShaped *glyphs = realloc(b->glyphs, b->len * sizeof(*glyphs));
        if (!glyphs) return -1;
        b->glyphs = glyphs;
        b->glyphs_allocated = b->len;
    }
    for (size_t i = 0; i < b->len; ++i)
        b->glyphs[i] = (VegaShaped){b->info[i].codepoint, b->info[i].cluster, b->pos[i].x_advance, b->pos[i].y_advance,
                                   b->pos[i].x_offset, b->pos[i].y_offset};
    *out = b->glyphs;
    return (long)b->len;
}
