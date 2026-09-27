#include "symbols-gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BANDS = 16 };

/* Coordinates travel doubled, so implied points stay whole: 2x of a point
 * is its raw i16 without the flag, and an implied point's is the sum of its
 * two neighbours' coordinates. */
#define SHARED \
    "#version 310 es\n" \
    "precision highp float;\n" \
    "precision highp int;\n"

static const char vertex_source[] = SHARED
    "layout(location = 0) in vec4 place;   /* origin x, y in pixels; pixels per unit; unused */\n"
    "layout(location = 1) in vec4 bounds;  /* left, bottom, right, top in units */\n"
    "layout(location = 2) in uint bands;   /* its first band */\n"
    "uniform vec2 target;\n"
    "out vec2 units;\n"
    "flat out float scale;\n"
    "flat out ivec2 rows;                 /* bottom and top in units */\n"
    "flat out uint base;\n"
    "void main() {\n"
    "    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "    vec2 pad = vec2(1.0 / place.z);\n"
    "    units = mix(bounds.xy - pad, bounds.zw + pad, corner);\n"
    "    vec2 pixel = place.xy + vec2(units.x, -units.y) * place.z;\n"
    "    gl_Position = vec4(pixel.x / target.x * 2.0 - 1.0, 1.0 - pixel.y / target.y * 2.0, 0.0, 1.0);\n"
    "    scale = place.z;\n"
    "    rows = ivec2(bounds.yw);\n"
    "    base = bands;\n"
    "}\n";

