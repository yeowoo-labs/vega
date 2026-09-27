#ifndef VEGA_SYMBOLS_GL_H
#define VEGA_SYMBOLS_GL_H
#include "symbols.h"
#include <GLES3/gl31.h>

/* Symbols on the GPU (OpenGL ES 3.1): the symbols wanted at one scale are
 * drawn on the CPU into an atlas, each pixel the exact area its curves cover
 * (vega_symbol_raster), and text is quads reading it. On the Pi 5 the CPU
 * draws the atlas faster than the GPU's compute could, and the GPU composes
 * a 1080p page of text in about a millisecond (docs/design/002). A scale that
 * keeps changing (a zoom) draws its atlas again. Needs a current context. */
typedef struct {
    GLuint draw, array, instances;
    GLint target;
} VegaGl;

typedef struct {
    GLuint texture; /* 8-bit coverage */
    int width, height;
    float scale;    /* pixels per unit */
    uint32_t symbols;
    VegaPlace *places; /* per symbol index: its box and where the atlas keeps it */
} VegaAtlas;

/* A symbol at a pen position (whole pixels from the target's top left, on the
 * baseline). */
typedef struct { int32_t x, y; uint32_t symbol; } VegaGlyph;

bool vega_gl_open(VegaGl *, char *error, size_t error_size);
/* Draw symbols (their indices; all when wanted is NULL) at scale into a new
 * atlas; false when they do not fit one texture. */
bool vega_gl_atlas(VegaGl *, const VegaSymbols *, float scale, const uint32_t *wanted, uint32_t count, VegaAtlas *out,
                   char *error, size_t error_size);
/* Coverage added into the red channel of the bound framebuffer. */
void vega_gl_draw(VegaGl *, const VegaAtlas *, const VegaGlyph *, uint32_t count, int width, int height);
void vega_gl_atlas_close(VegaAtlas *);
void vega_gl_close(VegaGl *);
#endif
