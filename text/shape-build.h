#ifndef VEGA_SHAPE_BUILD_H
#define VEGA_SHAPE_BUILD_H
#include "shape.h"
#include <hb.h>
#include <stdio.h>

/* Building a shaping section (text/shape.h), and checking it, with
 * HarfBuzz: only the build tool links this. */

typedef struct { unsigned char *bytes; size_t size; } VegaSection;

/* A section for face: plans for each script (ISO 15924 tags) and both
 * directions, compiled as HarfBuzz's planner compiles it. Refuses a script
 * HarfBuzz shapes with a shaper other than the default, Arabic, Hebrew and
 * Hangul ones, and a font with AAT tables (morx, kerx, trak) or a kern
 * table of state machines. glyphs, when given, gets every glyph shaping can
 * reach from codes (for their outlines). */
bool vega_shaping_compile(hb_face_t *face, const uint32_t *scripts, size_t script_count, const uint32_t *codes,
                          size_t code_count, VegaSection *out, hb_set_t *glyphs, char *error, size_t error_size);

typedef struct {
    size_t runs, characters, glyphs, differences;
    double vega_seconds, harfbuzz_seconds; /* shaping alone, each */
} VegaShapeCheck;
/* Shape runs random runs of script (in its plan's direction) made of codes
 * with both the section and HarfBuzz, and count the runs that differ (the
 * first few are written to report). */
void vega_shaping_check(hb_font_t *font, const VegaShaping *s, uint32_t script, int direction, const uint32_t *codes,
                        size_t code_count, uint64_t seed, size_t runs, VegaShapeCheck *out, FILE *report);
#endif
