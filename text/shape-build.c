/* Building shaping sections with HarfBuzz (text/shape-build.h): plans as
 * HarfBuzz's planner compiles them (hb-ot-shape.cc's features, hb-ot-map.cc's
 * masks and stages), what the section carries, and a check of it against
 * HarfBuzz on random runs. */
#include "shape-build.h"
#include "syllabic.h"
#include "text.h"
#include <hb-ot.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- The planner (hb_ot_map_builder_t) */

enum { F_NONE = 0, F_GLOBAL = 1, F_HAS_FALLBACK = 2, F_MANUAL_ZWNJ = 4, F_MANUAL_ZWJ = 8, F_GLOBAL_SEARCH = 16,
       F_RANDOM = 32, F_PER_SYLLABLE = 64 };
enum { MAX_VALUE = 255, GLOBAL_BIT_SHIFT = 31, MAX_BITS = 8, MAX_FEATURES = 96, MAX_STAGES = 32 };
#define GLOBAL_BIT_MASK (1u << GLOBAL_BIT_SHIFT)
#define NO_FEATURE HB_OT_LAYOUT_NO_FEATURE_INDEX
#define TAG(s) HB_TAG((s)[0], (s)[1], (s)[2], (s)[3])

typedef struct { hb_tag_t tag; unsigned seq, max_value, flags, default_value, stage[2]; } FeatureInfo;
typedef struct {
    hb_tag_t tag;
    unsigned index[2], stage[2], shift;
    uint32_t mask, one_mask;
    bool auto_zwnj, auto_zwj, random, per_syllable, needs_fallback;
} FeatureMap;
/* A lookup to run, or (index VEGA_STEP_PAUSE) the pause that ends a stage. */
typedef struct { unsigned index; uint32_t mask; bool auto_zwnj, auto_zwj, random, per_syllable; unsigned pause; } LookupMap;

typedef struct {
    hb_face_t *face;
    unsigned script_index[2], language_index[2];
    hb_tag_t chosen_script[2];
    bool found_script[2]; /* one of the script's own tags, not DFLT or latn */
    FeatureInfo infos[MAX_FEATURES];
    unsigned info_count, current_stage[2];
    unsigned pause[2][MAX_STAGES]; /* what ends each stage: VEGA_PAUSE_..., or 0 */
    bool is_simple;
    /* compiled */
    FeatureMap features[MAX_FEATURES];
    unsigned feature_count;
    uint32_t global_mask;
    LookupMap *lookups[2];
    unsigned lookup_count[2];
    unsigned stage_first[2][MAX_STAGES], stage_end[2][MAX_STAGES]; /* each stage's lookups */
} Map;

static const hb_tag_t table_tags[2] = {HB_OT_TAG_GSUB, HB_OT_TAG_GPOS};

static void add_feature(Map *m, hb_tag_t tag, unsigned flags, unsigned value) {
    if (!tag || m->info_count == MAX_FEATURES) return;
    m->infos[m->info_count] = (FeatureInfo){tag, m->info_count, value, flags, flags & F_GLOBAL ? value : 0,
                                            {m->current_stage[0], m->current_stage[1]}};
    m->info_count++;
}
static void enable_feature(Map *m, hb_tag_t tag, unsigned flags, unsigned value) { add_feature(m, tag, flags | F_GLOBAL, value); }
static void add_gsub_pause(Map *m, unsigned pause) {
    if (m->current_stage[0] < MAX_STAGES) m->pause[0][m->current_stage[0]] = pause;
    m->current_stage[0]++;
}

static bool has_feature(const Map *m, hb_tag_t tag) {
    for (int t = 0; t < 2; ++t)
        if (hb_ot_layout_language_find_feature(m->face, table_tags[t], m->script_index[t], m->language_index[t], tag, NULL))
            return true;
    return false;
}

/* The first feature of the table with tag, whatever its script. */
static bool table_find_feature(hb_face_t *face, hb_tag_t table, hb_tag_t tag, unsigned *index) {
    hb_tag_t tags[64];
    unsigned offset = 0, count;
    do {
        count = 64;
        hb_ot_layout_table_get_feature_tags(face, table, offset, &count, tags);
        for (unsigned i = 0; i < count; ++i)
            if (tags[i] == tag) {
                *index = offset + i;
                return true;
            }
        offset += count;
    } while (count == 64);
    *index = NO_FEATURE;
    return false;
}

static int by_tag_seq(const void *a, const void *b) {
    const FeatureInfo *x = a, *y = b;
    return x->tag != y->tag ? (x->tag < y->tag ? -1 : 1) : (x->seq > y->seq) - (x->seq < y->seq);
}
static int by_tag(const void *a, const void *b) {
    const FeatureMap *x = a, *y = b;
    return (x->tag > y->tag) - (x->tag < y->tag);
}

static const FeatureMap *find_feature(const Map *m, hb_tag_t tag) {
    for (unsigned i = 0; i < m->feature_count; ++i)
        if (m->features[i].tag == tag) return &m->features[i];
    return NULL;
}
static uint32_t one_mask(const Map *m, const char *tag) {
    const FeatureMap *f = find_feature(m, TAG(tag));
    return f ? f->one_mask : 0;
}

static bool push_lookup(Map *m, int table, LookupMap l, unsigned *allocated) {
    if (m->lookup_count[table] == *allocated) {
        *allocated = *allocated ? 2 * *allocated : 64;
        LookupMap *grown = realloc(m->lookups[table], *allocated * sizeof(*grown));
        if (!grown) return false;
        m->lookups[table] = grown;
    }
    m->lookups[table][m->lookup_count[table]++] = l;
    return true;
}

static bool add_lookups(Map *m, int table, unsigned feature_index, uint32_t mask, bool auto_zwnj, bool auto_zwj,
                        bool random, bool per_syllable, unsigned *allocated) {
    unsigned table_lookup_count = hb_ot_layout_table_get_lookup_count(m->face, table_tags[table]);
    unsigned indices[32], offset = 0, len;
    do {
        len = 32;
        hb_ot_layout_feature_get_lookups(m->face, table_tags[table], feature_index, offset, &len, indices);
        for (unsigned i = 0; i < len; ++i)
            if (indices[i] < table_lookup_count &&
                !push_lookup(m, table, (LookupMap){indices[i], mask, auto_zwnj, auto_zwj, random, per_syllable, 0},
                             allocated))
                return false;
        offset += len;
    } while (len == 32);
    return true;
}

/* hb_qsort as HarfBuzz builds it (hb-algs.hh's sort_r_simple, Isaac
 * Turner's quicksort), by lookup index: which of two entries of one lookup
 * comes first decides whose per-syllable and random flags the merge keeps,
 * so the order must be its own, unstable as it is. */
static void swap_lookups(LookupMap *a, LookupMap *b) {
    LookupMap t = *a;
    *a = *b;
    *b = t;
}
static int cmp_lookups(const LookupMap *a, const LookupMap *b) { return (a->index > b->index) - (a->index < b->index); }
static void swap_blocks(LookupMap *p, size_t na, size_t nb) {
    if (!na || !nb) return;
    if (na > nb)
        for (size_t i = 0; i < nb; ++i) swap_lookups(&p[i], &p[na + i]);
    else
        for (size_t i = 0; i < na; ++i) swap_lookups(&p[i], &p[nb + i]);
}
static void sort_lookups(LookupMap *b, size_t nel) {
    if (nel < 10) {
        for (size_t i = 1; i < nel; ++i)
            for (size_t j = i; j > 0 && cmp_lookups(&b[j - 1], &b[j]) > 0; --j) swap_lookups(&b[j - 1], &b[j]);
        return;
    }
    LookupMap *end = b + nel, *last = b + nel - 1, *t;
    /* the median of the second, middle and second-last is the pivot, moved last */
    LookupMap *l[3] = {b + 1, b + nel / 2, last - 1};
    if (cmp_lookups(l[0], l[1]) > 0) t = l[0], l[0] = l[1], l[1] = t;
    if (cmp_lookups(l[1], l[2]) > 0) {
        t = l[1], l[1] = l[2], l[2] = t;
        if (cmp_lookups(l[0], l[1]) > 0) t = l[0], l[0] = l[1], l[1] = t;
    }
    if (l[1] != last) swap_lookups(l[1], last);
    LookupMap *pivot = last, *pl = b, *ple = b, *pr = last, *pre = last;
    while (pl < pr) {
        for (; pl < pr; pl++) {
            int c = cmp_lookups(pl, pivot);
            if (c > 0) break;
            if (c == 0) {
                if (ple < pl) swap_lookups(ple, pl);
                ple++;
            }
        }
        if (pl >= pr) break;
        while (pl < pr) {
            pr--;
            int c = cmp_lookups(pr, pivot);
            if (c == 0) {
                pre--;
                if (pr < pre) swap_lookups(pr, pre);
            } else if (c < 0) {
                if (pl < pr) swap_lookups(pl, pr);
                pl++;
                break;
            }
        }
    }
    pl = pr;
    swap_blocks(b, (size_t)(ple - b), (size_t)(pl - ple));
    swap_blocks(pr, (size_t)(pre - pr), (size_t)(end - pre));
    sort_lookups(b, (size_t)(pl - ple));
    sort_lookups(end - (pre - pr), (size_t)(pre - pr));
}

