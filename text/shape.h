#ifndef VEGA_SHAPE_H
#define VEGA_SHAPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shaping (docs/design/004-text.md): a run of characters (one script, one
 * direction) to glyphs and their positions, as HarfBuzz 10 shapes it with
 * its default, Arabic, Hebrew, Hangul and Indic shapers, glyph for glyph and
 * unit for unit.
 *
 * What a font's shaping needs travels in a symbol file (its shaping
 * section), written by the build tool:
 *   - plans, compiled from the font: per script and direction, which
 *     lookups run, in what order, on which glyphs (the feature masks);
 *   - the font's character map and advances;
 *   - its OpenType layout tables (GDEF, GSUB, GPOS) as they are: they are
 *     read in place, every read checked against the table's end, so a
 *     damaged table shapes wrong but never reads outside it.
 * Positions are in the font's units (VegaShaping.upem to an em).
 *
 * The section, little-endian:
 *   "VSHP", u32 size, u16 upem, u16 glyph_count,
 *   u32 cmap_count, u32 cmap_offset       (u32 code, u32 glyph; by code),
 *   u32 advances_offset                   (glyph_count u16),
 *   u32 offset, u32 size of GDEF, of GSUB and of GPOS (0 0 when absent),
 *   u32 plan_count, u32 plans_offset      (VEGA_PLAN_BYTES each),
 *   u32 step_count, u32 steps_offset      (8 bytes each),
 *   u32 offset, u32 size of the kern table (0 0 when absent),
 *   u32 extents_offset                    (glyph_count of i16 x_bearing,
 *                                          y_bearing, width, height; 0 when
 *                                          no plan positions marks without
 *                                          GPOS; x_bearing INT16_MIN: none),
 *   u32 0
 * Offsets are from the section's start. A plan (VEGA_PLAN_BYTES):
 *   u32 script (ISO 15924 as a tag: 'Latn'), u8 direction, u8 native
 *   direction (of its script; 0 when it has none), u8 shaper, u8 flags,
 *   u32 global mask, u32 kern mask, u32 masks[VEGA_PLAN_MASKS], u32 first
 *   GSUB step, u32 GSUB steps, u32 first GPOS step, u32 GPOS steps,
 *   u8 shaper flags, u8 0[3],
 *   the stages the shaper asks whether a feature would substitute in
 *   (VEGA_PLAN_STAGES of: u16 first step, counted from the plan's first
 *   GSUB step, u16 steps, u32 0), u32 0.
 * A step, a lookup to run: u16 lookup index, u8 flags, u8 0, u32 mask; or
 * a pause, where the shaper does its part: u16 VEGA_STEP_PAUSE, u8 which
 * (VEGA_PAUSE_...), u8 0, u32 0. */
enum { VEGA_SHAPING_HEADER = 80, VEGA_PLAN_BYTES = 144, VEGA_STEP_BYTES = 8, VEGA_PLAN_MASKS = 16,
       VEGA_PLAN_STAGES = 5 };
enum { VEGA_SHAPE_LTR = 1, VEGA_SHAPE_RTL = 2 };
enum { VEGA_SHAPER_DEFAULT, VEGA_SHAPER_ARABIC, VEGA_SHAPER_HEBREW, VEGA_SHAPER_HANGUL, VEGA_SHAPER_INDIC };
/* Plan flags: GPOS runs; mark advances are zeroed after it (by GDEF); glyph
 * classes come from Unicode (the font has none); the font positions marks
 * (a 'mark' feature); automatic fractions; the kern table kerns; marks are
 * placed around their base without GPOS, and zeroing a mark's advance keeps
 * it where it was. */
enum { VEGA_PLAN_GPOS = 1, VEGA_PLAN_ZERO_MARKS = 2, VEGA_PLAN_GUESS_CLASSES = 4, VEGA_PLAN_GPOS_MARK = 8,
       VEGA_PLAN_FRACTIONS = 16, VEGA_PLAN_KERN = 32, VEGA_PLAN_FALLBACK_MARKS = 64, VEGA_PLAN_ADJUST_ZEROING = 128 };
/* The masks a plan names, for the features set glyph by glyph: those of
 * every shaper, then each shaper's own. */
enum { VEGA_MASK_RTLM, VEGA_MASK_FRAC, VEGA_MASK_NUMR, VEGA_MASK_DNOM, VEGA_MASK_SHAPER };
enum { VEGA_MASK_ISOL = VEGA_MASK_SHAPER, VEGA_MASK_FINA, VEGA_MASK_FIN2, VEGA_MASK_FIN3, VEGA_MASK_MEDI,
       VEGA_MASK_MED2, VEGA_MASK_INIT };                                      /* Arabic */
