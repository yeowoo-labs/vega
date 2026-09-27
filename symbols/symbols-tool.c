#define _POSIX_C_SOURCE 200809L
/* vega-symbols: build symbol files from fonts and SVG icons, and measure them
 * against FreeType (docs/design/002-symbol-format.md).
 *   vega-symbols build [--font FONT.ttf --codes 0x20-0x7e,...] [--svg CODE=ICON.svg ...] [--units N] -o OUT.vsym
 *   vega-symbols compare FONT.ttf OUT.vsym --sizes 12,16,32,64
 *   vega-symbols info OUT.vsym
 *   vega-symbols render OUT.vsym CODE PIXELS OUT.pgm
 *   vega-symbols gpu OUT.vsym --sizes 12,16,32,64 [--frames N] */
#include "symbols-gl.h"
#include "symbols.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI 3.14159265358979323846

typedef struct { double x, y; } Point;
/* An outline as the pen drew it. smooth: the end lies halfway between this
 * control and the next, by construction, so it may go unwritten. */
typedef enum { MOVE, LINE, QUAD } Kind;
typedef struct { Kind kind; bool smooth; Point control, end; } Segment;
typedef struct { Segment *items; size_t count, capacity; Point at, start; bool open; double font_scale; } Path;
typedef struct { uint32_t code; size_t first, count; int16_t advance; } Built; /* segments [first, first + count) */

static void fail(const char *why) {
    fprintf(stderr, "vega-symbols: %s\n", why);
    exit(1);
}

static void add(Path *path, Segment segment) {
    if (path->count == path->capacity) {
        path->capacity = path->capacity ? 2 * path->capacity : 64;
        if (!(path->items = realloc(path->items, path->capacity * sizeof(*path->items)))) fail("out of memory");
    }
    path->items[path->count++] = segment;
    path->at = segment.end;
}

static bool same(Point a, Point b) { return a.x == b.x && a.y == b.y; }

static void line_to(Path *path, Point to) {
    if (!same(to, path->at)) add(path, (Segment){LINE, false, to, to});
}

static void quad_to(Path *path, Point control, Point to) { add(path, (Segment){QUAD, false, control, to}); }

static Point lerp(Point a, Point b, double t) { return (Point){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
static double length(double x, double y) { return sqrt(x * x + y * y); }

/* Whether the cubic of these differences stays within tolerance of zero
 * (split in halves until its control points do). */
static bool fits(Point p0, Point p1, Point p2, Point p3, double tolerance) {
    if (length(p1.x, p1.y) <= tolerance && length(p2.x, p2.y) <= tolerance) return true;
    Point mid = {(p0.x + 3 * (p1.x + p2.x) + p3.x) / 8, (p0.y + 3 * (p1.y + p2.y) + p3.y) / 8};
    if (length(mid.x, mid.y) > tolerance) return false;
    Point d = {(p3.x + p2.x - p1.x - p0.x) / 8, (p3.y + p2.y - p1.y - p0.y) / 8};
    return fits(p0, lerp(p0, p1, 0.5), (Point){mid.x - d.x, mid.y - d.y}, mid, tolerance) &&
           fits(mid, (Point){mid.x + d.x, mid.y + d.y}, lerp(p2, p3, 0.5), p3, tolerance);
}

/* The piece of cubic c over [a, b], as a cubic. */
static void piece(const Point c[4], double a, double b, Point out[4]) {
    Point q[4];
    for (int k = 0; k < 2; ++k) {
        double t = k ? b : a, u = 1 - t;
        Point e = {u * u * u * c[0].x + 3 * u * u * t * c[1].x + 3 * u * t * t * c[2].x + t * t * t * c[3].x,
                   u * u * u * c[0].y + 3 * u * u * t * c[1].y + 3 * u * t * t * c[2].y + t * t * t * c[3].y};
        Point d = {3 * (u * u * (c[1].x - c[0].x) + 2 * u * t * (c[2].x - c[1].x) + t * t * (c[3].x - c[2].x)),
                   3 * (u * u * (c[1].y - c[0].y) + 2 * u * t * (c[2].y - c[1].y) + t * t * (c[3].y - c[2].y))};
        double h = (b - a) / 3 * (k ? -1 : 1);
        q[3 * k] = e;
        q[1 + k] = (Point){e.x + d.x * h, e.y + d.y * h};
    }
    memcpy(out, q, sizeof(q));
}

/* A cubic as a quadratic spline whose joins lie halfway between neighbouring
 * controls (so they go unwritten), within tolerance: the approach of cu2qu
 * (fontTools). With n pieces, piece i takes its control on the line between
 * the points where the cubic's end tangents reach 1.5 times their control
 * arms, at i / (n - 1); the fewest pieces that fit win. */
static bool spline(const Point c[4], int n, double tolerance, Point *controls) {
    if (n == 1) {
        /* One quadratic: its control where the end tangents meet. */
        Point a = {c[1].x - c[0].x, c[1].y - c[0].y}, b = {c[2].x - c[3].x, c[2].y - c[3].y};
        double cross = a.x * b.y - a.y * b.x;
        if (fabs(cross) < 1e-12) return false;
        double t = ((c[3].x - c[0].x) * b.y - (c[3].y - c[0].y) * b.x) / cross;
        controls[0] = (Point){c[0].x + a.x * t, c[0].y + a.y * t};
        Point q1 = lerp(c[0], controls[0], 2.0 / 3), q2 = lerp(c[3], controls[0], 2.0 / 3);
        return fits((Point){0, 0}, (Point){q1.x - c[1].x, q1.y - c[1].y}, (Point){q2.x - c[2].x, q2.y - c[2].y},
                     (Point){0, 0}, tolerance);
    }
    Point from = c[0], before = {0, 0};
    for (int i = 0; i < n; ++i) {
        Point p[4];
        piece(c, (double)i / n, (double)(i + 1) / n, p);
        Point near = lerp(p[0], p[1], 1.5), far = lerp(p[3], p[2], 1.5);
        controls[i] = lerp(near, far, (double)i / (n - 1));
    }
    for (int i = 0; i < n; ++i) {
        Point p[4];
        piece(c, (double)i / n, (double)(i + 1) / n, p);
        Point to = i + 1 < n ? lerp(controls[i], controls[i + 1], 0.5) : c[3];
        Point after = {to.x - p[3].x, to.y - p[3].y};
        Point q1 = lerp(from, controls[i], 2.0 / 3), q2 = lerp(to, controls[i], 2.0 / 3);
        if (length(after.x, after.y) > tolerance ||
            !fits(before, (Point){q1.x - p[1].x, q1.y - p[1].y}, (Point){q2.x - p[2].x, q2.y - p[2].y}, after, tolerance))
            return false;
        from = to;
        before = after;
    }
    return true;
}

static void cubic_to(Path *path, Point c1, Point c2, Point to) {
    Point c[4] = {path->at, c1, c2, to}, controls[64];
    if (same(c[0], c1) && same(c1, c2) && same(c2, to)) return;
    for (int n = 1; n <= 64; ++n)
        if (spline(c, n, 1, controls)) {
            for (int i = 0; i < n; ++i)
                add(path, (Segment){QUAD, i + 1 < n, controls[i], i + 1 < n ? lerp(controls[i], controls[i + 1], 0.5) : to});
            return;
        }
    fail("a cubic that 64 quadratics do not fit");
}

static void close_path(Path *path) {
    if (path->open) {
        line_to(path, path->start);
        /* A contour of a move alone draws nothing: drop it. */
        if (path->count && path->items[path->count - 1].kind == MOVE) --path->count;
    }
    path->open = false;
}

static void move_to(Path *path, Point to) {
    close_path(path);
    add(path, (Segment){MOVE, false, to, to});
    path->start = to;
    path->open = true;
}

/* ---- Fonts, through FreeType's outline walk (in its units, times font_scale). */

static Point point_of(const Path *path, const FT_Vector *v) {
    return (Point){(double)v->x * path->font_scale, (double)v->y * path->font_scale};
}
static int ft_move(const FT_Vector *to, void *user) { move_to(user, point_of(user, to)); return 0; }
static int ft_line(const FT_Vector *to, void *user) { line_to(user, point_of(user, to)); return 0; }
static int ft_conic(const FT_Vector *c, const FT_Vector *to, void *user) {
    quad_to(user, point_of(user, c), point_of(user, to));
    return 0;
}
static int ft_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user) {
    cubic_to(user, point_of(user, c1), point_of(user, c2), point_of(user, to));
    return 0;
}

/* ---- SVG icons: the d of each path, the viewBox's height as the em. */

static const char *number(const char *p, double *out) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
    char *end;
    *out = strtod(p, &end);
    return end == p ? NULL : end;
}