static bool compile_map(Map *m) {
    unsigned required_index[2], required_stage[2] = {0, 0};
    hb_tag_t required_tag[2];
    for (int t = 0; t < 2; ++t)
        if (!hb_ot_layout_language_get_required_feature(m->face, table_tags[t], m->script_index[t], m->language_index[t],
                                                        &required_index[t], &required_tag[t]))
            required_index[t] = NO_FEATURE, required_tag[t] = 0;
    /* Sort features and merge duplicates. */
    if (m->info_count) {
        if (!m->is_simple) qsort(m->infos, m->info_count, sizeof(*m->infos), by_tag_seq);
        FeatureInfo *f = m->infos;
        unsigned j = 0;
        for (unsigned i = 1; i < m->info_count; ++i)
            if (f[i].tag != f[j].tag) f[++j] = f[i];
            else {
                if (f[i].flags & F_GLOBAL) {
                    f[j].flags |= F_GLOBAL;
                    f[j].max_value = f[i].max_value;
                    f[j].default_value = f[i].default_value;
                } else {
                    if (f[j].flags & F_GLOBAL) f[j].flags ^= F_GLOBAL;
                    if (f[i].max_value > f[j].max_value) f[j].max_value = f[i].max_value;
                }
                f[j].flags |= f[i].flags & F_HAS_FALLBACK;
                for (int t = 0; t < 2; ++t)
                    if (f[i].stage[t] < f[j].stage[t]) f[j].stage[t] = f[i].stage[t];
            }
        m->info_count = j + 1;
    }
    /* Allocate bits. */
    m->global_mask = GLOBAL_BIT_MASK;
    unsigned next_bit = 3 + 1; /* after the glyph flags HarfBuzz keeps in the mask */
    for (unsigned i = 0; i < m->info_count; ++i) {
        const FeatureInfo *info = &m->infos[i];
        unsigned bits_needed = 0;
        if (!((info->flags & F_GLOBAL) && info->max_value == 1)) {
            unsigned storage = 0;
            for (unsigned v = info->max_value; v; v >>= 1) storage++;
            bits_needed = storage < MAX_BITS ? storage : MAX_BITS;
        }
        if (!info->max_value || next_bit + bits_needed >= GLOBAL_BIT_SHIFT) continue;
        bool found = false;
        unsigned index[2];
        for (int t = 0; t < 2; ++t) {
            if (required_tag[t] == info->tag) required_stage[t] = info->stage[t];
            if (hb_ot_layout_language_find_feature(m->face, table_tags[t], m->script_index[t], m->language_index[t],
                                                   info->tag, &index[t]))
                found = true;
            else
                index[t] = NO_FEATURE;
        }
        if (!found && (info->flags & F_GLOBAL_SEARCH))
            for (int t = 0; t < 2; ++t)
                found |= table_find_feature(m->face, table_tags[t], info->tag, &index[t]);
        if (!found && !(info->flags & F_HAS_FALLBACK)) continue;
        FeatureMap *map = &m->features[m->feature_count++];
        *map = (FeatureMap){info->tag, {index[0], index[1]}, {info->stage[0], info->stage[1]}, 0, 0, 0,
                            !(info->flags & F_MANUAL_ZWNJ), !(info->flags & F_MANUAL_ZWJ), info->flags & F_RANDOM,
                            info->flags & F_PER_SYLLABLE, !found};
        if ((info->flags & F_GLOBAL) && info->max_value == 1) {
            map->shift = GLOBAL_BIT_SHIFT;
            map->mask = GLOBAL_BIT_MASK;
        } else {
            map->shift = next_bit;
            map->mask = (1u << (next_bit + bits_needed)) - (1u << next_bit);
            next_bit += bits_needed;
            m->global_mask |= (info->default_value << map->shift) & map->mask;
        }
        map->one_mask = (1u << map->shift) & map->mask;
    }
    if (m->is_simple) qsort(m->features, m->feature_count, sizeof(*m->features), by_tag);
    /* The final pause, then each stage's lookups: sorted, duplicates merged,
     * the stage's pause after them. */
    if (++m->current_stage[0] > MAX_STAGES || ++m->current_stage[1] > MAX_STAGES) return false;
    for (int t = 0; t < 2; ++t) {
        unsigned allocated = 0, last = 0;
        for (unsigned stage = 0; stage < m->current_stage[t]; ++stage) {
            if (required_index[t] != NO_FEATURE && required_stage[t] == stage &&
                !add_lookups(m, t, required_index[t], GLOBAL_BIT_MASK, true, true, false, false, &allocated))
                return false;
            for (unsigned i = 0; i < m->feature_count; ++i) {
                const FeatureMap *f = &m->features[i];
                if (f->stage[t] == stage &&
                    !add_lookups(m, t, f->index[t], f->mask, f->auto_zwnj, f->auto_zwj, f->random, f->per_syllable,
                                 &allocated))
                    return false;
            }
            LookupMap *l = m->lookups[t];
            unsigned n = m->lookup_count[t];
            if (last + 1 < n) {
                sort_lookups(l + last, n - last);
                unsigned j = last;
                for (unsigned i = j + 1; i < n; ++i)
                    if (l[i].index != l[j].index) l[++j] = l[i];
                    else {
                        l[j].mask |= l[i].mask;
                        l[j].auto_zwnj &= l[i].auto_zwnj;
                        l[j].auto_zwj &= l[i].auto_zwj;
                    }
                m->lookup_count[t] = j + 1;
            }
            m->stage_first[t][stage] = last;
            m->stage_end[t][stage] = m->lookup_count[t];
            if (m->pause[t][stage] &&
                !push_lookup(m, t, (LookupMap){VEGA_STEP_PAUSE, 0, false, false, false, false, m->pause[t][stage]},
                             &allocated))
                return false;
            last = m->lookup_count[t];
        }
    }
    return true;
}

/* The scripts HarfBuzz gives the Universal Shaping Engine, or the default
 * shaper when the font has no script of their own. */
static const char *const use_scripts[] = {
    "Tibt", "Mong", "Sinh", "Buhd", "Hano", "Tglg", "Tagb", "Limb", "Tale", "Bugi", "Khar", "Sylo", "Tfng", "Bali",
    "Nkoo", "Phag", "Cham", "Kali", "Lepc", "Rjng", "Saur", "Sund", "Egyp", "Java", "Kthi", "Mtei", "Lana", "Tavt",
    "Batk", "Brah", "Mand", "Cakm", "Plrd", "Shrd", "Takr", "Dupl", "Gran", "Khoj", "Sind", "Mahj", "Mani", "Modi",
    "Hmng", "Phlp", "Sidd", "Tirh", "Ahom", "Mult", "Adlm", "Bhks", "Marc", "Newa", "Gonm", "Soyo", "Zanb", "Dogr",
    "Gong", "Rohg", "Maka", "Medf", "Sogo", "Sogd", "Elym", "Nand", "Hmnp", "Wcho", "Chrs", "Diak", "Kits", "Yezi",
    "Cpmn", "Ougr", "Tnsa", "Toto", "Vith", "Kawi", "Nagm", "Gara", "Gukh", "Krai", "Onao", "Sunu", "Todr", "Tutg"};
static const char *const indic_scripts[] = {"Beng", "Deva", "Gujr", "Guru", "Knda", "Mlym", "Orya", "Taml", "Telu"};

static bool listed(const char *const *list, size_t count, uint32_t script) {
    for (size_t i = 0; i < count; ++i)
        if (TAG(list[i]) == script) return true;
    return false;
}

/* hb_ot_shaper_categorize, for the shapers the runtime has; -1 for others. */
static int categorize(uint32_t script, hb_tag_t gsub_script) {
    bool dflt = gsub_script == HB_OT_TAG_DEFAULT_SCRIPT || gsub_script == TAG("latn");
    if (script == TAG("Arab")) return VEGA_SHAPER_ARABIC;
    if (script == TAG("Hebr")) return VEGA_SHAPER_HEBREW;
    if (script == TAG("Hang")) return VEGA_SHAPER_HANGUL;
    if (script == TAG("Khmr")) return VEGA_SHAPER_KHMER;
    if (script == TAG("Thai") || script == TAG("Laoo")) return VEGA_SHAPER_THAI;
    /* Syriac is Arabic's, unless the font has only DFLT for it. */
    if (script == TAG("Syrc")) return gsub_script != HB_OT_TAG_DEFAULT_SCRIPT ? VEGA_SHAPER_ARABIC : VEGA_SHAPER_DEFAULT;
    if (script == TAG("Qaag")) return -1;
    if (listed(indic_scripts, sizeof(indic_scripts) / sizeof(*indic_scripts), script))
        return dflt ? VEGA_SHAPER_DEFAULT : (gsub_script & 0xFF) == '3' ? VEGA_SHAPER_USE : VEGA_SHAPER_INDIC;
    if (listed(use_scripts, sizeof(use_scripts) / sizeof(*use_scripts), script))
        return dflt ? VEGA_SHAPER_DEFAULT : VEGA_SHAPER_USE;
    if (script == TAG("Mymr")) return dflt || gsub_script == TAG("mymr") ? VEGA_SHAPER_DEFAULT : VEGA_SHAPER_MYANMAR;
    return VEGA_SHAPER_DEFAULT;
}

static bool has_table(hb_face_t *face, hb_tag_t tag) {
    hb_blob_t *blob = hb_face_reference_table(face, tag);
    bool has = hb_blob_get_length(blob) > 0;
    hb_blob_destroy(blob);
    return has;
}

typedef struct { unsigned first, count; } Stage;
typedef struct {
    uint32_t script;
    int direction, native, shaper, flags;
    uint32_t global_mask, kern_mask, masks[VEGA_PLAN_MASKS];
    LookupMap *lookups[2];
    unsigned lookup_count[2];
    unsigned shaper_flags;
    Stage stages[VEGA_PLAN_STAGES];
} Plan;

/* The GSUB stage a feature is in (hb_ot_map_t::get_stage_lookups), for a
 * shaper's would-substitute questions. */
static Stage stage_of(const Map *m, const char *tag) {
    const FeatureMap *f = find_feature(m, TAG(tag));
    if (!f || f->stage[0] >= m->current_stage[0]) return (Stage){0, 0};
    unsigned first = m->stage_first[0][f->stage[0]];
    return (Stage){first, m->stage_end[0][f->stage[0]] - first};
}

/* What HarfBuzz makes of the font's kern table (hb-ot-kern-table.hh): one
 * with subtables that pass its checks (data), Apple's state machines among
 * them (machine), cross-stream kerning (cross); and whether it has what
 * Vega does not apply (other: a format but pairs, a version but 0 and 1). */
typedef struct { bool data, machine, cross, other; } Kerning;

