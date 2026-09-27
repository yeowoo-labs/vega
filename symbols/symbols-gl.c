#include "symbols-gl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ATLAS_MAX = 4096 };

#define SHARED \
    "#version 310 es\n" \
    "precision highp float;\n" \
    "precision highp int;\n"

/* Text: a quad per glyph at whole pixels, each pixel one atlas texel. */
static const char draw_vertex[] = SHARED
    "layout(location = 0) in ivec4 rect; /* in the atlas */\n"
    "layout(location = 1) in ivec2 at;   /* its top left on the target */\n"
    "uniform vec2 target;\n"
    "out vec2 texel;\n"
    "void main() {\n"
    "    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)), size = vec2(rect.zw);\n"
    "    vec2 pixel = vec2(at) + corner * size;\n"
    "    texel = vec2(rect.xy) + corner * size;\n"
    "    gl_Position = vec4(pixel.x / target.x * 2.0 - 1.0, 1.0 - pixel.y / target.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char draw_fragment[] = SHARED
    "uniform highp sampler2D atlas;\n"
    "in vec2 texel;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(texelFetch(atlas, ivec2(texel), 0).r, 0.0, 0.0, 1.0); }\n";

static GLuint compile(GLenum type, const char *source, char *error, size_t error_size) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(shader, (GLsizei)error_size, NULL, error);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool vega_gl_open(VegaGl *gl, char *error, size_t error_size) {
    *gl = (VegaGl){0};
    GLuint vertex = compile(GL_VERTEX_SHADER, draw_vertex, error, error_size);
    GLuint fragment = vertex ? compile(GL_FRAGMENT_SHADER, draw_fragment, error, error_size) : 0;
    if (!fragment) {
        if (vertex) glDeleteShader(vertex);
        return false;
    }
    gl->draw = glCreateProgram();
    glAttachShader(gl->draw, vertex);
    glAttachShader(gl->draw, fragment);
    glLinkProgram(gl->draw);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok;
    glGetProgramiv(gl->draw, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(gl->draw, (GLsizei)error_size, NULL, error);
        vega_gl_close(gl);
        return false;
    }
    glUseProgram(gl->draw);
    glUniform1i(glGetUniformLocation(gl->draw, "atlas"), 0);
    gl->target = glGetUniformLocation(gl->draw, "target");
    glGenVertexArrays(1, &gl->array);
    glGenBuffers(1, &gl->instances);
    glBindVertexArray(gl->array);
    glBindBuffer(GL_ARRAY_BUFFER, gl->instances);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(0, 4, GL_INT, 24, (void *)0);
    glVertexAttribIPointer(1, 2, GL_INT, 24, (void *)16);
    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);
    glBindVertexArray(0);
    if (glGetError() != GL_NO_ERROR) {
        snprintf(error, error_size, "cannot set up the text program");
        vega_gl_close(gl);
        return false;
    }
    return true;
}

/* The pixels a symbol reaches at scale, one more on each side. */
static VegaPlace place_of(const VegaSymbol *symbol, float scale) {
    if (!symbol->count) return (VegaPlace){0};
    int left = (int)floorf(symbol->left * scale) - 1, right = (int)ceilf(symbol->right * scale) + 1;
    int top = (int)ceilf(symbol->top * scale) + 1, bottom = (int)floorf(symbol->bottom * scale) - 1;
    return (VegaPlace){0, 0, right - left, top - bottom, left, top};
}