/* An arc's flags are single digits, perhaps with no space between them. */
static const char *flag(const char *p, double *out) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
    if (*p != '0' && *p != '1') return NULL;
    *out = *p - '0';
    return p + 1;
}

typedef struct { double x, y, width, height, scale; } Box;

static Point svg_point(const Box *box, double x, double y) {
    return (Point){(x - box->x) * box->scale, (box->y + box->height - y) * box->scale};
}

/* An elliptical arc from (x1, y1) to (x2, y2) as cubics of at most a quarter
 * turn each (SVG 1.1, F.6.5: from its endpoints to its center). */
static void arc_to(Path *path, const Box *box, double x1, double y1, double rx, double ry, double degrees, bool large,
                   bool sweep, double x2, double y2) {
    rx = fabs(rx);
    ry = fabs(ry);
    if ((x1 == x2 && y1 == y2)) return;
    if (!rx || !ry) {
        line_to(path, svg_point(box, x2, y2));
        return;
    }
    double phi = degrees * PI / 180, cp = cos(phi), sp = sin(phi);
    double hx = (x1 - x2) / 2, hy = (y1 - y2) / 2, px = cp * hx + sp * hy, py = -sp * hx + cp * hy;
    double lambda = px * px / (rx * rx) + py * py / (ry * ry);
    if (lambda > 1) { rx *= sqrt(lambda); ry *= sqrt(lambda); }
    double num = rx * rx * ry * ry - rx * rx * py * py - ry * ry * px * px, den = rx * rx * py * py + ry * ry * px * px;
    double k = (large == sweep ? -1 : 1) * sqrt(fmax(num / den, 0));
    double ccx = k * rx * py / ry, ccy = -k * ry * px / rx;
    double cx = cp * ccx - sp * ccy + (x1 + x2) / 2, cy = sp * ccx + cp * ccy + (y1 + y2) / 2;
    double start = atan2((py - ccy) / ry, (px - ccx) / rx), turn = atan2((-py - ccy) / ry, (-px - ccx) / rx) - start;
    if (sweep && turn < 0) turn += 2 * PI;
    if (!sweep && turn > 0) turn -= 2 * PI;
    int n = (int)ceil(fabs(turn) / (PI / 2) - 1e-9);
    double step = turn / n, kappa = 4.0 / 3 * tan(step / 4);
    for (int i = 0; i < n; ++i) {
        double a = start + i * step, b = a + step;
        double u[4][2] = {{cos(a), sin(a)}, {cos(a) - kappa * sin(a), sin(a) + kappa * cos(a)},
                          {cos(b) + kappa * sin(b), sin(b) - kappa * cos(b)}, {cos(b), sin(b)}};
        Point q[4];
        for (int j = 0; j < 4; ++j)
            q[j] = svg_point(box, cx + rx * cp * u[j][0] - ry * sp * u[j][1], cy + rx * sp * u[j][0] + ry * cp * u[j][1]);
        if (i == n - 1) q[3] = svg_point(box, x2, y2);
        cubic_to(path, q[1], q[2], q[3]);
    }
}