static Kerning kerning_of(hb_face_t *face) {
    Kerning k = {false, false, false, false};
    hb_blob_t *blob = hb_face_reference_table(face, HB_TAG('k', 'e', 'r', 'n'));
    unsigned length;
    const unsigned char *p = (const unsigned char *)hb_blob_get_data(blob, &length);
    #define U16(at) ((uint64_t)(at) + 2 <= length ? (uint32_t)p[at] << 8 | p[(at) + 1] : 0)
    #define U32(at) ((uint64_t)(at) + 4 <= length ? U16(at) << 16 | U16((at) + 2) : 0)
    uint32_t major = U16(0);
    k.data = U32(0) != 0;
    k.other = major > 1;
    bool apple = major == 1, valid = length >= (apple ? 8u : 4u);
    uint32_t count = apple ? U32(4) : U16(2), header = apple ? 8 : 6;
    uint64_t at = apple ? 8 : 4;
    for (uint32_t i = 0; k.data && !k.other && valid && i < count; ++i) {
        /* each subtable within the table (the last one's pairs may run past
         * the length it gives: HarfBuzz lets them, for fonts whose 16 bits
         * overflowed) */
        valid = at + header <= length;
        if (!valid) break;
        uint32_t size = apple ? U32(at) : U16(at + 2);
        unsigned format = p[at + (apple ? 5 : 4)], coverage = p[at + (apple ? 4 : 5)];
        valid = size >= header && at + size <= length;
        uint64_t end = i + 1 < count ? at + size : length;
        if (valid && format == 0) valid = at + header + 8 + 6 * (uint64_t)U16(at + header) <= end;
        k.machine |= format == 1;
        k.other |= format != 0;
        k.cross |= apple ? coverage & 0x40 : coverage & 4;
        at += size;
    }
    #undef U16
    #undef U32
    if (!valid) k = (Kerning){false, false, false, false}; /* HarfBuzz drops a table that fails its checks */
    hb_blob_destroy(blob);
    return k;
}

#define FAIL(...)                                    \
    do {                                             \
        snprintf(error, error_size, __VA_ARGS__);    \
        goto fail;                                   \
    } while (0)

