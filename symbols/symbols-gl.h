#ifndef VEGA_SYMBOLS_GL_H
#define VEGA_SYMBOLS_GL_H
#include "symbols.h"
#include <GLES3/gl31.h>

/* Symbols drawn by the GPU (OpenGL ES 3.1): the file's point table goes up
 * as it is, one storage buffer, and each pixel of a symbol's box sums the
 * exact area of the curves crossing its row, as vega_coverage does. Rows
 * are cut into bands at upload, each listing its curves, so a pixel reads
 * only those. Needs a current context. */
typedef struct {
    GLuint program, points, bands, instances, array;
    GLint viewport;
} VegaGl;

/* A symbol placed: its origin in pixels from the target's top left, and
 * pixels per unit. */
typedef struct { float x, y, scale; uint32_t symbol; } VegaGlyph;

bool vega_gl_open(VegaGl *, const VegaSymbols *, char *error, size_t error_size);
/* Coverage added into the red channel of the bound framebuffer. */
void vega_gl_draw(VegaGl *, const VegaSymbols *, const VegaGlyph *, uint32_t count, int width, int height);
void vega_gl_close(VegaGl *);
#endif