static void svg_path(Path *path, const Box *box, const char *d) {
    double cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0; /* current, subpath start, last control */
    char command = 0, last = 0;
    for (const char *p = d; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (!*p) break;
        if (strchr("MmLlHhVvQqTtCcSsZzAa", *p)) command = *p++;
        else if (!command) fail("SVG path data without a command");
        bool relative = command >= 'a';
        double ox = relative ? cx : 0, oy = relative ? cy : 0, v[7];
        char upper = (char)(command & ~32);
        int want = upper == 'M' || upper == 'L' || upper == 'T' ? 2 : upper == 'H' || upper == 'V' ? 1 :
                   upper == 'Q' || upper == 'S' ? 4 : upper == 'C' ? 6 : upper == 'A' ? 7 : 0;
        for (int i = 0; i < want; ++i)
            if (!(p = (upper == 'A' && (i == 3 || i == 4) ? flag : number)(p, &v[i]))) fail("SVG path data");
        switch (upper) {
        case 'Z':
            close_path(path);
            cx = sx; cy = sy;
            path->at = svg_point(box, cx, cy);
            command = 0; /* a number after closepath is an error */
            break;
        case 'A': {
            double x = ox + v[5], y = oy + v[6];
            arc_to(path, box, cx, cy, v[0], v[1], v[2], v[3] != 0, v[4] != 0, x, y);
            cx = x; cy = y;
            break;
        }
        case 'M':
            cx = ox + v[0]; cy = oy + v[1]; sx = cx; sy = cy;
            move_to(path, svg_point(box, cx, cy));
            command = relative ? 'l' : 'L'; /* pairs after a moveto are linetos */
            break;
        case 'L': cx = ox + v[0]; cy = oy + v[1]; line_to(path, svg_point(box, cx, cy)); break;
        case 'H': cx = ox + v[0]; line_to(path, svg_point(box, cx, cy)); break;
        case 'V': cy = (relative ? cy : 0) + v[0]; line_to(path, svg_point(box, cx, cy)); break;
        case 'Q':
            lx = ox + v[0]; ly = oy + v[1]; cx = ox + v[2]; cy = oy + v[3];
            quad_to(path, svg_point(box, lx, ly), svg_point(box, cx, cy));
            break;
        case 'T':
            lx = last == 'Q' || last == 'T' ? 2 * cx - lx : cx; ly = last == 'Q' || last == 'T' ? 2 * cy - ly : cy;
            cx = ox + v[0]; cy = oy + v[1];
            quad_to(path, svg_point(box, lx, ly), svg_point(box, cx, cy));
            break;
        case 'C': case 'S': {
            double x1, y1;
            if (upper == 'C') { x1 = ox + v[0]; y1 = oy + v[1]; }
            else { x1 = last == 'C' || last == 'S' ? 2 * cx - lx : cx; y1 = last == 'C' || last == 'S' ? 2 * cy - ly : cy; }
            int at = upper == 'C' ? 2 : 0;
            lx = ox + v[at]; ly = oy + v[at + 1];
            double x = ox + v[at + 2], y = oy + v[at + 3];
            cubic_to(path, svg_point(box, x1, y1), svg_point(box, lx, ly), svg_point(box, x, y));
            cx = x; cy = y;
            break;
        }
        }
        last = upper;
    }
    close_path(path);
}

static char *read_file(const char *name, size_t *size) {
    FILE *f = fopen(name, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *bytes = length >= 0 ? malloc((size_t)length + 1) : NULL;
    if (bytes && fread(bytes, 1, (size_t)length, f) != (size_t)length) { free(bytes); bytes = NULL; }
    fclose(f);
    if (bytes) { bytes[length] = 0; *size = (size_t)length; }
    return bytes;
}

static const char *attribute(const char *from, const char *end, const char *name) {
    size_t length = strlen(name);
    for (const char *p = from; p && p < end; ++p) {
        p = strstr(p, name);
        if (!p || p >= end) return NULL;
        if ((p == from || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t') && p[length] == '=' &&
            (p[length + 1] == '"' || p[length + 1] == '\'')) return p + length + 2;
    }
    return NULL;
}

/* Only solid, nonzero-filled paths: anything else an icon asks for would be
 * drawn wrong, so it is refused rather than dropped. */
static const char *const unsupported[] = {"transform", "opacity", "evenodd", "stroke", "fill=\"none\"", "<use",
                                          "<rect", "<circle", "<ellipse", "<poly", "<line", "<radial", "<clip",
                                          "<mask", "<image", "<text", "<filter", "<pattern"};

static void svg_icon(Path *path, const char *file, uint16_t units, int16_t *advance) {
    size_t size;
    char *svg = read_file(file, &size), why[512];
    if (!svg) fail("cannot read an SVG file");
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); ++i)
        if (strstr(svg, unsupported[i])) {
            snprintf(why, sizeof(why), "%s: icons hold only solid paths, not %s", file, unsupported[i]);
            fail(why);
        }
    const char *tag = strstr(svg, "<svg"), *tag_end = tag ? strchr(tag, '>') : NULL, *view = tag ? attribute(tag, tag_end, "viewBox") : NULL;
    /* Without a viewBox, the width and height (in px) are the user space. */
    Box box = {0};
    const char *width = tag ? attribute(tag, tag_end, "width") : NULL, *height = tag ? attribute(tag, tag_end, "height") : NULL;
    if (view ? !(view = number(view, &box.x)) || !(view = number(view, &box.y)) || !(view = number(view, &box.width)) ||
                   !number(view, &box.height)
             : !width || !height || !number(width, &box.width) || !number(height, &box.height) || box.height <= 0) {
        snprintf(why, sizeof(why), "%s: an icon needs a viewBox, or a width and height", file);
        fail(why);
    }
    box.scale = units / box.height;
    *advance = (int16_t)lround(box.width * box.scale);
    for (const char *p = svg; (p = strstr(p, "<path"));) {
        const char *end = strchr(p, '>'), *d = attribute(p, end, "d");
        if (d) {
            char quote = d[-1];
            const char *close = strchr(d, quote);
            char *data = strndup(d, (size_t)(close - d));
            svg_path(path, &box, data);
            free(data);
        }
        p = end ? end : p + 5;
    }
    free(svg);
}