static bool plan_for(hb_face_t *face, uint32_t script_tag, int direction, Plan *plan, char *error, size_t error_size) {
    Map *m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    m->face = face;
    hb_script_t script = hb_script_from_iso15924_tag(script_tag);
    char name[5] = {(char)(script_tag >> 24), (char)(script_tag >> 16), (char)(script_tag >> 8), (char)script_tag, 0};
    unsigned script_count = HB_OT_MAX_TAGS_PER_SCRIPT, language_count = HB_OT_MAX_TAGS_PER_LANGUAGE;
    hb_tag_t script_tags[HB_OT_MAX_TAGS_PER_SCRIPT], language_tags[HB_OT_MAX_TAGS_PER_LANGUAGE];
    hb_ot_tags_from_script_and_language(script, HB_LANGUAGE_INVALID, &script_count, script_tags, &language_count,
                                        language_tags);
    for (int t = 0; t < 2; ++t) {
        m->found_script[t] = hb_ot_layout_table_select_script(face, table_tags[t], script_count, script_tags,
                                                              &m->script_index[t], &m->chosen_script[t]);
        hb_ot_layout_script_select_language(face, table_tags[t], m->script_index[t], language_count, language_tags,
                                            &m->language_index[t]);
    }
    int shaper = categorize(script_tag, m->chosen_script[0]);
    Kerning kern_table = kerning_of(face);
    if (shaper < 0) FAIL("%s: HarfBuzz shapes it with a shaper Vega does not have", name);
    if (has_table(face, HB_TAG('m', 'o', 'r', 'x')) || has_table(face, HB_TAG('k', 'e', 'r', 'x')) ||
        has_table(face, HB_TAG('t', 'r', 'a', 'k')))
        FAIL("%s: the font has AAT tables (morx, kerx, trak)", name);

    /* hb_ot_shape_collect_features */
    m->is_simple = true;
    enable_feature(m, TAG("rvrn"), F_NONE, 1);
    add_gsub_pause(m, 0);
    if (direction == VEGA_SHAPE_LTR) {
        enable_feature(m, TAG("ltra"), F_NONE, 1);
        enable_feature(m, TAG("ltrm"), F_NONE, 1);
    } else {
        enable_feature(m, TAG("rtla"), F_NONE, 1);
        add_feature(m, TAG("rtlm"), F_NONE, 1);
    }
    add_feature(m, TAG("frac"), F_NONE, 1);
    add_feature(m, TAG("numr"), F_NONE, 1);
    add_feature(m, TAG("dnom"), F_NONE, 1);
    enable_feature(m, TAG("rand"), F_RANDOM, MAX_VALUE);
    enable_feature(m, TAG("trak"), F_HAS_FALLBACK, 1);
    enable_feature(m, TAG("Harf"), F_NONE, 1);
    enable_feature(m, TAG("HARF"), F_NONE, 1);
    static const char *const arabic_features[7] = {"isol", "fina", "fin2", "fin3", "medi", "med2", "init"};
    if (shaper == VEGA_SHAPER_ARABIC) {
        m->is_simple = false;
        enable_feature(m, TAG("stch"), F_NONE, 1);
        add_gsub_pause(m, VEGA_PAUSE_RECORD_STCH);
        enable_feature(m, TAG("ccmp"), F_MANUAL_ZWJ, 1);
        enable_feature(m, TAG("locl"), F_MANUAL_ZWJ, 1);
        add_gsub_pause(m, 0);
        for (int i = 0; i < 7; ++i) {
            bool syriac = arabic_features[i][3] == '2' || arabic_features[i][3] == '3';
            add_feature(m, TAG(arabic_features[i]), F_MANUAL_ZWJ | (script == HB_SCRIPT_ARABIC && !syriac ? F_HAS_FALLBACK : 0), 1);
            add_gsub_pause(m, 0);
        }
        enable_feature(m, TAG("rlig"), F_MANUAL_ZWJ | F_HAS_FALLBACK, 1);
        if (script == HB_SCRIPT_ARABIC) add_gsub_pause(m, VEGA_PAUSE_ARABIC_FALLBACK);
        enable_feature(m, TAG("calt"), F_MANUAL_ZWJ, 1);
        if (!has_feature(m, TAG("rclt"))) add_gsub_pause(m, 0);
        enable_feature(m, TAG("liga"), F_MANUAL_ZWJ, 1);
        enable_feature(m, TAG("clig"), F_MANUAL_ZWJ, 1);
        enable_feature(m, TAG("mset"), F_MANUAL_ZWJ, 1);
    }
    /* hb-ot-shaper-indic.cc: syllables found before any lookup; locl and
     * ccmp; the initial reordering; the basic features one stage each; the
     * final reordering; the rest. */
    static const struct { const char *tag; unsigned flags; } indic_features[17] = {
        {"nukt", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"akhn", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"rphf", F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"rkrf", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"pref", F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"blwf", F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"abvf", F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"half", F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"pstf", F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"vatu", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"cjct", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"init", F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"pres", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"abvs", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"blws", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"psts", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE},
        {"haln", F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE}};
    if (shaper == VEGA_SHAPER_INDIC) {
        m->is_simple = false;
        add_gsub_pause(m, VEGA_PAUSE_SYLLABLES);
        enable_feature(m, TAG("locl"), F_PER_SYLLABLE, 1);
        enable_feature(m, TAG("ccmp"), F_PER_SYLLABLE, 1);
        add_gsub_pause(m, VEGA_PAUSE_INITIAL_REORDERING);
        for (int i = 0; i < 11; ++i) {
            add_feature(m, TAG(indic_features[i].tag), indic_features[i].flags, 1);
            add_gsub_pause(m, 0);
        }
        add_gsub_pause(m, VEGA_PAUSE_FINAL_REORDERING);
        for (int i = 11; i < 17; ++i) add_feature(m, TAG(indic_features[i].tag), indic_features[i].flags, 1);
    }
    /* hb-ot-shaper-khmer.cc: syllables and their reordering before any
     * lookup; the basic features together, within syllables; the rest. */
    static const char *const khmer_basic[] = {"pref", "blwf", "abvf", "pstf", "cfar"};
    static const char *const khmer_other[] = {"pres", "abvs", "blws", "psts"};
    if (shaper == VEGA_SHAPER_KHMER) {
        m->is_simple = false;
        add_gsub_pause(m, VEGA_PAUSE_SYLLABLES);
        add_gsub_pause(m, VEGA_PAUSE_INITIAL_REORDERING);
        enable_feature(m, TAG("locl"), F_PER_SYLLABLE, 1);
        enable_feature(m, TAG("ccmp"), F_PER_SYLLABLE, 1);
        for (int i = 0; i < 5; ++i) add_feature(m, TAG(khmer_basic[i]), F_MANUAL_ZWNJ | F_MANUAL_ZWJ | F_PER_SYLLABLE, 1);
        add_gsub_pause(m, VEGA_PAUSE_CLEAR_SYLLABLES);
        for (int i = 0; i < 4; ++i) add_feature(m, TAG(khmer_other[i]), F_GLOBAL | F_MANUAL_ZWNJ | F_MANUAL_ZWJ, 1);
    }
    /* hb-ot-shaper-myanmar.cc: syllables; locl and ccmp; the reordering;
     * the basic features one stage each; the rest. */
    static const char *const myanmar_basic[] = {"rphf", "pref", "blwf", "pstf"};
    static const char *const myanmar_other[] = {"pres", "abvs", "blws", "psts"};
    if (shaper == VEGA_SHAPER_MYANMAR) {
        m->is_simple = false;
        add_gsub_pause(m, VEGA_PAUSE_SYLLABLES);
        enable_feature(m, TAG("locl"), F_PER_SYLLABLE, 1);
        enable_feature(m, TAG("ccmp"), F_PER_SYLLABLE, 1);
        add_gsub_pause(m, VEGA_PAUSE_INITIAL_REORDERING);
        for (int i = 0; i < 4; ++i) {
            enable_feature(m, TAG(myanmar_basic[i]), F_MANUAL_ZWJ | F_PER_SYLLABLE, 1);
            add_gsub_pause(m, 0);
        }
        add_gsub_pause(m, VEGA_PAUSE_CLEAR_SYLLABLES);
        for (int i = 0; i < 4; ++i) enable_feature(m, TAG(myanmar_other[i]), F_MANUAL_ZWJ, 1);
    }
    /* hb-ot-shaper-use.cc: syllables; the default glyph pre-processing;
     * rphf and pref, each recording what it substituted; the basic
     * features; the reordering; the topographical forms; the rest. */
    static const char *const use_basic[] = {"rkrf", "abvf", "blwf", "half", "pstf", "vatu", "cjct"};
    static const char *const use_topographical[] = {"isol", "init", "medi", "fina"};
    static const char *const use_other[] = {"abvs", "blws", "haln", "pres", "psts"};
    if (shaper == VEGA_SHAPER_USE) {
        m->is_simple = false;
        add_gsub_pause(m, VEGA_PAUSE_SYLLABLES);
        enable_feature(m, TAG("locl"), F_PER_SYLLABLE, 1);
        enable_feature(m, TAG("ccmp"), F_PER_SYLLABLE, 1);
        enable_feature(m, TAG("nukt"), F_PER_SYLLABLE, 1);
        enable_feature(m, TAG("akhn"), F_MANUAL_ZWJ | F_PER_SYLLABLE, 1);
        add_gsub_pause(m, VEGA_PAUSE_CLEAR_SUBSTITUTED);
        add_feature(m, TAG("rphf"), F_MANUAL_ZWJ | F_PER_SYLLABLE, 1);
        add_gsub_pause(m, VEGA_PAUSE_RECORD_RPHF);
        add_gsub_pause(m, VEGA_PAUSE_CLEAR_SUBSTITUTED);
        enable_feature(m, TAG("pref"), F_MANUAL_ZWJ | F_PER_SYLLABLE, 1);
        add_gsub_pause(m, VEGA_PAUSE_RECORD_PREF);
        for (int i = 0; i < 7; ++i) enable_feature(m, TAG(use_basic[i]), F_MANUAL_ZWJ | F_PER_SYLLABLE, 1);
        add_gsub_pause(m, VEGA_PAUSE_INITIAL_REORDERING);
        add_gsub_pause(m, VEGA_PAUSE_CLEAR_SYLLABLES);
        for (int i = 0; i < 4; ++i) add_feature(m, TAG(use_topographical[i]), F_NONE, 1);
        add_gsub_pause(m, 0);
        for (int i = 0; i < 5; ++i) enable_feature(m, TAG(use_other[i]), F_MANUAL_ZWJ, 1);
    }
    if (shaper == VEGA_SHAPER_HANGUL) {
        m->is_simple = false;
        add_feature(m, TAG("ljmo"), F_NONE, 1);
        add_feature(m, TAG("vjmo"), F_NONE, 1);
        add_feature(m, TAG("tjmo"), F_NONE, 1);
    }
    enable_feature(m, TAG("Buzz"), F_NONE, 1);
    enable_feature(m, TAG("BUZZ"), F_NONE, 1);
    static const char *const common[] = {"abvm", "blwm", "ccmp", "locl", "mark", "mkmk", "rlig"};
    for (int i = 0; i < 7; ++i)
        add_feature(m, TAG(common[i]), F_GLOBAL | (i == 4 || i == 5 ? F_MANUAL_ZWNJ | F_MANUAL_ZWJ : 0), 1);
    static const char *const horizontal[] = {"calt", "clig", "curs", "dist", "kern", "liga", "rclt"};
    for (int i = 0; i < 7; ++i) add_feature(m, TAG(horizontal[i]), F_GLOBAL | (i == 4 ? F_HAS_FALLBACK : 0), 1);
    /* Hangul turns calt off: fonts put jamo lookups there. Indic turns liga
     * off, and is done with syllables after GSUB. */
    if (shaper == VEGA_SHAPER_HANGUL) add_feature(m, TAG("calt"), F_GLOBAL, 0);
    if (shaper == VEGA_SHAPER_INDIC) {
        add_feature(m, TAG("liga"), F_GLOBAL, 0);
        add_gsub_pause(m, VEGA_PAUSE_CLEAR_SYLLABLES);
    }
    /* Khmer asks for clig, and turns liga off. */
    if (shaper == VEGA_SHAPER_KHMER) {
        enable_feature(m, TAG("clig"), F_NONE, 1);
        add_feature(m, TAG("liga"), F_GLOBAL, 0);
    }
    if (!compile_map(m)) FAIL("out of memory, or more than %d stages", MAX_STAGES);

    /* hb_ot_shape_planner_t::compile */
    const FeatureMap *kern = find_feature(m, TAG("kern"));
    bool has_gpos_kern = kern && kern->index[1] != NO_FEATURE;
    bool disable_gpos = shaper == VEGA_SHAPER_HEBREW && m->chosen_script[1] != TAG("hebr");
    bool apply_gpos = !disable_gpos && hb_ot_layout_has_positioning(face);
    bool apply_kern = (!has_gpos_kern || !apply_gpos) && kern_table.data;
    if (apply_kern && (kern_table.other || kern_table.cross))
        FAIL("%s: the font's kern table has more than pairs (state machines, classes, cross-stream kerning)", name);
    /* Hangul's, Indic's and Khmer's shapers zero no marks (Myanmar's does,
     * before GPOS); none of these places marks without GPOS. */
    bool zeroes = shaper != VEGA_SHAPER_HANGUL && shaper != VEGA_SHAPER_INDIC && shaper != VEGA_SHAPER_KHMER;
    bool script_marks = zeroes && shaper != VEGA_SHAPER_MYANMAR && shaper != VEGA_SHAPER_USE && shaper != VEGA_SHAPER_THAI;
    bool adjust = !apply_gpos && (!apply_kern || !kern_table.cross), fallback_marks = adjust && script_marks;
    bool arabic_fallback = false;
    if (shaper == VEGA_SHAPER_ARABIC) {
        bool fallback = script == HB_SCRIPT_ARABIC;
        for (int i = 0; i < 7; ++i) {
            const FeatureMap *f = find_feature(m, TAG(arabic_features[i]));
            bool syriac = arabic_features[i][3] == '2' || arabic_features[i][3] == '3';
            fallback = fallback && (syriac || (f && f->needs_fallback));
        }
        arabic_fallback = fallback;
    }
    hb_direction_t native = hb_script_get_horizontal_direction(script);
    uint32_t frac = one_mask(m, "frac"), numr = one_mask(m, "numr"), dnom = one_mask(m, "dnom");
    *plan = (Plan){script_tag, direction, native == HB_DIRECTION_LTR ? VEGA_SHAPE_LTR : native == HB_DIRECTION_RTL ? VEGA_SHAPE_RTL : 0,
                   shaper,
                   (apply_gpos ? VEGA_PLAN_GPOS : 0) | (zeroes ? VEGA_PLAN_ZERO_MARKS : 0) |
                       (hb_ot_layout_has_glyph_classes(face) ? 0 : VEGA_PLAN_GUESS_CLASSES) |
                       (one_mask(m, "mark") ? VEGA_PLAN_GPOS_MARK : 0) | (frac || (numr && dnom) ? VEGA_PLAN_FRACTIONS : 0) |
                       (apply_kern ? VEGA_PLAN_KERN : 0) | (fallback_marks ? VEGA_PLAN_FALLBACK_MARKS : 0) |
                       (adjust ? VEGA_PLAN_ADJUST_ZEROING : 0),
                   m->global_mask, kern ? kern->one_mask : 0, {0}, {m->lookups[0], m->lookups[1]},
                   {m->lookup_count[0], m->lookup_count[1]}, 0, {{0, 0}}};
    plan->masks[VEGA_MASK_RTLM] = one_mask(m, "rtlm");
    plan->masks[VEGA_MASK_FRAC] = frac;
    plan->masks[VEGA_MASK_NUMR] = numr;
    plan->masks[VEGA_MASK_DNOM] = dnom;
    if (shaper == VEGA_SHAPER_ARABIC || shaper == VEGA_SHAPER_USE)
        for (int i = 0; i < 7; ++i) plan->masks[VEGA_MASK_ISOL + i] = one_mask(m, arabic_features[i]);
    if (shaper == VEGA_SHAPER_ARABIC) plan->masks[VEGA_MASK_ARABIC_RLIG] = one_mask(m, "rlig");
    if (arabic_fallback) plan->shaper_flags |= VEGA_ARABIC_FALLBACK;
    if (shaper == VEGA_SHAPER_ARABIC && one_mask(m, "stch")) plan->shaper_flags |= VEGA_ARABIC_STCH;
    if (shaper == VEGA_SHAPER_USE) plan->masks[VEGA_MASK_USE_RPHF] = one_mask(m, "rphf");
    if (shaper == VEGA_SHAPER_HANGUL) {
        plan->masks[VEGA_MASK_LJMO] = one_mask(m, "ljmo");
        plan->masks[VEGA_MASK_VJMO] = one_mask(m, "vjmo");
        plan->masks[VEGA_MASK_TJMO] = one_mask(m, "tjmo");
    }
    if (shaper == VEGA_SHAPER_INDIC) {
        /* the masks of the features set glyph by glyph (not global ones) */
        static const char *const masked[] = {"rphf", "pref", "blwf", "abvf", "half", "pstf", "init"};
        for (int i = 0; i < 7; ++i) plan->masks[VEGA_MASK_RPHF + i] = one_mask(m, masked[i]);
        static const char *const staged[VEGA_PLAN_STAGES] = {"rphf", "pref", "blwf", "pstf", "vatu"};
        for (int i = 0; i < VEGA_PLAN_STAGES; ++i) plan->stages[i] = stage_of(m, staged[i]);
        if ((m->chosen_script[0] & 0xFF) != '2') plan->shaper_flags |= VEGA_INDIC_OLD_SPEC;
    }
    if (shaper == VEGA_SHAPER_KHMER)
        for (int i = 0; i < 5; ++i) plan->masks[VEGA_MASK_KHMER_PREF + i] = one_mask(m, khmer_basic[i]);
    if (shaper == VEGA_SHAPER_THAI && script == HB_SCRIPT_THAI && !m->found_script[0])
        plan->shaper_flags |= VEGA_THAI_PUA;
    free(m);
    return true;
fail:
    free(m->lookups[0]);
    free(m->lookups[1]);
    free(m);
    return false;
}

