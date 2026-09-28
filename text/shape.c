/* Shaping as HarfBuzz 10 does it (text/shape.h), with its default, Arabic
 * and Hebrew shapers. The code follows HarfBuzz's own step for step
 * (hb-ot-shape.cc, hb-ot-shape-normalize.cc, hb-buffer.cc, hb-ot-layout-
 * gsubgpos.hh, the GSUB and GPOS lookups, hb-ot-shaper-arabic.cc and
 * -hebrew.cc), under HarfBuzz's names, so that the two read side by side;
 * the build tool compares the results with HarfBuzz's (symbols-tool.c). */
#include "shape.h"
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* A table's lookups, their subtables' types resolved through extensions. */
static bool resolve_lookups(VegaShaping *s, int table, size_t *subtables, size_t *allocated) {
    VegaTable t = table ? s->gpos : s->gsub;
    uint32_t list = t.size ? offset16(t, 0, 8) : 0, count = list ? get16(t, list) : 0;
    s->lookup_count[table] = count;
    if (!(s->lookups[table] = calloc(count ? count : 1, sizeof(VegaLookup)))) return false;
    uint16_t extension = table ? 9 : 7;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = offset16(t, list, list + 2 + 2 * i), n = at ? get16(t, at + 4) : 0;
        VegaLookup *l = &s->lookups[table][i];
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
    s->cmap_count = le32(bytes + 12);
    uint32_t cmap = le32(bytes + 16), advances = le32(bytes + 20);
    if (!s->upem) REFUSE("units per em");
    if (!within(size, cmap, (uint64_t)s->cmap_count * 8)) REFUSE("character map");
    s->cmap = bytes + cmap;
    for (uint32_t i = 0; i < s->cmap_count; ++i) {
        uint32_t code = le32(s->cmap + 8 * i), glyph = le32(s->cmap + 8 * i + 4);
        if ((i && code <= le32(s->cmap + 8 * i - 8)) || !glyph || glyph >= s->glyph_count)
            REFUSE("character map: entry %u", i);
    }
    for (uint32_t i = 0; i < s->cmap_count; ++i) {
        uint32_t code = le32(s->cmap + 8 * i);
        if (code > 0xFFFF) break;
        uint16_t **page = &s->bmp[code >> 8];
        if (!*page && !(*page = calloc(256, sizeof(**page)))) REFUSE("out of memory");
        (*page)[code & 255] = (uint16_t)le32(s->cmap + 8 * i + 4);
    }
    if (!within(size, advances, 2 * (uint64_t)s->glyph_count)) REFUSE("advances");
    s->advances = bytes + advances;
    VegaTable *tables[3] = {&s->gdef, &s->gsub, &s->gpos};
    for (int k = 0; k < 3; ++k) {
        uint32_t at = le32(bytes + 24 + 8 * k), n = le32(bytes + 28 + 8 * k);
        if (!within(size, at, n)) REFUSE("layout table %d", k);
        *tables[k] = (VegaTable){n ? bytes + at : NULL, n};
    }
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
    size_t subtables = 0, allocated = 0;
    if (!resolve_lookups(s, 0, &subtables, &allocated) || !resolve_lookups(s, 1, &subtables, &allocated))
        REFUSE("out of memory");
    for (uint32_t i = 0; i < s->plan_count; ++i) {
        const unsigned char *p = s->plans + (size_t)i * VEGA_PLAN_BYTES;
        uint32_t ranges[2][2] = {{le32(p + 56), le32(p + 60)}, {le32(p + 64), le32(p + 68)}};
        if ((p[4] != VEGA_SHAPE_LTR && p[4] != VEGA_SHAPE_RTL) || p[5] > VEGA_SHAPE_RTL || p[6] > VEGA_SHAPER_HEBREW)
            REFUSE("plan %u", i);
        for (int table = 0; table < 2; ++table) {
            if (ranges[table][0] > step_count || ranges[table][1] > step_count - ranges[table][0])
                REFUSE("plan %u: steps", i);
            for (uint32_t k = 0; k < ranges[table][1]; ++k)
                if (le16(s->steps + (size_t)(ranges[table][0] + k) * VEGA_STEP_BYTES) >= s->lookup_count[table])
                    REFUSE("plan %u: a lookup the font lacks", i);
        }
    }
    return true;
}