/* ---- Writing. */

typedef struct { long x, y; bool off, move; } Emitted;
typedef struct { Emitted *items; size_t count, capacity; } Points;

static void put16(unsigned char *p, int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)((unsigned)v >> 8); }
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }

static void emit(Points *points, Point at, bool off, bool move) {
    if (points->count == points->capacity) {
        points->capacity = points->capacity ? 2 * points->capacity : 256;
        if (!(points->items = realloc(points->items, points->capacity * sizeof(*points->items)))) fail("out of memory");
    }
    points->items[points->count++] = (Emitted){lround(at.x), lround(at.y), off, move};
}

/* Whether s ends where its control and the next curve's would imply. */
static bool implied(const Segment *s, const Segment *next) {
    if (s->kind != QUAD || !next || next->kind != QUAD) return false;
    return s->smooth || (2 * lround(s->end.x) == lround(s->control.x) + lround(next->control.x) &&
                         2 * lround(s->end.y) == lround(s->control.y) + lround(next->control.y));
}

/* One symbol's segments as points: each contour an on point that starts it,
 * then per segment its control (if a curve) and its end, unless the next
 * curve's control implies it; the last end, back at the start, is written.
 * A contour that starts at an implied point (as FreeType starts TrueType's
 * when its first point is off) starts instead at its first written one. */
static void emit_symbol(Points *points, const Segment *segments, size_t count) {
    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        while (end < count && segments[end].kind != MOVE) ++end;
        const Segment *body = segments + i + 1;
        size_t n = end - i - 1, from = 0;
        if (n > 1 && implied(&body[n - 1], &body[0]))
            for (size_t k = 0; k + 1 < n; ++k)
                if (!implied(&body[k], &body[k + 1])) { from = k + 1; break; }
        emit(points, from ? body[from - 1].end : segments[i].end, false, true);
        for (size_t m = 0; m < n; ++m) {
            const Segment *s = &body[(from + m) % n], *next = m + 1 < n ? &body[(from + m + 1) % n] : NULL;
            const Emitted *last = &points->items[points->count - 1];
            if (s->kind == LINE) {
                if (last->off || last->x != lround(s->end.x) || last->y != lround(s->end.y)) emit(points, s->end, false, false);
                continue;
            }
            emit(points, s->control, true, false);
            if (!implied(s, next)) emit(points, s->end, false, false);
        }
        i = end;
    }
}

static int by_code(const void *a, const void *b) {
    uint32_t x = ((const Built *)a)->code, y = ((const Built *)b)->code;
    return (x > y) - (x < y);
}