static const char fragment_source[] = SHARED
    "layout(std430, binding = 0) readonly buffer Points { uint points[]; };\n"
    "layout(std430, binding = 1) readonly buffer Bands { uint bands[]; };\n"
    "uniform uint listed; /* where the lists of curves start in bands */\n"
    "in vec2 units;\n"
    "flat in float scale;\n"
    "flat in ivec2 rows;\n"
    "flat in uint base;\n"
    "out vec4 color;\n"
    "\n"
    "ivec3 point(uint i) { /* doubled x, doubled y, off */\n"
    "    uint w = points[i];\n"
    "    int x = int(w << 16u) >> 16, y = int(w) >> 16;\n"
    "    return ivec3(x & ~1, y & ~1, x & 1);\n"
    "}\n"
    "int band(int doubled) { /* which of 16 bands a doubled y lies in */\n"
    "    return clamp((doubled - 2 * rows.x) * 16 / (2 * (rows.y - rows.x) + 2), 0, 15);\n"
    "}\n"
    "int roots(float a, float b, float c, inout float ts[10], int n) {\n"
    "    float r0 = -1.0, r1 = -1.0;\n"
    "    if (a == 0.0) { if (b != 0.0) r0 = -c / b; }\n"
    "    else {\n"
    "        float d = b * b - 4.0 * a * c;\n"
    "        if (d >= 0.0) {\n"
    "            float q = -0.5 * (b + (b >= 0.0 ? sqrt(d) : -sqrt(d)));\n"
    "            r0 = q / a;\n"
    "            if (q != 0.0) r1 = c / q;\n"
    "        }\n"
    "    }\n"
    "    if (r0 > 0.0 && r0 < 1.0) ts[n++] = r0;\n"
    "    if (r1 > 0.0 && r1 < 1.0) ts[n++] = r1;\n"
    "    return n;\n"
    "}\n"
    "/* As curve_area in symbols.c: the curve's share of the pixel [0, 1]^2. */\n"
    "float area(vec2 p0, vec2 p1, vec2 p2) {\n"
    "    if (max(max(p0.y, p1.y), p2.y) <= 0.0 || min(min(p0.y, p1.y), p2.y) >= 1.0 || min(min(p0.x, p1.x), p2.x) >= 1.0)\n"
    "        return 0.0;\n"
    "    if (max(max(p0.x, p1.x), p2.x) <= 0.0) return clamp(p2.y, 0.0, 1.0) - clamp(p0.y, 0.0, 1.0);\n"
    "    vec2 a = p0 - 2.0 * p1 + p2, b = p0 - p1;\n"
    "    float ts[10];\n"
    "    ts[0] = 0.0;\n"
    "    int n = 1;\n"
    "    n = roots(a.y, -2.0 * b.y, p0.y, ts, n);\n"
    "    n = roots(a.y, -2.0 * b.y, p0.y - 1.0, ts, n);\n"
    "    n = roots(a.x, -2.0 * b.x, p0.x, ts, n);\n"
    "    n = roots(a.x, -2.0 * b.x, p0.x - 1.0, ts, n);\n"
    "    ts[n++] = 1.0;\n"
    "    for (int i = 1; i < n; ++i)\n"
    "        for (int j = i; j > 0 && ts[j] < ts[j - 1]; --j) { float t = ts[j]; ts[j] = ts[j - 1]; ts[j - 1] = t; }\n"
    "    float u0 = 1.0 - p0.x, u1 = 2.0 * b.x, u2 = -a.x, v0 = -2.0 * b.y, v1 = 2.0 * a.y;\n"
    "    float k1 = u0 * v0, k2 = (u0 * v1 + u1 * v0) / 2.0, k3 = (u1 * v1 + u2 * v0) / 3.0, k4 = u2 * v1 / 4.0, sum = 0.0;\n"
    "    for (int i = 0; i + 1 < n; ++i) {\n"
    "        float ta = ts[i], tb = ts[i + 1], t = (ta + tb) / 2.0;\n"
    "        if (tb <= ta) continue;\n"
    "        float y = (a.y * t - 2.0 * b.y) * t + p0.y, x = (a.x * t - 2.0 * b.x) * t + p0.x;\n"
    "        if (y <= 0.0 || y >= 1.0 || x >= 1.0) continue;\n"
    "        if (x <= 0.0) sum += (a.y * tb - 2.0 * b.y) * tb - (a.y * ta - 2.0 * b.y) * ta;\n"
    "        else sum += (((k4 * tb + k3) * tb + k2) * tb + k1) * tb - (((k4 * ta + k3) * ta + k2) * ta + k1) * ta;\n"
    "    }\n"
    "    return sum;\n"
    "}\n"
    "void main() {\n"
    "    vec2 corner = units - vec2(0.5 / scale); /* the pixel's lower left, in units */\n"
    "    int b0 = band(2 * int(floor(corner.y))), b1 = band(2 * int(ceil(corner.y + 1.0 / scale)));\n"
    "    float sum = 0.0;\n"
    "    for (int b = b0; b <= b1; ++b) {\n"
    "        uint at = bands[base + 2u * uint(b)], end = at + bands[base + 2u * uint(b) + 1u];\n"
    "        for (; at < end; ++at) {\n"
    "            uint k = bands[listed + at];\n"
    "            ivec3 p = point(k), n = point(k + 1u);\n"
    "            ivec2 s, c, e;\n"
    "            if (p.z == 1) {\n"
    "                ivec3 q = point(k - 1u);\n"
    "                s = q.z == 1 ? (q.xy + p.xy) / 2 : q.xy;\n"
    "                c = p.xy;\n"
    "                e = n.z == 1 ? (p.xy + n.xy) / 2 : n.xy;\n"
    "            } else {\n"
    "                s = p.xy;\n"
    "                c = (p.xy + n.xy) / 2;\n"
    "                e = n.xy;\n"
    "            }\n"
    "            /* a curve over several of this pixel's bands counts in the first */\n"
    "            if (max(band(min(min(s.y, c.y), e.y)), b0) != b) continue;\n"
    "            sum += area((vec2(s) * 0.5 - corner) * scale, (vec2(c) * 0.5 - corner) * scale, (vec2(e) * 0.5 - corner) * scale);\n"
    "        }\n"
    "    }\n"
    "    color = vec4(min(abs(sum), 1.0), 0.0, 0.0, 1.0);\n"
    "}\n";

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

/* Doubled coordinates make implied points whole; bands as the shader cuts them. */
static int band(int doubled, int bottom, int top) {
    int b = (doubled - 2 * bottom) * BANDS / (2 * (top - bottom) + 2);
    return b < 0 ? 0 : b >= BANDS ? BANDS - 1 : b;
}