bool vega_gl_atlas(VegaGl *gl, const VegaSymbols *s, float scale, const uint32_t *wanted, uint32_t count, VegaAtlas *out,
                   char *error, size_t error_size) {
    (void)gl;
    *out = (VegaAtlas){.scale = scale, .symbols = s->symbols};
    if (!wanted) count = s->symbols;
    out->places = calloc(s->symbols ? s->symbols : 1, sizeof(*out->places));
    if (!out->places) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    /* Shelves, as wide as the square the rectangles would fill (a power of
     * two), the next shelf starting under the tallest of the one before. */
    uint64_t area = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = wanted ? wanted[i] : i;
        if (index >= s->symbols) continue;
        VegaSymbol symbol = vega_symbol_at(s, index);
        VegaPlace p = place_of(&symbol, scale);
        area += (uint64_t)p.width * (uint64_t)p.height;
    }
    int width = 64;
    while (width < ATLAS_MAX && (uint64_t)width * (uint64_t)width < area + area / 4) width *= 2;
    int x = 0, y = 0, shelf = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = wanted ? wanted[i] : i;
        if (index >= s->symbols) continue;
        VegaSymbol symbol = vega_symbol_at(s, index);
        VegaPlace p = place_of(&symbol, scale);
        if (!p.width) continue;
        if (p.width > width) width = p.width;
        if (x + p.width > width) { x = 0; y += shelf; shelf = 0; }
        p.x = x, p.y = y;
        x += p.width;
        shelf = p.height > shelf ? p.height : shelf;
        out->places[index] = p;
    }
    int height = y + shelf;
    if (width > ATLAS_MAX || height > ATLAS_MAX) {
        snprintf(error, error_size, "%u symbols at %.4f pixels a unit need %dx%d, more than one %d-pixel atlas", count,
                 (double)scale, width, height, ATLAS_MAX);
        vega_gl_atlas_close(out);
        return false;
    }
    out->width = width, out->height = height ? height : 1;
    unsigned char *pixels = calloc((size_t)out->width * (size_t)out->height, 1);
    if (!pixels) {
        snprintf(error, error_size, "out of memory");
        vega_gl_atlas_close(out);
        return false;
    }
    for (uint32_t i = 0; i < s->symbols; ++i) {
        VegaPlace p = out->places[i];
        if (!p.width) continue;
        VegaSymbol symbol = vega_symbol_at(s, i);
        vega_symbol_raster(s, &symbol, scale, p.left / scale, p.top / scale, p.width, p.height,
                           pixels + (size_t)p.y * (size_t)out->width + (size_t)p.x, (size_t)out->width);
    }
    glGenTextures(1, &out->texture);
    glBindTexture(GL_TEXTURE_2D, out->texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, out->width, out->height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, out->width, out->height, GL_RED, GL_UNSIGNED_BYTE, pixels);
    free(pixels);
    if (glGetError() != GL_NO_ERROR) {
        snprintf(error, error_size, "cannot upload the atlas");
        vega_gl_atlas_close(out);
        return false;
    }
    return true;
}

void vega_gl_draw(VegaGl *gl, const VegaAtlas *atlas, const VegaGlyph *glyphs, uint32_t count, int width, int height) {
    int32_t *instances = malloc((size_t)(count ? count : 1) * 6 * sizeof(*instances));
    uint32_t n = 0;
    for (uint32_t i = 0; instances && i < count; ++i) {
        if (glyphs[i].symbol >= atlas->symbols) continue;
        VegaPlace p = atlas->places[glyphs[i].symbol];
        if (!p.width) continue;
        int32_t *q = instances + (size_t)n++ * 6;
        q[0] = p.x, q[1] = p.y, q[2] = p.width, q[3] = p.height;
        q[4] = glyphs[i].x + p.left, q[5] = glyphs[i].y - p.top;
    }
    glUseProgram(gl->draw);
    glUniform2f(gl->target, (float)width, (float)height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas->texture);
    glBindVertexArray(gl->array);
    glBindBuffer(GL_ARRAY_BUFFER, gl->instances);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * 6 * sizeof(*instances)), instances, GL_STREAM_DRAW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    if (n) glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)n);
    glBindVertexArray(0);
    free(instances);
}

void vega_gl_atlas_close(VegaAtlas *atlas) {
    if (atlas->texture) glDeleteTextures(1, &atlas->texture);
    free(atlas->places);
    *atlas = (VegaAtlas){0};
}

void vega_gl_close(VegaGl *gl) {
    if (gl->instances) glDeleteBuffers(1, &gl->instances);
    if (gl->array) glDeleteVertexArrays(1, &gl->array);
    if (gl->draw) glDeleteProgram(gl->draw);
    *gl = (VegaGl){0};
}