/* ---- The section */

typedef struct { unsigned char *bytes; size_t size, allocated; bool failed; } Out;

static void put(Out *o, const void *bytes, size_t n) {
    if (o->failed) return;
    if (o->size + n > o->allocated) {
        size_t a = o->allocated ? o->allocated : 4096;
        while (a < o->size + n) a *= 2;
        unsigned char *grown = realloc(o->bytes, a);
        if (!grown) {
            o->failed = true;
            return;
        }
        o->bytes = grown;
        o->allocated = a;
    }
    memcpy(o->bytes + o->size, bytes, n);
    o->size += n;
}
static void put32(Out *o, uint32_t v) {
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    put(o, b, 4);
}
static void put16(Out *o, uint32_t v) {
    unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    put(o, b, 2);
}
static void align4(Out *o) {
    static const unsigned char zero[4] = {0};
    put(o, zero, (4 - o->size % 4) % 4);
}
static void set32(Out *o, size_t at, uint32_t v) {
    if (o->failed) return;
    for (int i = 0; i < 4; ++i) o->bytes[at + i] = (unsigned char)(v >> (8 * i));
}

/* ---- Arabic fallback shaping (hb-ot-shaper-arabic-fallback.hh): for a font
 * with presentation forms but no Arabic features, lookups made from its
 * character map, written as OpenType (the runtime applies them as any
 * GSUB lookup): single substitutions to the init, medi, fina and isol
 * forms, and rlig's ligatures of three, of two, and of marks. */

typedef struct { unsigned char *bytes; size_t size, allocated; bool failed; } Bytes;
static void be16(Bytes *b, uint32_t v) {
    if (b->failed) return;
    if (b->size + 2 > b->allocated) {
        size_t a = b->allocated ? 2 * b->allocated : 256;
        unsigned char *grown = realloc(b->bytes, a);
        if (!grown) {
            b->failed = true;
            return;
        }
        b->bytes = grown;
        b->allocated = a;
    }
    b->bytes[b->size++] = (unsigned char)(v >> 8);
    b->bytes[b->size++] = (unsigned char)v;
}
static void be16_at(Bytes *b, size_t at, uint32_t v) {
    if (b->failed) return;
    b->bytes[at] = (unsigned char)(v >> 8);
    b->bytes[at + 1] = (unsigned char)v;
}

static bool glyph_of(hb_font_t *font, uint32_t u, uint32_t *glyph) {
    hb_codepoint_t g;
    if (!u || !hb_font_get_nominal_glyph(font, u, &g)) return false;
    *glyph = g;
    return true;
}

/* Glyphs sorted stably, their partners carried along; false if two are
 * the same (the font maps two characters to one glyph). */
static bool sort_pairs(uint32_t *keys, uint32_t *values, size_t n) {
    for (size_t i = 1; i < n; ++i)
        for (size_t j = i; j > 0 && keys[j - 1] > keys[j]; --j) {
            uint32_t k = keys[j], v = values[j];
            keys[j] = keys[j - 1], values[j] = values[j - 1];
            keys[j - 1] = k, values[j - 1] = v;
        }
    for (size_t i = 1; i < n; ++i)
        if (keys[i] == keys[i - 1]) return false;
    return true;
}

/* A lookup (type, flags, one subtable) at the end of b; the subtable's
 * start, for its offsets. */
static size_t start_lookup(Bytes *b, unsigned type, unsigned flags) {
    size_t at = b->size;
    be16(b, type);
    be16(b, flags);
    be16(b, 1);
    be16(b, 8); /* the subtable, right after */
    return at + 8;
}
static void coverage_of(Bytes *b, const uint32_t *glyphs, size_t n) {
    be16(b, 1);
    be16(b, (uint32_t)n);
    for (size_t i = 0; i < n; ++i) be16(b, glyphs[i]);
}

/* The lookup for fallback feature k into b; false with nothing written
 * when it would be empty, and *refused when the font cannot have it. */
static bool fallback_lookup(hb_font_t *font, int k, Bytes *b, bool *refused) {
    enum { FORMS = VEGA_ARABIC_FORMS_LAST - VEGA_ARABIC_FORMS_FIRST + 1 };
    if (k < 4) {
        uint32_t glyphs[FORMS], substitutes[FORMS];
        size_t n = 0;
        for (uint32_t u = VEGA_ARABIC_FORMS_FIRST; u <= VEGA_ARABIC_FORMS_LAST; ++u) {
            uint32_t g, s;
            if (!glyph_of(font, u, &g) || !glyph_of(font, vega_arabic_forms[u - VEGA_ARABIC_FORMS_FIRST][k], &s) || g == s)
                continue;
            glyphs[n] = g;
            substitutes[n++] = s;
        }
        if (!n) return false;
        if (!sort_pairs(glyphs, substitutes, n)) return !(*refused = true);
        size_t sub = start_lookup(b, 1, 0x0008);
        be16(b, 2);
        be16(b, (uint32_t)(6 + 2 * n)); /* the coverage, after the substitutes */
        be16(b, (uint32_t)n);
        for (size_t i = 0; i < n; ++i) be16(b, substitutes[i]);
        coverage_of(b, glyphs, n);
        (void)sub;
        return true;
    }
    const VegaArabicLigatures *table = &vega_arabic_ligatures[k - 4];
    /* its sets: runs of one first character */
    uint32_t firsts[64], sets[64], set_start[65];
    size_t n_sets = 0;
    for (unsigned i = 0; i < table->count; ++i)
        if (!i || table->ligatures[i].first != table->ligatures[i - 1].first) {
            if (n_sets == 64) return !(*refused = true);
            set_start[n_sets++] = i;
        }
    set_start[n_sets] = table->count;
    size_t n = 0;
    for (size_t i = 0; i < n_sets; ++i)
        if (glyph_of(font, table->ligatures[set_start[i]].first, &firsts[n])) sets[n++] = (uint32_t)i;
    if (!sort_pairs(firsts, sets, n)) return !(*refused = true);
    /* what each set keeps: ligatures whose glyph and components the font has */
    unsigned total = 0;
    for (size_t i = 0; i < n; ++i)
        for (uint32_t l = set_start[sets[i]]; l < set_start[sets[i] + 1]; ++l) {
            const VegaArabicLigature *lig = &table->ligatures[l];
            uint32_t g;
            bool ok = glyph_of(font, lig->ligature, &g);
            for (unsigned c = 0; ok && c + 1 < table->length; ++c) ok = glyph_of(font, lig->components[c], &g);
            total += ok;
        }
    if (!total) return false;
    size_t sub = start_lookup(b, 4, k == 6 ? 0 : 0x0008);
    be16(b, 1);
    size_t coverage_at = b->size;
    be16(b, 0);
    be16(b, (uint32_t)n);
    size_t offsets_at = b->size;
    for (size_t i = 0; i < n; ++i) be16(b, 0);
    for (size_t i = 0; i < n; ++i) {
        be16_at(b, offsets_at + 2 * i, (uint32_t)(b->size - sub));
        size_t set_at = b->size, count_at = b->size;
        be16(b, 0);
        unsigned count = 0;
        size_t lig_offsets = b->size;
        for (uint32_t l = set_start[sets[i]]; l < set_start[sets[i] + 1]; ++l) {
            const VegaArabicLigature *lig = &table->ligatures[l];
            uint32_t g;
            bool ok = glyph_of(font, lig->ligature, &g);
            for (unsigned c = 0; ok && c + 1 < table->length; ++c) ok = glyph_of(font, lig->components[c], &g);
            if (ok) be16(b, 0), count++;
        }
        be16_at(b, count_at, count);
        unsigned written = 0;
        for (uint32_t l = set_start[sets[i]]; l < set_start[sets[i] + 1]; ++l) {
            const VegaArabicLigature *lig = &table->ligatures[l];
            uint32_t g, components[2];
            bool ok = glyph_of(font, lig->ligature, &g);
            for (unsigned c = 0; ok && c + 1 < table->length; ++c) ok = glyph_of(font, lig->components[c], &components[c]);
            if (!ok) continue;
            be16_at(b, lig_offsets + 2 * written++, (uint32_t)(b->size - set_at));
            be16(b, g);
            be16(b, table->length);
            for (unsigned c = 0; c + 1 < table->length; ++c) be16(b, components[c]);
        }
    }
    be16_at(b, coverage_at, (uint32_t)(b->size - sub));
    coverage_of(b, firsts, n);
    return true;
}

/* The fallback lookups as a GSUB table (empty script and feature lists),
 * and what each lookup is for. */
static bool fallback_table(hb_font_t *font, Bytes *gsub, uint8_t *features, unsigned *count, char *error,
                           size_t error_size) {
    Bytes lookups[7] = {{0}};
    *count = 0;
    bool refused = false;
    for (int k = 0; k < 7 && !refused; ++k)
        if (fallback_lookup(font, k, &lookups[*count], &refused)) features[(*count)++] = (uint8_t)k;
    if (refused) {
        snprintf(error, error_size, "Arab: the font maps two characters to one glyph among those fallback shaping uses");
        for (int k = 0; k < 7; ++k) free(lookups[k].bytes);
        return false;
    }
    be16(gsub, 1);
    be16(gsub, 0);
    be16(gsub, 10); /* scripts */
    be16(gsub, 12); /* features */
    be16(gsub, 14); /* lookups */
    be16(gsub, 0);
    be16(gsub, 0);
    be16(gsub, *count);
    size_t list = 14, offsets = gsub->size;
    for (unsigned i = 0; i < *count; ++i) be16(gsub, 0);
    for (unsigned i = 0; i < *count; ++i) {
        be16_at(gsub, offsets + 2 * i, (uint32_t)(gsub->size - list));
        for (size_t k = 0; k < lookups[i].size; k += 2) be16(gsub, (uint32_t)lookups[i].bytes[k] << 8 | lookups[i].bytes[k + 1]);
    }
    bool failed = gsub->failed || gsub->size > 0xFFFF;
    for (int k = 0; k < 7; ++k) failed |= lookups[k].failed, free(lookups[k].bytes);
    if (failed) snprintf(error, error_size, "Arab: fallback lookups: out of memory or over 64 KiB");
    return !failed;
}