void vega_shaping_close(VegaShaping *s) {
    for (int i = 0; i < 256; ++i) free(s->bmp[i]), s->bmp[i] = NULL;
    free(s->lookups[0]);
    free(s->lookups[1]);
    free(s->subtables);
    s->lookups[0] = s->lookups[1] = NULL;
    s->subtables = NULL;
}

uint32_t vega_shaping_glyph(const VegaShaping *s, uint32_t code) {
    if (code <= 0xFFFF) return s->bmp[code >> 8] ? s->bmp[code >> 8][code & 255] : 0;
    uint32_t low = 0, high = s->cmap_count;
    while (low < high) {
        uint32_t middle = (low + high) / 2, c = le32(s->cmap + 8 * (size_t)middle);
        if (c < code) low = middle + 1;
        else if (c > code) high = middle;
        else return le32(s->cmap + 8 * (size_t)middle + 4);
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
    uint8_t lig_props, action;
} Info;
typedef struct {
    int32_t x_advance, y_advance, x_offset, y_offset;
    int32_t chain;
    uint8_t attach;
} Position;

struct VegaShaper {
    Info *info, *out;
    Position *pos;
    size_t len, out_len, idx, allocated, max_len;
    bool have_output, successful;
    int direction;
    uint32_t scratch, serial;
    long max_ops;
    VegaShaped *glyphs;
    size_t glyphs_allocated;
};

enum { HAS_NON_ASCII = 1, HAS_DEFAULT_IGNORABLES = 2, HAS_SPACE_FALLBACK = 4, HAS_GPOS_ATTACHMENT = 8, HAS_CGJ = 16,
       HAS_VARIATION_SELECTOR_FALLBACK = 32 };

VegaShaper *vega_shaper_new(void) { return calloc(1, sizeof(VegaShaper)); }

void vega_shaper_free(VegaShaper *b) {
    if (!b) return;
    free(b->info);
    free(b->out);
    free(b->pos);
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
    if (!pos) {
        b->successful = false;
        return false;
    }
    b->pos = pos;
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
static bool default_ignorable(uint32_t ch) {
    uint32_t plane = ch >> 16;
    if (!plane) {
        switch (ch >> 8) {
        case 0x00: return ch == 0x00AD;
        case 0x03: return ch == 0x034F;
        case 0x06: return ch == 0x061C;
        case 0x17: return ch >= 0x17B4 && ch <= 0x17B5;
        case 0x18: return ch >= 0x180B && ch <= 0x180F;
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
 * positions in the SBL order, Arabic's shadda before the other marks. */
static unsigned modified_combining_class(uint32_t u) {
    if (u == 0x1A60 || u == 0x0FC6) return 254;
    if (u == 0x0F39) return 127;
    static const uint8_t hebrew_arabic[27] = {22, 15, 16, 17, 23, 18, 19, 20, 21, 14, 24, 12, 25, 13,
                                              10, 11, 26, 28, 29, 30, 31, 32, 33, 27, 34, 35, 36};
    unsigned c = vega_combining_class(u);
    if (c >= 10 && c <= 36) return hebrew_arabic[c - 10];
    switch (c) {
    case 84: case 91: return 0;
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
        if (default_ignorable(u)) {
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

typedef struct {
    uint32_t script;
    int direction, native, shaper, flags;
    uint32_t global_mask, masks[VEGA_PLAN_MASKS];
    const unsigned char *gsub, *gpos;
    uint32_t gsub_count, gpos_count;
} Plan;

static bool find_plan(const VegaShaping *s, uint32_t script, int direction, Plan *plan) {
    const unsigned char *found = NULL;
    for (int pass = 0; pass < 2 && !found; ++pass)
        for (uint32_t i = 0; i < s->plan_count && !found; ++i) {
            const unsigned char *p = s->plans + (size_t)i * VEGA_PLAN_BYTES;
            if (le32(p) == (pass ? 0x5A797979u : script) && p[4] == direction) found = p;
        }
    if (!found) return false;
    *plan = (Plan){le32(found), found[4], found[5], found[6], found[7], le32(found + 8), {0},
                   s->steps + (size_t)le32(found + 56) * VEGA_STEP_BYTES,
                   s->steps + (size_t)le32(found + 64) * VEGA_STEP_BYTES, le32(found + 60), le32(found + 68)};
    for (int k = 0; k < VEGA_PLAN_MASKS; ++k) plan->masks[k] = le32(found + 12 + 4 * k);
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
    case 0x05B7: if (a == 0x05D9) *ab = 0xFB1F; else if (a == 0x05D0) *ab = 0xFB2E; break;
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

static bool compose(const Normalizer *c, uint32_t a, uint32_t b, uint32_t *ab) {
    return c->plan->shaper == VEGA_SHAPER_HEBREW ? compose_hebrew(c, a, b, ab) : compose_unicode(a, b, ab);
}

static bool nominal(const VegaShaping *s, uint32_t u, uint32_t *glyph) { return (*glyph = vega_shaping_glyph(s, u)) != 0; }

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

/* Returns how many characters ab became, 0 if it did not decompose. */
static unsigned decompose(const Normalizer *c, bool shortest, uint32_t ab) {
    uint32_t a, bb, a_glyph = 0, b_glyph = 0;
    if (!decompose_unicode(ab, &a, &bb) || (bb && !nominal(c->s, bb, &b_glyph))) return 0;
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

/* A variation selector's cluster: normalization gives up on it. (Variation
 * sequences the font maps, cmap format 14, are not carried: each character
 * passes on alone, the selector hidden.) */
static void handle_variation_selector_cluster(const Normalizer *c, size_t end) {
    VegaShaper *b = c->b;
    uint32_t glyph;
    while (b->idx + 1 < end && b->successful) {
        if (variation_selector(b->info[b->idx + 1].codepoint)) {
            b->info[b->idx].glyph_index = nominal(c->s, b->info[b->idx].codepoint, &glyph) ? glyph : 0;
            next_glyph(b);
            b->scratch |= HAS_VARIATION_SELECTOR_FALLBACK;
            Info *vs = &b->info[b->idx];
            vs->uprops = (uint16_t)((vs->uprops & ~0x1Fu & 0xFF) | VEGA_CF | UPROPS_VS);
            vs->glyph_index = nominal(c->s, vs->codepoint, &glyph) ? glyph : 0;
            next_glyph(b);
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
    /* The shapers' preference is "auto": composed diacritics. */
    const bool might_short_circuit = true, always_short_circuit = false;
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

    if (!all_simple && b->successful) {
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

enum { ISOL, FINA, FIN2, FIN3, MEDI, MED2, INIT, NONE };
enum { JT_U, JT_L, JT_R, JT_D, JG_ALAPH, JG_DALATH_RISH, JT_T = 7 };

static unsigned joining_type(uint32_t u) {
    switch (vega_joining(u)) {
    case VEGA_JOINING_L: return JT_L;
    case VEGA_JOINING_R: return JT_R;
    case VEGA_JOINING_D: case VEGA_JOINING_C: return JT_D;
    case VEGA_JOINING_T: return JT_T;
    default: return JT_U;
    }
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
        unsigned type = joining_type(b->info[i].codepoint);
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
    for (size_t i = 0; i < b->len; ++i)
        if (b->info[i].action < NONE) b->info[i].mask |= plan->masks[VEGA_MASK_ISOL + b->info[i].action];
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
} Skippy;

enum { SKIP_NO, SKIP_YES, SKIP_MAYBE };
enum { MATCH_NO, MATCH_YES, MATCH_MAYBE };
enum { MATCH, NOT_MATCH, SKIP };

static void skippy_init(Skippy *it, Apply *c, bool context_match, size_t start) {
    *it = (Skippy){c, c->lookup_props, context_match ? 0xFFFFFFFFu : c->lookup_mask,
                   c->table == 1 || (context_match && c->auto_zwnj), context_match || c->auto_zwj, c->table == 1,
                   {MATCH_ANY, 0, 0}, 0, start, c->b->len};
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
    if (!(info->mask & it->mask)) return MATCH_NO;
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

static void apply_table(const VegaShaping *s, VegaShaper *b, int table, const unsigned char *steps, uint32_t count) {
    /* A lookup can apply only where the buffer has a glyph it starts from. */
    Apply c = {s, b, table, table ? s->gpos : s->gsub, 0, 0, true, true, false, s->glyph_classes != 0,
               MAX_NESTING_LEVEL, 1, -1, 0, buffer_digest(b)};
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *step = steps + (size_t)i * VEGA_STEP_BYTES;
        uint32_t index = le16(step);
        if (!digest_may_intersect(&s->lookups[table][index].digest, &c.digest)) continue;
        c.lookup_mask = le32(step + 4);
        c.last_base = -1;
        c.last_base_until = 0;
        c.auto_zwnj = step[2] & VEGA_STEP_AUTO_ZWNJ;
        c.auto_zwj = step[2] & VEGA_STEP_AUTO_ZWJ;
        c.random = step[2] & VEGA_STEP_RANDOM;
        apply_string(&c, &s->lookups[table][index]);
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

static void position(const VegaShaping *s, const Plan *plan, VegaShaper *b) {
    b->have_output = false;
    b->out_len = 0;
    memset(b->pos, 0, b->len * sizeof(Position));
    for (size_t i = 0; i < b->len; ++i) b->pos[i].x_advance = advance_of(s, b->info[i].codepoint);
    if (b->scratch & HAS_SPACE_FALLBACK) fallback_spaces(s, b);
    if (plan->flags & VEGA_PLAN_GPOS) apply_table(s, b, 1, plan->gpos, plan->gpos_count);
    if (plan->flags & VEGA_PLAN_ZERO_MARKS)
        for (size_t i = 0; i < b->len; ++i)
            if (b->info[i].gprops & MARK) b->pos[i].x_advance = b->pos[i].y_advance = 0;
    if (b->scratch & HAS_DEFAULT_IGNORABLES)
        for (size_t i = 0; i < b->len; ++i)
            if (is_default_ignorable(&b->info[i]))
                b->pos[i].x_advance = b->pos[i].y_advance = b->pos[i].x_offset = b->pos[i].y_offset = 0;
    if (b->scratch & HAS_GPOS_ATTACHMENT)
        for (size_t i = 0; i < b->len; ++i) propagate_attachment_offsets(b->pos, b->len, i, b->direction, MAX_NESTING_LEVEL);
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
    for (size_t i = 0; i < count; ++i) b->info[i] = (Info){codes[i], plan.global_mask, (uint32_t)i, 0, 0, 0, 0, 0};
    b->len = count;

    set_unicode_props(b);
    form_clusters(b);
    ensure_native_direction(b, &plan);

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
    for (size_t i = 0; i < b->len; ++i) {
        Info *info = &b->info[i];
        info->codepoint = info->glyph_index;
        info->gprops = glyph_props(s, info->codepoint);
        info->lig_props = 0;
        if (plan.flags & VEGA_PLAN_GUESS_CLASSES)
            info->gprops = gen_cat(info) != VEGA_MN || is_default_ignorable(info) ? BASE_GLYPH : MARK;
    }
    apply_table(s, b, 0, plan.gsub, plan.gsub_count);
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