enum { VEGA_MASK_LJMO = VEGA_MASK_SHAPER, VEGA_MASK_VJMO, VEGA_MASK_TJMO };      /* Hangul */
enum { VEGA_MASK_RPHF = VEGA_MASK_SHAPER, VEGA_MASK_PREF, VEGA_MASK_BLWF, VEGA_MASK_ABVF, VEGA_MASK_HALF,
       VEGA_MASK_PSTF, VEGA_MASK_INDIC_INIT };                                /* Indic */
/* The stages a plan names: Indic's, whose features decide a consonant's
 * position by whether they would substitute it. */
enum { VEGA_STAGE_RPHF, VEGA_STAGE_PREF, VEGA_STAGE_BLWF, VEGA_STAGE_PSTF, VEGA_STAGE_VATU };
/* Shaper flags: Indic's font follows the old specification (deva, not
 * dev2). */
enum { VEGA_INDIC_OLD_SPEC = 1 };
/* Step flags; pauses, which the shaper tells apart. */
enum { VEGA_STEP_AUTO_ZWNJ = 1, VEGA_STEP_AUTO_ZWJ = 2, VEGA_STEP_RANDOM = 4, VEGA_STEP_PER_SYLLABLE = 8 };
enum { VEGA_STEP_PAUSE = 0xFFFF };
enum { VEGA_PAUSE_SYLLABLES = 1, VEGA_PAUSE_INITIAL_REORDERING, VEGA_PAUSE_FINAL_REORDERING, VEGA_PAUSE_CLEAR_SYLLABLES };

typedef struct { const unsigned char *p; uint32_t size; } VegaTable;
/* Which glyphs may be in a set, as three 64-bit patterns of glyph bits
 * (HarfBuzz's set digest): a glyph whose bits are not all there is not. */
typedef struct { uint64_t mask[3]; } VegaDigest;
typedef struct {
    uint16_t type; /* with extensions resolved */
    uint32_t at;
    VegaDigest digest; /* the glyphs it starts from */
} VegaSubtable;
typedef struct {
    uint16_t type, flags;
    uint32_t props;                 /* flags, and the mark filtering set in the high half */
    uint32_t first, count;          /* subtables */
    bool reverse;
    VegaDigest digest;              /* the glyphs its subtables start from */
} VegaLookup;
typedef struct {
    const unsigned char *bytes;
    size_t size;
    uint16_t upem, glyph_count;
    uint32_t cmap_count, plan_count;
    const unsigned char *cmap, *advances, *plans, *steps, *extents;
    VegaTable gdef, gsub, gpos, kern;
    uint32_t glyph_classes, attach_classes, mark_sets; /* in GDEF; 0 when absent */
    VegaLookup *lookups[2];         /* GSUB, GPOS */
    uint32_t lookup_count[2];
    VegaSubtable *subtables;
    uint16_t *bmp[256];             /* the character map's glyphs below U+10000, by page */
} VegaShaping;

/* Open a shaping section (kept, not copied); false with why. */
bool vega_shaping_open(VegaShaping *, const unsigned char *bytes, size_t size, char *error, size_t error_size);
void vega_shaping_close(VegaShaping *);
/* The font's glyph for a character, or 0. */
uint32_t vega_shaping_glyph(const VegaShaping *, uint32_t code);

/* A shaped glyph, as HarfBuzz gives it: in visual order (left to right),
 * cluster the index of the first character it came from; each glyph is
 * drawn at the pen plus its offset, then the pen moves by its advance. */
typedef struct {
    uint32_t glyph, cluster;
    int32_t x_advance, y_advance, x_offset, y_offset;
} VegaShaped;

/* The buffers shaping works in, kept from one run to the next. */
typedef struct VegaShaper VegaShaper;
VegaShaper *vega_shaper_new(void);
void vega_shaper_free(VegaShaper *);
/* Shape a run of count characters of script (an ISO 15924 tag, as
 * vega_script_code gives it) in direction (VEGA_SHAPE_LTR or _RTL) by the
 * plan compiled for them (or for Common, 'Zyyy', when there is none). The
 * glyphs (as many as returned) are the shaper's until its next run; -1
 * when there is no plan or no memory. */
long vega_shape(VegaShaper *, const VegaShaping *, const uint32_t *codes, size_t count, uint32_t script, int direction,
                const VegaShaped **out);
#endif