static void write_symbols(const char *out, uint16_t units, Built *built, size_t count, const Path *path) {
    qsort(built, count, sizeof(*built), by_code);
    for (size_t i = 1; i < count; ++i) if (built[i].code == built[i - 1].code) fail("a code is given twice");
    Points points = {0};
    size_t *first = calloc(count + 1, sizeof(*first));
    for (size_t i = 0; i < count; ++i) {
        first[i] = points.count;
        emit_symbol(&points, path->items + built[i].first, built[i].count);
    }
    first[count] = points.count;
    /* Halve units while every coordinate is even: a TrueType font's own. */
    for (;;) {
        bool even = units % 2 == 0 && units > 16;
        for (size_t i = 0; even && i < points.count; ++i) even = points.items[i].x % 2 == 0 && points.items[i].y % 2 == 0;
        for (size_t i = 0; even && i < count; ++i) even = built[i].advance % 2 == 0;
        if (!even) break;
        for (size_t i = 0; i < points.count; ++i) { points.items[i].x /= 2; points.items[i].y /= 2; }
        for (size_t i = 0; i < count; ++i) built[i].advance = (int16_t)(built[i].advance / 2);
        units /= 2;
    }
    size_t size = VEGA_SYMBOLS_HEADER + count * VEGA_SYMBOL_BYTES + points.count * VEGA_POINT_BYTES;
    unsigned char *bytes = calloc(1, size);
    if (!bytes) fail("out of memory");
    memcpy(bytes, "VEGASYM2", 8);
    put16(bytes + 8, units);
    put32(bytes + 12, (uint32_t)count);
    put32(bytes + 16, (uint32_t)points.count);
    put32(bytes + 20, (uint32_t)size);
    unsigned char *at = bytes + VEGA_SYMBOLS_HEADER + count * VEGA_SYMBOL_BYTES;
    for (size_t i = 0; i < points.count; ++i) {
        const Emitted *e = &points.items[i];
        if (e->x < -16384 || e->x > 16383 || e->y < -16384 || e->y > 16383) fail("a coordinate does not fit 15 bits");
        put16(at + 4 * i, (int)(2 * e->x + e->off));
        put16(at + 4 * i + 2, (int)(2 * e->y + e->move));
    }
    for (size_t i = 0; i < count; ++i) {
        long left = 0, bottom = 0, right = 0, top = 0;
        for (size_t k = first[i]; k < first[i + 1]; ++k) {
            const Emitted *e = &points.items[k];
            if (k == first[i]) { left = right = e->x; bottom = top = e->y; }
            left = e->x < left ? e->x : left;
            right = e->x > right ? e->x : right;
            bottom = e->y < bottom ? e->y : bottom;
            top = e->y > top ? e->y : top;
        }
        unsigned char *p = bytes + VEGA_SYMBOLS_HEADER + i * VEGA_SYMBOL_BYTES;
        put32(p, built[i].code);
        put32(p + 4, (uint32_t)first[i]);
        put32(p + 8, (uint32_t)(first[i + 1] - first[i]));
        put16(p + 12, (int)left);
        put16(p + 14, (int)bottom);
        put16(p + 16, (int)right);
        put16(p + 18, (int)top);
        put16(p + 20, built[i].advance);
    }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(bytes, 1, size, f) != size || fclose(f)) fail("cannot write the symbol file");
    char error[128];
    VegaSymbols check;
    if (!vega_symbols_open(&check, bytes, size, error, sizeof(error))) fail(error);
    size_t curves = 0;
    VegaCurve curve;
    for (uint32_t i = 0; i < check.symbols; ++i) {
        VegaSymbol symbol = vega_symbol_at(&check, i);
        for (uint32_t k = symbol.first; k < symbol.first + symbol.count; ++k) curves += vega_symbol_curve(&check, &symbol, k, &curve);
    }
    printf("{\"symbols\":%zu,\"points\":%zu,\"curves\":%zu,\"units\":%u,\"bytes\":%zu}\n", count, points.count, curves,
           units, size);
    free(bytes);
    free(first);
    free(points.items);
}

/* 0x20-0x7e,0xac00 as a list of codes. */
static size_t codes_of(const char *text, uint32_t **out) {
    size_t count = 0, capacity = 0;
    *out = NULL;
    for (const char *p = text; *p;) {
        char *end;
        unsigned long first = strtoul(p, &end, 0), last = first;
        if (end == p) fail("--codes takes FIRST[-LAST],...");
        p = end;
        if (*p == '-') { last = strtoul(p + 1, &end, 0); p = end; }
        for (unsigned long c = first; c <= last; ++c) {
            if (count == capacity && !(*out = realloc(*out, (capacity = capacity ? 2 * capacity : 256) * sizeof(**out))))
                fail("out of memory");
            (*out)[count++] = (uint32_t)c;
        }
        if (*p == ',') ++p;
    }
    return count;
}

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

#define BUILD_USAGE "usage: vega-symbols build [--font FONT.ttf --codes 0x20-0x7e,...] [--svg CODE=ICON.svg ...] [--units N] -o OUT.vsym"

static int build(int argc, char **argv) {
    const char *font = NULL, *out = NULL, *codes = NULL, **icons = calloc((size_t)argc, sizeof(*icons));
    size_t icon_count = 0;
    long units = 0;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 == argc) fail(BUILD_USAGE);
        if (!strcmp(argv[i], "-o")) out = argv[i + 1];
        else if (!strcmp(argv[i], "--font")) font = argv[i + 1];
        else if (!strcmp(argv[i], "--codes")) codes = argv[i + 1];
        else if (!strcmp(argv[i], "--units")) units = strtol(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--svg")) icons[icon_count++] = argv[i + 1];
        else fail(BUILD_USAGE);
    }
    if (!out || !font != !codes) fail(BUILD_USAGE);
    FT_Library library = NULL;
    FT_Face face = NULL;
    if (font && (FT_Init_FreeType(&library) || FT_New_Face(library, font, 0, &face))) fail("cannot open the font");
    /* Twice a font's units by default: TrueType's implied points, halfway
     * between two control points, then land on whole units. (FreeType finds
     * them by integer halving, so outlines are doubled before it walks them.) */
    if (!units) units = face ? 2 * face->units_per_EM : 4096;
    if (units < 16 || units > 16384) fail("--units: 16 to 16384");
    uint32_t *list = NULL;
    size_t count = codes ? codes_of(codes, &list) : 0;
    Built *built = calloc(count + icon_count + 1, sizeof(*built));
    Path path = {.font_scale = face ? (double)units / (2.0 * face->units_per_EM) : 1};
    size_t made = 0;
    FT_Outline_Funcs walk = {ft_move, ft_line, ft_conic, ft_cubic, 0, 0};
    for (size_t i = 0; i < count; ++i) {
        FT_UInt glyph = FT_Get_Char_Index(face, list[i]);
        if (!glyph || FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE)) continue; /* the font lacks it */
        size_t first = path.count;
        path.open = false;
        if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Outline_Transform(&face->glyph->outline, &(FT_Matrix){0x20000, 0, 0, 0x20000});
            if (FT_Outline_Decompose(&face->glyph->outline, &walk, &path)) fail("cannot walk an outline");
        }
        close_path(&path);
        long advance = lround((double)face->glyph->metrics.horiAdvance * units / face->units_per_EM);
        built[made++] = (Built){list[i], first, path.count - first, (int16_t)(advance > INT16_MAX ? INT16_MAX : advance)};
    }
    for (size_t i = 0; i < icon_count; ++i) {
        char *equals = strchr(icons[i], '=');
        if (!equals) fail("--svg takes CODE=FILE");
        size_t first = path.count;
        path.open = false;
        int16_t advance;
        svg_icon(&path, equals + 1, (uint16_t)units, &advance);
        built[made++] = (Built){(uint32_t)strtoul(icons[i], NULL, 0), first, path.count - first, advance};
    }
    write_symbols(out, (uint16_t)units, built, made, &path);
    if (face) FT_Done_Face(face);
    if (library) FT_Done_FreeType(library);
    free(icons);
    free(list);
    free(built);
    free(path.items);
    return 0;
}