bool vega_gl_open(VegaGl *gl, const VegaSymbols *s, char *error, size_t error_size) {
    *gl = (VegaGl){0};
    GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source, error, error_size);
    GLuint fragment = vertex ? compile(GL_FRAGMENT_SHADER, fragment_source, error, error_size) : 0;
    if (!fragment) return false;
    gl->program = glCreateProgram();
    glAttachShader(gl->program, vertex);
    glAttachShader(gl->program, fragment);
    glLinkProgram(gl->program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok;
    glGetProgramiv(gl->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(gl->program, (GLsizei)error_size, NULL, error);
        return false;
    }
    /* The point table, as the file has it. */
    glGenBuffers(1, &gl->points);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl->points);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(s->points ? s->points : 1) * VEGA_POINT_BYTES,
                 s->bytes + VEGA_SYMBOLS_HEADER + (size_t)s->symbols * VEGA_SYMBOL_BYTES, GL_STATIC_DRAW);
    /* Bands: per symbol, 16 (start, count) pairs into one list of curves,
     * each curve (by the point that begins it) in every band it reaches. */
    size_t table = (size_t)s->symbols * BANDS * 2, listed = 0;
    uint32_t *counts = calloc(table + 1, sizeof(*counts));
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t *bands = pass ? malloc((table + listed + 1) * sizeof(*bands)) : NULL;
        size_t next = 0;
        for (uint32_t i = 0; i < s->symbols; ++i) {
            VegaSymbol symbol = vega_symbol_at(s, i);
            uint32_t *mine = pass ? bands + (size_t)i * BANDS * 2 : counts + (size_t)i * BANDS * 2;
            if (pass)
                for (int b = 0; b < BANDS; ++b) {
                    mine[2 * b] = (uint32_t)next;
                    next += counts[(size_t)i * BANDS * 2 + 2 * b + 1];
                    mine[2 * b + 1] = 0;
                }
            VegaCurve c;
            for (uint32_t k = symbol.first; k < symbol.first + symbol.count; ++k) {
                if (!vega_symbol_curve(s, &symbol, k, &c)) continue;
                float low = c.y0 < c.y1 ? c.y0 : c.y1, high = c.y0 > c.y1 ? c.y0 : c.y1;
                low = c.y2 < low ? c.y2 : low;
                high = c.y2 > high ? c.y2 : high;
                int from = band((int)(2 * low), symbol.bottom, symbol.top), to = band((int)(2 * high), symbol.bottom, symbol.top);
                for (int b = from; b <= to; ++b) {
                    if (pass) bands[table + mine[2 * b] + mine[2 * b + 1]] = k;
                    ++mine[2 * b + 1];
                    listed += !pass;
                }
            }
        }
        if (pass) {
            glGenBuffers(1, &gl->bands);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl->bands);
            glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((table + listed + 1) * sizeof(*bands)), bands, GL_STATIC_DRAW);
            free(bands);
        }
    }
    free(counts);
    glGenVertexArrays(1, &gl->array);
    glGenBuffers(1, &gl->instances);
    glBindVertexArray(gl->array);
    glBindBuffer(GL_ARRAY_BUFFER, gl->instances);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 36, (void *)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 36, (void *)16);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, 36, (void *)32);
    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);
    glBindVertexArray(0);
    glUseProgram(gl->program);
    glUniform1ui(glGetUniformLocation(gl->program, "listed"), (GLuint)table);
    gl->viewport = glGetUniformLocation(gl->program, "target");
    if (glGetError() != GL_NO_ERROR) {
        snprintf(error, error_size, "GL error while uploading");
        return false;
    }
    return true;
}

void vega_gl_draw(VegaGl *gl, const VegaSymbols *s, const VegaGlyph *glyphs, uint32_t count, int width, int height) {
    float *instances = malloc((size_t)(count ? count : 1) * 36);
    for (uint32_t i = 0; i < count; ++i) {
        VegaSymbol symbol = vega_symbol_at(s, glyphs[i].symbol);
        float *f = instances + (size_t)i * 9;
        f[0] = glyphs[i].x, f[1] = glyphs[i].y, f[2] = glyphs[i].scale, f[3] = 0;
        f[4] = symbol.left, f[5] = symbol.bottom, f[6] = symbol.right, f[7] = symbol.top;
        uint32_t base = glyphs[i].symbol * BANDS * 2;
        memcpy(f + 8, &base, 4);
    }
    glUseProgram(gl->program);
    glUniform2f(gl->viewport, (float)width, (float)height);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, gl->points);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gl->bands);
    glBindVertexArray(gl->array);
    glBindBuffer(GL_ARRAY_BUFFER, gl->instances);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)count * 36, instances, GL_STREAM_DRAW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)count);
    glBindVertexArray(0);
    free(instances);
}

void vega_gl_close(VegaGl *gl) {
    glDeleteBuffers(1, &gl->points);
    glDeleteBuffers(1, &gl->bands);
    glDeleteBuffers(1, &gl->instances);
    glDeleteVertexArrays(1, &gl->array);
    glDeleteProgram(gl->program);
    *gl = (VegaGl){0};
}