static int by_sequence(const void *a, const void *b) {
    const unsigned char *x = a, *y = b;
    for (int k = 0; k < 8; k += 4) {
        uint32_t p = (uint32_t)x[k] | (uint32_t)x[k + 1] << 8 | (uint32_t)x[k + 2] << 16 | (uint32_t)x[k + 3] << 24;
        uint32_t q = (uint32_t)y[k] | (uint32_t)y[k + 1] << 8 | (uint32_t)y[k + 2] << 16 | (uint32_t)y[k + 3] << 24;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

bool vega_shaping_compile(hb_face_t *face, const uint32_t *scripts, size_t script_count, const uint32_t *codes,
                          size_t code_count, VegaSection *out, hb_set_t *glyphs, char *error, size_t error_size) {
    Plan *plans = calloc(2 * script_count + 1, sizeof(*plans));
    size_t plan_count = 0;
    Out o = {0};
    hb_font_t *font = hb_font_create(face);
    hb_set_t *unicodes = hb_set_create();
    if (!plans) FAIL("out of memory");
    /* Both directions: a run's comes from bidi, and need not be its script's. */
    for (size_t i = 0; i < script_count; ++i)
        for (int d = VEGA_SHAPE_LTR; d <= VEGA_SHAPE_RTL; ++d) {
            if (!plan_for(face, scripts[i], d, &plans[plan_count], error, error_size)) goto fail;
            plan_count++;
        }
    unsigned glyph_count = hb_face_get_glyph_count(face);
    if (glyph_count > 65535) FAIL("more than 65535 glyphs");
    /* The character map as HarfBuzz maps, not as it lists (a format 13 map
     * it lists cut short, a symbol map it maps into the private use area):
     * every code point asked, in groups of codes going to consecutive
     * glyphs, or all to one. */
    uint32_t *groups = NULL, group_count = 0, allocated = 0;
    for (hb_codepoint_t u = 0; u <= 0x10FFFF; ++u) {
        hb_codepoint_t g;
        if (!hb_font_get_nominal_glyph(font, u, &g) || !g) continue;
        hb_set_add(unicodes, u);
        if (group_count) {
            uint32_t *last = groups + 3 * (group_count - 1), glyph = last[2] & 0x7FFFFFFF;
            bool single = last[0] == last[1], same = last[2] >> 31;
            if (last[1] + 1 == u && ((single || !same) && g == glyph + (u - last[0]))) {
                last[1] = u;
                continue;
            }
            if (last[1] + 1 == u && (single || same) && g == glyph) {
                last[1] = u;
                last[2] = glyph | 0x80000000u;
                continue;
            }
        }
        if (group_count == allocated) {
            allocated = allocated ? 2 * allocated : 256;
            uint32_t *grown = realloc(groups, 3 * (size_t)allocated * sizeof(*groups));
            if (!grown) {
                free(groups);
                FAIL("out of memory");
            }
            groups = grown;
        }
        uint32_t *next = groups + 3 * group_count++;
        next[0] = next[1] = u;
        next[2] = g;
    }
    hb_blob_t *tables[3] = {hb_face_reference_table(face, HB_OT_TAG_GDEF), hb_face_reference_table(face, HB_OT_TAG_GSUB),
                            hb_face_reference_table(face, HB_OT_TAG_GPOS)};
    /* The header, filled in as the parts land. */
    put(&o, "VSHP", 4);
    put32(&o, 0);
    put16(&o, hb_face_get_upem(face));
    put16(&o, glyph_count);
    for (int i = 0; i < 19; ++i) put32(&o, 0);
    set32(&o, 12, group_count);
    set32(&o, 16, (uint32_t)o.size);
    for (uint32_t i = 0; i < 3 * group_count; ++i) put32(&o, groups[i]);
    free(groups);
    /* Variation sequences (cmap format 14): each selector's bases, and the
     * glyph HarfBuzz gives the pair (a default one's is the base's own). */
    hb_set_t *selectors = hb_set_create(), *bases = hb_set_create();
    hb_face_collect_variation_selectors(face, selectors);
    uint32_t sequence_count = 0;
    size_t sequences_at = o.size;
    for (hb_codepoint_t v = HB_SET_VALUE_INVALID; hb_set_next(selectors, &v);) {
        hb_set_clear(bases);
        hb_face_collect_variation_unicodes(face, v, bases);
        for (hb_codepoint_t u = HB_SET_VALUE_INVALID; hb_set_next(bases, &u);) {
            hb_codepoint_t g;
            if (!hb_font_get_variation_glyph(font, u, v, &g)) continue;
            put32(&o, u);
            put32(&o, v);
            put32(&o, g);
            sequence_count++;
        }
    }
    hb_set_destroy(selectors);
    hb_set_destroy(bases);
    /* By base, then selector (they came by selector). */
    if (!o.failed) qsort(o.bytes + sequences_at, sequence_count, 12, by_sequence);
    set32(&o, 80, sequence_count);
    set32(&o, 84, (uint32_t)sequences_at);
    set32(&o, 20, (uint32_t)o.size);
    for (unsigned g = 0; g < glyph_count; ++g) {
        hb_position_t advance = hb_font_get_glyph_h_advance(font, g);
        if (advance < 0 || advance > 65535) {
            for (int k = 0; k < 3; ++k) hb_blob_destroy(tables[k]);
            FAIL("glyph %u: an advance outside 0 to 65535", g);
        }
        put16(&o, (uint32_t)advance);
    }
    for (int k = 0; k < 3; ++k) {
        align4(&o);
        unsigned length;
        const char *data = hb_blob_get_data(tables[k], &length);
        set32(&o, 24 + 8 * k, length ? (uint32_t)o.size : 0);
        set32(&o, 28 + 8 * k, length);
        put(&o, data, length);
        hb_blob_destroy(tables[k]);
    }
    /* The kern table as it is, for the plans that kern with it; and the
     * glyphs' ink boxes, for those that place marks without GPOS. */
    bool kerns = false, boxes = false;
    for (size_t i = 0; i < plan_count; ++i) {
        kerns |= plans[i].flags & VEGA_PLAN_KERN;
        boxes |= plans[i].flags & VEGA_PLAN_FALLBACK_MARKS;
    }
    if (kerns) {
        hb_blob_t *kern = hb_face_reference_table(face, HB_TAG('k', 'e', 'r', 'n'));
        unsigned length;
        const char *data = hb_blob_get_data(kern, &length);
        align4(&o);
        set32(&o, 64, (uint32_t)o.size);
        set32(&o, 68, length);
        put(&o, data, length);
        hb_blob_destroy(kern);
    }
    if (boxes) {
        align4(&o);
        set32(&o, 72, (uint32_t)o.size);
        for (unsigned g = 0; g < glyph_count; ++g) {
            hb_glyph_extents_t e;
            bool known = hb_font_get_glyph_extents(font, g, &e);
            if (known && (e.x_bearing < -32767 || e.x_bearing > 32767 || e.y_bearing < -32768 || e.y_bearing > 32767 ||
                          e.width < -32768 || e.width > 32767 || e.height < -32768 || e.height > 32767))
                FAIL("glyph %u: an ink box outside 16 bits", g);
            put16(&o, known ? (uint32_t)(e.x_bearing & 0xFFFF) : 0x8000);
            put16(&o, (uint32_t)((known ? e.y_bearing : 0) & 0xFFFF));
            put16(&o, (uint32_t)((known ? e.width : 0) & 0xFFFF));
            put16(&o, (uint32_t)((known ? e.height : 0) & 0xFFFF));
        }
    }
    bool falls_back = false;
    for (size_t i = 0; i < plan_count; ++i) falls_back |= plans[i].shaper_flags & VEGA_ARABIC_FALLBACK;
    if (falls_back) {
        Bytes gsub = {0};
        uint8_t features[7] = {0};
        unsigned count;
        if (!fallback_table(font, &gsub, features, &count, error, error_size)) {
            free(gsub.bytes);
            goto fail;
        }
        align4(&o);
        set32(&o, 76, (uint32_t)o.size);
        unsigned char head[8] = {(unsigned char)count};
        memcpy(head + 1, features, 7);
        put(&o, head, 8);
        put32(&o, (uint32_t)gsub.size);
        put(&o, gsub.bytes, gsub.size);
        free(gsub.bytes);
    }
    align4(&o);
    set32(&o, 48, (uint32_t)plan_count);
    set32(&o, 52, (uint32_t)o.size);
    uint32_t step = 0;
    for (size_t i = 0; i < plan_count; ++i) {
        const Plan *p = &plans[i];
        put32(&o, p->script);
        unsigned char small[4] = {(unsigned char)p->direction, (unsigned char)p->native, (unsigned char)p->shaper,
                                  (unsigned char)p->flags};
        put(&o, small, 4);
        put32(&o, p->global_mask);
        put32(&o, p->kern_mask);
        for (int k = 0; k < VEGA_PLAN_MASKS; ++k) put32(&o, p->masks[k]);
        for (int t = 0; t < 2; ++t) {
            put32(&o, step);
            put32(&o, p->lookup_count[t]);
            step += p->lookup_count[t];
        }
        put32(&o, p->shaper_flags);
        for (int k = 0; k < VEGA_PLAN_STAGES; ++k) {
            put16(&o, p->stages[k].first);
            put16(&o, p->stages[k].count);
            put32(&o, 0);
        }
        put32(&o, 0);
    }
    set32(&o, 56, step);
    set32(&o, 60, (uint32_t)o.size);
    for (size_t i = 0; i < plan_count; ++i)
        for (int t = 0; t < 2; ++t)
            for (unsigned k = 0; k < plans[i].lookup_count[t]; ++k) {
                const LookupMap *l = &plans[i].lookups[t][k];
                put16(&o, l->index);
                unsigned char flags[2] = {(unsigned char)(l->index == VEGA_STEP_PAUSE ? l->pause
                                                          : (l->auto_zwnj ? VEGA_STEP_AUTO_ZWNJ : 0) |
                                                                (l->auto_zwj ? VEGA_STEP_AUTO_ZWJ : 0) |
                                                                (l->random ? VEGA_STEP_RANDOM : 0) |
                                                                (l->per_syllable ? VEGA_STEP_PER_SYLLABLE : 0)),
                                          0};
                put(&o, flags, 2);
                put32(&o, l->mask);
            }
    set32(&o, 4, (uint32_t)o.size);
    if (o.failed) FAIL("out of memory");

    if (glyphs) {
        /* What normalization can reach from codes: them, what they
         * decompose into and compose to, mirrors, the space; then what GSUB
         * can make of those. */
        hb_set_t *wanted = hb_set_create(), *lookups = hb_set_create();
        for (size_t i = 0; i < code_count; ++i) {
            hb_set_add(wanted, codes[i]);
            uint32_t mirror = vega_mirror(codes[i]);
            if (mirror) hb_set_add(wanted, mirror);
            const uint32_t *parts;
            size_t n = vega_decomposition(codes[i], &parts);
            for (size_t k = 0; k < n; ++k) hb_set_add(wanted, parts[k]);
        }
        static const uint32_t always[] = {0x0020, 0x2010, 0x25CC};
        for (size_t i = 0; i < sizeof(always) / sizeof(*always); ++i) hb_set_add(wanted, always[i]);
        for (hb_codepoint_t u = HB_SET_VALUE_INVALID; hb_set_next(unicodes, &u);) {
            const uint32_t *parts;
            size_t n = vega_decomposition(u, &parts);
            bool composed = n > 0;
            for (size_t k = 0; k < n && composed; ++k) composed = hb_set_has(wanted, parts[k]);
            if (composed) hb_set_add(wanted, u);
        }
        hb_set_add(glyphs, 0);
        for (hb_codepoint_t u = HB_SET_VALUE_INVALID; hb_set_next(wanted, &u);) {
            hb_codepoint_t g;
            if (hb_font_get_nominal_glyph(font, u, &g)) hb_set_add(glyphs, g);
        }
        for (size_t i = 0; i < plan_count; ++i)
            for (unsigned k = 0; k < plans[i].lookup_count[0]; ++k)
                if (plans[i].lookups[0][k].index != VEGA_STEP_PAUSE) hb_set_add(lookups, plans[i].lookups[0][k].index);
        hb_ot_layout_lookups_substitute_closure(face, lookups, glyphs);
        hb_set_destroy(wanted);
        hb_set_destroy(lookups);
    }
    for (size_t i = 0; i < plan_count; ++i) free(plans[i].lookups[0]), free(plans[i].lookups[1]);
    free(plans);
    hb_set_destroy(unicodes);
    hb_font_destroy(font);
    *out = (VegaSection){o.bytes, o.size};
    return true;
fail:
    if (plans)
        for (size_t i = 0; i < plan_count; ++i) free(plans[i].lookups[0]), free(plans[i].lookups[1]);
    free(plans);
    free(o.bytes);
    hb_set_destroy(unicodes);
    hb_font_destroy(font);
    return false;
}

/* ---- The check */

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

typedef struct { uint64_t s; } Random;
static uint64_t next_random(Random *r) {
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 0x2545F4914F6CDD1DULL;
}
static uint32_t below(Random *r, uint32_t n) { return n ? (uint32_t)(next_random(r) % n) : 0; }

typedef struct { uint32_t *items; size_t count; } Pool;
static void pool_add(Pool *p, uint32_t code) {
    if (!(p->count & (p->count - 1)) || !p->count) {
        uint32_t *grown = realloc(p->items, (p->count ? 2 * p->count : 16) * sizeof(*grown));
        if (!grown) return;
        p->items = grown;
    }
    p->items[p->count++] = code;
}
static uint32_t pick(Random *r, const Pool *p) { return p->items[below(r, (uint32_t)p->count)]; }

/* A jamo of one part of a Hangul syllable: half the time one that composes
 * (whether the font has it or not), half the time any the font has. */
static uint32_t jamo(Random *r, const Pool *p, uint32_t first, uint32_t count) {
    return p->count && below(r, 2) ? pick(r, p) : first + below(r, count);
}

/* An Indic syllable the ways text spells one (hb-ot-shaper-indic.cc):
 * consonants joined by viramas (a joiner by some), a Ra and virama first
 * for a reph, nuktas, matras, syllable modifiers, a virama at the end; a
 * vowel with its modifiers. indic holds the script's consonants, Ra,
 * viramas, nuktas, matras, modifiers and vowels. */
enum { INDIC_CONSONANTS, INDIC_RA, INDIC_HALANTS, INDIC_NUKTAS, INDIC_MATRAS, INDIC_MODIFIERS, INDIC_VOWELS, INDIC_POOLS };
static size_t indic_syllable(Random *r, const Pool *indic, uint32_t *text, size_t n) {
    const Pool *c = &indic[INDIC_CONSONANTS], *ra = &indic[INDIC_RA], *h = &indic[INDIC_HALANTS],
               *nukta = &indic[INDIC_NUKTAS], *m = &indic[INDIC_MATRAS], *sm = &indic[INDIC_MODIFIERS],
               *v = &indic[INDIC_VOWELS];
    if (!c->count || !h->count) return n;
    if (ra->count && !below(r, 4)) {
        text[n++] = pick(r, ra);
        text[n++] = pick(r, h);
        if (!below(r, 8)) text[n++] = 0x200D;
    }
    if (v->count && !below(r, 6)) {
        text[n++] = pick(r, v);
        if (nukta->count && !below(r, 8)) text[n++] = pick(r, nukta);
    } else
        for (unsigned k = 0, count = 1 + below(r, 3); k < count; ++k) {
            text[n++] = ra->count && !below(r, 5) ? pick(r, ra) : pick(r, c);
            if (nukta->count && !below(r, 6)) text[n++] = pick(r, nukta);
            if (k + 1 == count) break;
            unsigned joiner = below(r, 8);
            if (joiner == 0) text[n++] = 0x200D;
            text[n++] = pick(r, h);
            if (joiner == 1) text[n++] = 0x200D;
            if (joiner == 2) text[n++] = 0x200C;
        }
    if (m->count && below(r, 3)) {
        text[n++] = pick(r, m);
        if (!below(r, 8)) text[n++] = pick(r, m);
    }
    if (sm->count && !below(r, 4)) text[n++] = pick(r, sm);
    if (!below(r, 10)) text[n++] = pick(r, h);
    if (!below(r, 12)) text[n++] = below(r, 2) ? 0x200C : 0x200D;
    return n;
}

/* A Hangul syllable the ways text spells one (hb-ot-shaper-hangul.cc): jamo
 * in the orders that compose (L V, L V T, LV T), a syllable whole, jamo in
 * any order; now and then a tone mark after. jamo holds the font's L, V
 * and T. */
static size_t hangul_syllable(Random *r, const Pool *jamo_pools, uint32_t *text, size_t n) {
    const Pool *l = &jamo_pools[0], *v = &jamo_pools[1], *t = &jamo_pools[2];
    switch (below(r, 5)) {
    case 0:
    case 1:
        text[n++] = jamo(r, l, 0x1100, 19);
        text[n++] = jamo(r, v, 0x1161, 21);
        if (below(r, 2)) text[n++] = jamo(r, t, 0x11A8, 27);
        break;
    case 2:
        text[n++] = 0xAC00 + 28 * below(r, 19 * 21);
        text[n++] = jamo(r, t, 0x11A8, 27);
        break;
    case 3: text[n++] = 0xAC00 + below(r, 11172); break;
    default:
        for (unsigned k = 1 + below(r, 3); k--;) {
            unsigned part = below(r, 3);
            text[n++] = jamo(r, &jamo_pools[part], part == 0 ? 0x1100 : part == 1 ? 0x1161 : 0x11A8, part == 0 ? 19 : part == 1 ? 21 : 27);
        }
    }
    if (!below(r, 8)) text[n++] = 0x302E + below(r, 2);
    return n;
}

static void describe(FILE *report, const char *who, const VegaShaped *g, unsigned count) {
    fprintf(report, "  %-8s", who);
    for (unsigned i = 0; i < count; ++i)
        fprintf(report, " %u@%u(%d,%d+%d,%d)", g[i].glyph, g[i].cluster, g[i].x_advance, g[i].y_advance, g[i].x_offset,
                g[i].y_offset);
    fprintf(report, "\n");
}

void vega_shaping_check(hb_font_t *font, const VegaShaping *s, uint32_t script, int direction, const uint32_t *codes,
                        size_t code_count, uint64_t seed, size_t runs, VegaShapeCheck *out, FILE *report) {
    *out = (VegaShapeCheck){0};
    /* Letters of the script, marks it can take (its own and inherited
     * ones), the rest of its characters and Common's (spaces, digits,
     * punctuation), and those whose glyphs some substitution starts from. */
    Pool letters = {0}, marks = {0}, others = {0}, hot = {0}, jamo_pools[3] = {{0}}, indic[INDIC_POOLS] = {{0}};
    bool hangul = script == HB_TAG('H', 'a', 'n', 'g');
    bool use = listed(use_scripts, sizeof(use_scripts) / sizeof(*use_scripts), script);
    bool syllabic = listed(indic_scripts, sizeof(indic_scripts) / sizeof(*indic_scripts), script) ||
                    script == HB_TAG('K', 'h', 'm', 'r') || script == HB_TAG('M', 'y', 'm', 'r') || use;
    hb_set_t *inputs = hb_set_create();
    uint32_t common = HB_TAG('Z', 'y', 'y', 'y'), inherited = HB_TAG('Z', 'i', 'n', 'h');
    for (unsigned t = 0; t < s->lookup_count[0]; ++t)
        hb_ot_layout_lookup_collect_glyphs(hb_font_get_face(font), HB_OT_TAG_GSUB, t, NULL, inputs, NULL, NULL);
    for (size_t i = 0; i < code_count; ++i) {
        uint32_t code = codes[i], of = vega_script_code(vega_script(code));
        VegaCategory c = vega_category(code);
        hb_codepoint_t glyph;
        if (!hb_font_get_nominal_glyph(font, code, &glyph) || c == VEGA_CC || c == VEGA_CS || c == VEGA_CO) continue;
        bool mark = c == VEGA_MN || c == VEGA_MC || c == VEGA_ME;
        if (hangul && ((code >= 0x1100 && code <= 0x115F) || (code >= 0xA960 && code <= 0xA97C))) pool_add(&jamo_pools[0], code);
        if (hangul && ((code >= 0x1160 && code <= 0x11A7) || (code >= 0xD7B0 && code <= 0xD7C6))) pool_add(&jamo_pools[1], code);
        if (hangul && ((code >= 0x11A8 && code <= 0x11FF) || (code >= 0xD7CB && code <= 0xD7FB))) pool_add(&jamo_pools[2], code);
        if (use && (of == script || of == inherited)) {
            int pool = -1;
            switch (vega_use_category(code)) {
            case VEGA_USE_B: case VEGA_USE_GB: case VEGA_USE_CS: pool = INDIC_CONSONANTS; break;
            case VEGA_USE_R: pool = INDIC_RA; break;
            case VEGA_USE_H: case VEGA_USE_IS: case VEGA_USE_HVM: case VEGA_USE_SK: pool = INDIC_HALANTS; break;
            case VEGA_USE_SUB: case VEGA_USE_CMABV: case VEGA_USE_CMBLW: case VEGA_USE_MABV: case VEGA_USE_MBLW:
            case VEGA_USE_MPST: case VEGA_USE_MPRE: pool = INDIC_NUKTAS; break;
            case VEGA_USE_VABV: case VEGA_USE_VBLW: case VEGA_USE_VPST: case VEGA_USE_VPRE: pool = INDIC_MATRAS; break;
            case VEGA_USE_VMABV: case VEGA_USE_VMBLW: case VEGA_USE_VMPST: case VEGA_USE_VMPRE: case VEGA_USE_SMABV:
            case VEGA_USE_SMBLW: case VEGA_USE_FABV: case VEGA_USE_FBLW: case VEGA_USE_FPST: case VEGA_USE_FMABV:
            case VEGA_USE_FMBLW: case VEGA_USE_FMPST: pool = INDIC_MODIFIERS; break;
            }
            if (pool >= 0) pool_add(&indic[pool], code);
        } else if (syllabic && (of == script || of == inherited)) {
            int pool = -1;
            switch (vega_indic_categories(code) & 0xFF) {
            case VEGA_OT_C: case VEGA_OT_CS: pool = INDIC_CONSONANTS; break;
            case VEGA_OT_RA: pool = INDIC_RA; break;
            case VEGA_OT_H: pool = INDIC_HALANTS; break;
            case VEGA_OT_N: pool = INDIC_NUKTAS; break;
            case VEGA_OT_M: case VEGA_OT_MPST: case VEGA_OT_VABV: case VEGA_OT_VBLW: case VEGA_OT_VPRE:
            case VEGA_OT_VPST: pool = INDIC_MATRAS; break;
            case VEGA_OT_SM: case VEGA_OT_A: case VEGA_OT_SMPST: case VEGA_OT_ROBATIC: case VEGA_OT_XGROUP:
            case VEGA_OT_YGROUP: case VEGA_OT_AS: case VEGA_OT_MH: case VEGA_OT_MR: case VEGA_OT_MW: case VEGA_OT_MY:
            case VEGA_OT_ML: case VEGA_OT_PT: case VEGA_OT_VS: pool = INDIC_MODIFIERS; break;
            case VEGA_OT_V: pool = INDIC_VOWELS; break;
            }
            if (pool >= 0) pool_add(&indic[pool], code);
        }
        if (mark && (of == script || of == inherited)) pool_add(&marks, code);
        else if (!mark && of == script && c >= VEGA_LU && c <= VEGA_LO) pool_add(&letters, code);
        else if (!mark && (of == script || of == common) && c != VEGA_ZL && c != VEGA_ZP) pool_add(&others, code);
        else continue;
        if (hb_set_has(inputs, glyph)) pool_add(&hot, code);
    }
    hb_set_destroy(inputs);
    if (!letters.count) letters = others, others = (Pool){0};
    /* Variation sequences the font maps whose base is among the codes: a
     * base, then its selector (bases[k] and selectors[k]). */
    Pool bases = {0}, selectors = {0};
    hb_set_t *given = hb_set_create(), *vs = hb_set_create(), *us = hb_set_create();
    for (size_t i = 0; i < code_count; ++i) hb_set_add(given, codes[i]);
    hb_face_collect_variation_selectors(hb_font_get_face(font), vs);
    for (hb_codepoint_t v = HB_SET_VALUE_INVALID; hb_set_next(vs, &v);) {
        hb_set_clear(us);
        hb_face_collect_variation_unicodes(hb_font_get_face(font), v, us);
        for (hb_codepoint_t u = HB_SET_VALUE_INVALID; hb_set_next(us, &u);)
            if (hb_set_has(given, u)) {
                pool_add(&bases, u);
                pool_add(&selectors, v);
            }
    }
    size_t sequences = bases.count < selectors.count ? bases.count : selectors.count;
    hb_set_destroy(given);
    hb_set_destroy(vs);
    hb_set_destroy(us);
    VegaShaper *shaper = vega_shaper_new();
    hb_buffer_t *buffer = hb_buffer_create();
    Random random = {seed * 0x9E3779B97F4A7C15ULL + script + 1};
    uint32_t text[256];
    bool rtl_script = direction == VEGA_SHAPE_RTL;
    unsigned mark_rate = rtl_script ? 40 : 12;
    for (size_t run = 0; run < runs && letters.count && shaper; ++run) {
        size_t n = 0;
        if (below(&random, 4)) {
            /* Words: letters (often ones substitution starts from), marks after
             * some, spaces and punctuation between. */
            unsigned words = 1 + below(&random, 6);
            for (unsigned w = 0; w < words && n < 200; ++w) {
                if (w) {
                    if (others.count && !below(&random, 6)) text[n++] = pick(&random, &others);
                    text[n++] = ' ';
                }
                unsigned length = 1 + below(&random, 8);
                for (unsigned k = 0; k < length && n < 200; ++k) {
                    if (hangul && below(&random, 2)) {
                        n = hangul_syllable(&random, jamo_pools, text, n);
                        continue;
                    }
                    if (syllabic && below(&random, 3)) {
                        n = indic_syllable(&random, indic, text, n);
                        continue;
                    }
                    if (sequences && !below(&random, 8)) {
                        size_t k = below(&random, (uint32_t)sequences);
                        text[n++] = bases.items[k];
                        text[n++] = selectors.items[k];
                        continue;
                    }
                    text[n++] = hot.count && below(&random, 3) == 0 ? pick(&random, &hot) : pick(&random, &letters);
                    /* a selector the font may not map after it */
                    if (!below(&random, 60)) text[n++] = 0xFE00 + below(&random, 16);
                    if (marks.count && below(&random, 100) < mark_rate) {
                        text[n++] = pick(&random, &marks);
                        if (!below(&random, 5)) text[n++] = pick(&random, &marks);
                        if (!below(&random, 20)) text[n++] = pick(&random, &marks);
                    }
                    if (!below(&random, 40)) text[n++] = below(&random, 2) ? 0x200C : 0x200D;
                }
            }
        } else {
            /* Anything, in any order: marks first, marks in piles, joiners. */
            unsigned length = 1 + below(&random, 24);
            for (unsigned k = 0; k < length; ++k) {
                unsigned kind = below(&random, 10);
                const Pool *p = kind < 4 ? &letters : kind < 7 && marks.count ? &marks : kind < 8 && hot.count ? &hot : &others;
                text[n++] = kind == 9 ? (below(&random, 3) ? ' ' : below(&random, 2) ? 0x200C : 0x200D)
                                      : p->count ? pick(&random, p) : pick(&random, &letters);
            }
        }
        const VegaShaped *ours;
        double start = now();
        long count = vega_shape(shaper, s, text, n, script, direction, &ours);
        double middle = now();
        hb_buffer_clear_contents(buffer);
        hb_buffer_add_codepoints(buffer, text, (int)n, 0, (int)n);
        hb_buffer_set_direction(buffer, direction == VEGA_SHAPE_RTL ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer, hb_script_from_iso15924_tag(script));
        hb_shape(font, buffer, NULL, 0);
        out->vega_seconds += middle - start;
        out->harfbuzz_seconds += now() - middle;
        unsigned theirs_count;
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &theirs_count);
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, &theirs_count);
        bool same = count == (long)theirs_count;
        for (unsigned i = 0; same && i < theirs_count; ++i)
            same = ours[i].glyph == info[i].codepoint && ours[i].cluster == info[i].cluster &&
                   ours[i].x_advance == pos[i].x_advance && ours[i].y_advance == pos[i].y_advance &&
                   ours[i].x_offset == pos[i].x_offset && ours[i].y_offset == pos[i].y_offset;
        out->runs++;
        out->characters += n;
        out->glyphs += theirs_count;
        if (same) continue;
        if (report && out->differences < 5) {
            fprintf(report, "%c%c%c%c run %zu:", (char)(script >> 24), (char)(script >> 16), (char)(script >> 8),
                    (char)script, run);
            for (size_t i = 0; i < n; ++i) fprintf(report, " %04X", text[i]);
            fprintf(report, "\n");
            if (count >= 0) describe(report, "vega", ours, (unsigned)count);
            else fprintf(report, "  vega     (failed)\n");
            VegaShaped *theirs = malloc((theirs_count + 1) * sizeof(*theirs));
            for (unsigned i = 0; theirs && i < theirs_count; ++i)
                theirs[i] = (VegaShaped){info[i].codepoint, info[i].cluster, pos[i].x_advance, pos[i].y_advance,
                                        pos[i].x_offset, pos[i].y_offset};
            if (theirs) describe(report, "harfbuzz", theirs, theirs_count);
            free(theirs);
        }
        out->differences++;
    }
    hb_buffer_destroy(buffer);
    vega_shaper_free(shaper);
    free(letters.items);
    free(bases.items);
    free(selectors.items);
    free(marks.items);
    free(others.items);
    free(hot.items);
    for (int k = 0; k < 3; ++k) free(jamo_pools[k].items);
    for (int k = 0; k < INDIC_POOLS; ++k) free(indic[k].items);
}