static VegaSymbols open_symbols(const char *name, char **bytes) {
    size_t size;
    char error[128];
    VegaSymbols s;
    if (!(*bytes = read_file(name, &size))) fail("cannot read the symbol file");
    if (!vega_symbols_open(&s, (unsigned char *)*bytes, size, error, sizeof(error))) fail(error);
    return s;
}

static volatile long sink; /* keeps the timed loops' reads */
static int ignore_point(const FT_Vector *to, void *user) { ++*(long *)user; (void)to; return 0; }
static int ignore_conic(const FT_Vector *c, const FT_Vector *to, void *user) { (void)c; return ignore_point(to, user); }
static int ignore_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user) {
    (void)c1;
    (void)c2;
    return ignore_point(to, user);
}

/* What reading costs, both ways: opening (verifying the symbol file; parsing
 * the font's tables) and reaching every symbol's outline. */
static void measure_open(FT_Library library, const char *font, const VegaSymbols *s) {
    size_t font_size;
    char *font_bytes = read_file(font, &font_size), error[128];
    if (!font_bytes) fail("cannot read the font");
    int rounds = 200;
    double started = seconds();
    for (int i = 0; i < rounds; ++i) {
        VegaSymbols again;
        if (!vega_symbols_open(&again, s->bytes, s->size, error, sizeof(error))) fail(error);
    }
    double open_us = (seconds() - started) / rounds * 1e6;
    started = seconds();
    for (int i = 0; i < rounds; ++i) {
        FT_Face face;
        if (FT_New_Memory_Face(library, (const FT_Byte *)font_bytes, (FT_Long)font_size, 0, &face)) fail("cannot open the font");
        FT_Done_Face(face);
    }
    double freetype_open_us = (seconds() - started) / rounds * 1e6;
    FT_Face face;
    if (FT_New_Memory_Face(library, (const FT_Byte *)font_bytes, (FT_Long)font_size, 0, &face)) fail("cannot open the font");
    FT_Outline_Funcs walk = {ignore_point, ignore_point, ignore_conic, ignore_cubic, 0, 0};
    long touched = 0, walked = 0;
    uint32_t most = 0;
    for (uint32_t i = 0; i < s->symbols; ++i) most = vega_symbol_at(s, i).count > most ? vega_symbol_at(s, i).count : most;
    VegaCurve *curves = malloc((most + 1) * sizeof(*curves));
    started = seconds();
    for (int r = 0; r < rounds; ++r)
        for (uint32_t i = 0; i < s->symbols; ++i) {
            VegaSymbol symbol;
            if (!vega_symbol(s, vega_symbol_at(s, i).code, &symbol)) fail("lookup");
            uint32_t count = vega_symbol_curves(s, &symbol, curves);
            touched += count ? (long)curves[count - 1].x2 : 0;
        }
    double outline_ns = (seconds() - started) / rounds / s->symbols * 1e9;
    started = seconds();
    for (int r = 0; r < rounds; ++r)
        for (uint32_t i = 0; i < s->symbols; ++i) {
            FT_UInt glyph = FT_Get_Char_Index(face, vega_symbol_at(s, i).code);
            if (glyph && !FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE)) FT_Outline_Decompose(&face->glyph->outline, &walk, &walked);
        }
    double freetype_outline_ns = (seconds() - started) / rounds / s->symbols * 1e9;
    printf("{\"symbol_bytes\":%zu,\"font_bytes\":%zu,\"open_us\":%.1f,\"freetype_open_us\":%.1f,\"outline_ns\":%.0f,"
           "\"freetype_outline_ns\":%.0f}\n", s->size, font_size, open_us, freetype_open_us, outline_ns, freetype_outline_ns);
    sink = touched + walked;
    FT_Done_Face(face);
    free(font_bytes);
    free(curves);
}

/* Every symbol with a glyph in the font, at each size: our coverage against
 * FreeType's (no hinting, 8-bit gray) over FreeType's bitmap. */
static int compare(int argc, char **argv) {
    if (argc < 6 || strcmp(argv[4], "--sizes")) fail("usage: vega-symbols compare FONT.ttf SYMBOLS.vsym --sizes 12,16,32,64");
    char *bytes;
    VegaSymbols s = open_symbols(argv[3], &bytes);
    FT_Library library;
    FT_Face face;
    if (FT_Init_FreeType(&library) || FT_New_Face(library, argv[2], 0, &face)) fail("cannot open the font");
    measure_open(library, argv[2], &s);
    for (const char *p = argv[5]; *p;) {
        char *end;
        long size = strtol(p, &end, 10);
        p = *end == ',' ? end + 1 : end;
        if (size <= 0 || FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size)) fail("--sizes");
        double scale = (double)size / s.units, total = 0, ours_seconds = 0;
        uint64_t pixels = 0, close = 0;
        int worst = 0, glyphs = 0;
        unsigned char *raster = NULL;
        size_t raster_size = 0;
        for (uint32_t i = 0; i < s.symbols; ++i) {
            VegaSymbol symbol = vega_symbol_at(&s, i);
            FT_UInt glyph = FT_Get_Char_Index(face, symbol.code);
            if (!glyph || FT_Load_Glyph(face, glyph, FT_LOAD_NO_HINTING | FT_LOAD_RENDER)) continue;
            FT_Bitmap *b = &face->glyph->bitmap;
            size_t need = (size_t)b->width * b->rows;
            if (!need) continue;
            if (need > raster_size && !(raster = realloc(raster, raster_size = need))) fail("out of memory");
            double started = seconds();
            vega_symbol_raster(&s, &symbol, scale, face->glyph->bitmap_left / scale, face->glyph->bitmap_top / scale,
                               (int)b->width, (int)b->rows, raster);
            ours_seconds += seconds() - started;
            for (unsigned j = 0; j < b->rows; ++j)
                for (unsigned i2 = 0; i2 < b->width; ++i2) {
                    int theirs = b->buffer[j * (unsigned)b->pitch + i2], ours = raster[j * b->width + i2];
                    int difference = abs(theirs - ours);
                    total += difference;
                    close += difference <= 16;
                    if (difference > worst) worst = difference;
                    ++pixels;
                }
            ++glyphs;
        }
        printf("{\"pixels_per_em\":%ld,\"glyphs\":%d,\"mean_difference\":%.2f,\"worst\":%d,\"within_16\":%.4f,"
               "\"reference_us_per_glyph\":%.1f}\n", size, glyphs, pixels ? total / (double)pixels : 0, worst,
               pixels ? (double)close / (double)pixels : 1, glyphs ? ours_seconds / glyphs * 1e6 : 0);
        free(raster);
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    free(bytes);
    return 0;
}

/* Each symbol's code, points, curves, bounds, advance and filled area (in square
 * units: the integral of x dy around its outline, as a check against other
 * readers of the same fonts and icons). */
static int info(int argc, char **argv) {
    if (argc < 3) fail("usage: vega-symbols info SYMBOLS.vsym");
    char *bytes;
    VegaSymbols s = open_symbols(argv[2], &bytes);
    printf("{\"units\":%u,\"symbols\":[", s.units);
    for (uint32_t i = 0; i < s.symbols; ++i) {
        VegaSymbol symbol = vega_symbol_at(&s, i);
        double area = 0;
        uint32_t curves = 0;
        VegaCurve c;
        for (uint32_t k = symbol.first; k < symbol.first + symbol.count; ++k) {
            if (!vega_symbol_curve(&s, &symbol, k, &c)) continue;
            ++curves;
            /* x(t) = x0 - 2 bx t + ax t^2, y'(t) = -2 by + 2 ay t, over [0, 1]. */
            double ax = c.x0 - 2 * c.x1 + c.x2, bx = c.x0 - c.x1, ay = c.y0 - 2 * c.y1 + c.y2, by = c.y0 - c.y1;
            area += c.x0 * -2 * by + (c.x0 * 2 * ay + 4 * bx * by) / 2 + (-4 * bx * ay - 2 * ax * by) / 3 + ax * ay / 2;
        }
        printf("%s{\"code\":%u,\"points\":%u,\"curves\":%u,\"bounds\":[%d,%d,%d,%d],\"advance\":%d,\"area\":%.1f}",
               i ? "," : "", symbol.code, symbol.count, curves, symbol.left, symbol.bottom, symbol.right, symbol.top,
               symbol.advance, area);
    }
    printf("]}\n");
    free(bytes);
    return 0;
}

static int render(int argc, char **argv) {
    if (argc < 6) fail("usage: vega-symbols render SYMBOLS.vsym CODE PIXELS OUT.pgm");
    char *bytes;
    VegaSymbols s = open_symbols(argv[2], &bytes);
    VegaSymbol symbol;
    if (!vega_symbol(&s, (uint32_t)strtoul(argv[3], NULL, 0), &symbol)) fail("no such symbol");
    double pixels = atof(argv[4]), scale = pixels / s.units;
    int width = (int)ceil((symbol.advance > symbol.right ? symbol.advance : symbol.right) * scale) + 2;
    int height = (int)ceil(pixels * 1.25) + 2;
    unsigned char *raster = malloc((size_t)width * (size_t)height);
    vega_symbol_raster(&s, &symbol, scale, -1 / scale, (pixels + 1) / scale, width, height, raster);
    FILE *f = fopen(argv[5], "wb");
    if (!f) fail("cannot write");
    fprintf(f, "P5\n%d %d\n255\n", width, height);
    fwrite(raster, 1, (size_t)width * (size_t)height, f);
    fclose(f);
    free(raster);
    free(bytes);
    return 0;
}

/* ---- The GPU: a context of its own, no window (EGL, Mesa's surfaceless platform). */

static void gl_context(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay display = platform ? platform(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    EGLint major, minor, found;
    EGLConfig config;
    EGLint wanted[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint version[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(display, wanted, &config, 1, &found))
        fail("no EGL display for OpenGL ES 3.1");
    /* With no surface, a context needs no config (EGL_KHR_no_config_context). */
    EGLContext context = eglCreateContext(display, found ? config : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, version);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        fail("no OpenGL ES 3.1 context");
}

/* A width x height target of 8-bit red, bound. */
static GLuint gl_target(int width, int height) {
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fail("no framebuffer");
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return texture;
}

/* Every symbol in a grid of cells, drawn by the GPU and by vega_coverage:
 * how far apart; then pages of text (glyphs cycling through the file)
 * drawn again and again: how long a page takes. */
static int gpu(int argc, char **argv) {
    if (argc < 5 || strcmp(argv[3], "--sizes")) fail("usage: vega-symbols gpu SYMBOLS.vsym --sizes 12,16,32,64 [--frames N]");
    int frames = argc > 6 && !strcmp(argv[5], "--frames") ? atoi(argv[6]) : 20;
    if (frames < 1) fail("--frames");
    char *bytes, error[4096];
    VegaSymbols s = open_symbols(argv[2], &bytes);
    gl_context();
    printf("{\"renderer\":\"%s\"}\n", (const char *)glGetString(GL_RENDERER));
    VegaGl gl;
    double started = seconds();
    if (!vega_gl_open(&gl, &s, error, sizeof(error))) fail(error);
    glFinish();
    double open_ms = (seconds() - started) * 1e3;
    for (const char *p = argv[4]; *p;) {
        char *end;
        long size = strtol(p, &end, 10);
        p = *end == ',' ? end + 1 : end;
        if (size <= 0 || size > 512) fail("--sizes");
        double scale = (double)size / s.units;
        /* Cells that hold any symbol, so none reaches into another's. */
        int left = 0, bottom = 0, right = 0, top = 0;
        for (uint32_t i = 0; i < s.symbols; ++i) {
            VegaSymbol symbol = vega_symbol_at(&s, i);
            left = symbol.left < left ? symbol.left : left;
            bottom = symbol.bottom < bottom ? symbol.bottom : bottom;
            right = symbol.right > right ? symbol.right : right;
            top = symbol.top > top ? symbol.top : top;
        }
        int cell = (int)ceil((right - left > top - bottom ? right - left : top - bottom) * scale) + 4;
        int across = 4096 / cell, rows = (int)((s.symbols + (uint32_t)across - 1) / (uint32_t)across);
        if (rows * cell > 4096) rows = 4096 / cell;
        int width = across * cell, height = rows * cell;
        uint32_t shown = s.symbols < (uint32_t)(across * rows) ? s.symbols : (uint32_t)(across * rows);
        VegaGlyph *glyphs = malloc(shown * sizeof(*glyphs));
        for (uint32_t i = 0; i < shown; ++i)
            glyphs[i] = (VegaGlyph){(float)(i % (uint32_t)across * (uint32_t)cell) + 2 - (float)(left * scale),
                                    (float)(i / (uint32_t)across * (uint32_t)cell) + 2 + (float)(top * scale), (float)scale, i};
        GLuint texture = gl_target(width, height);
        vega_gl_draw(&gl, &s, glyphs, shown, width, height);
        unsigned char *drawn = malloc((size_t)width * (size_t)height), *reference = malloc((size_t)cell * (size_t)cell);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, drawn);
        double total = 0;
        uint64_t pixels = 0;
        int worst = 0;
        for (uint32_t i = 0; i < shown; ++i) {
            VegaSymbol symbol = vega_symbol_at(&s, i);
            int x0 = (int)(i % (uint32_t)across) * cell, y0 = (int)(i / (uint32_t)across) * cell;
            vega_symbol_raster(&s, &symbol, scale, (x0 - glyphs[i].x) / scale, (glyphs[i].y - y0) / scale, cell, cell, reference);
            for (int j = 0; j < cell; ++j)
                for (int k = 0; k < cell; ++k) {
                    /* GL's rows run bottom up */
                    int gpu_value = drawn[(size_t)(height - 1 - (y0 + j)) * (size_t)width + (size_t)(x0 + k)];
                    int difference = abs(gpu_value - reference[j * cell + k]);
                    total += difference;
                    worst = difference > worst ? difference : worst;
                    ++pixels;
                }
        }
        /* Pages: glyphs at their advances in lines, a full target's worth. */
        int page_width = 1920, page_height = 1080;
        uint32_t page = 0, capacity = 1 << 16;
        VegaGlyph *text = malloc(capacity * sizeof(*text));
        float x = 0, line = (float)size * 1.2f;
        for (float y = line; y + (float)size * 0.3f < (float)page_height && page < capacity; ) {
            VegaSymbol symbol = vega_symbol_at(&s, page % s.symbols);
            float advance = (float)(symbol.advance * scale);
            if (x + advance > (float)page_width) { x = 0; y += line; continue; }
            text[page++] = (VegaGlyph){x, y, (float)scale, page % s.symbols};
            x += advance;
        }
        glDeleteTextures(1, &texture);
        texture = gl_target(page_width, page_height);
        vega_gl_draw(&gl, &s, text, page, page_width, page_height);
        glFinish();
        started = seconds();
        for (int f = 0; f < frames; ++f) vega_gl_draw(&gl, &s, text, page, page_width, page_height);
        glFinish();
        double page_ms = (seconds() - started) / frames * 1e3;
        printf("{\"pixels_per_em\":%ld,\"symbols\":%u,\"mean_difference\":%.3f,\"worst\":%d,\"page_glyphs\":%u,"
               "\"page_ms\":%.2f,\"upload_ms\":%.1f}\n", size, shown, pixels ? total / (double)pixels : 0, worst, page, page_ms, open_ms);
        glDeleteTextures(1, &texture);
        free(text);
        free(glyphs);
        free(drawn);
        free(reference);
    }
    vega_gl_close(&gl);
    free(bytes);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "build")) return build(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "compare")) return compare(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "info")) return info(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "render")) return render(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "gpu")) return gpu(argc, argv);
    fail("vega-symbols build|compare|info|render|gpu ...");
    return 2;
}
