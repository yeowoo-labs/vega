#define _POSIX_C_SOURCE 200809L
/* vega-symbols: build symbol files from fonts and SVG icons, and measure them
 * against FreeType (docs/design/002-symbol-format.md).
 *   vega-symbols build [--font FONT.ttf --codes 0x20-0x7e,...] [--svg CODE=ICON.svg ...] [--units N] -o OUT.vsym
 *   vega-symbols compare FONT.ttf OUT.vsym --sizes 12,16,32,64
 *   vega-symbols info OUT.vsym
 *   vega-symbols render OUT.vsym CODE PIXELS OUT.pgm */
#include "symbols.h"
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
typedef struct { Point p[3]; } Quad;
typedef struct { Quad *items; size_t count, capacity; Point at, start; bool open; double font_scale; } Path;
typedef struct { uint32_t code; size_t first, count; int16_t advance; } Built;

static void fail(const char *why) {
    fprintf(stderr, "vega-symbols: %s\n", why);
    exit(1);
}

static void add_quad(Path *path, Point a, Point b, Point c) {
    if (path->count == path->capacity) {
        path->capacity = path->capacity ? 2 * path->capacity : 64;
        if (!(path->items = realloc(path->items, path->capacity * sizeof(*path->items)))) fail("out of memory");
    }
    path->items[path->count++] = (Quad){{a, b, c}};
}

/* A line is a quadratic whose control point is its middle. */
static void line_to(Path *path, Point to) {
    if (to.x != path->at.x || to.y != path->at.y)
        add_quad(path, path->at, (Point){(path->at.x + to.x) / 2, (path->at.y + to.y) / 2}, to);
    path->at = to;
}

static void quad_to(Path *path, Point control, Point to) {
    add_quad(path, path->at, control, to);
    path->at = to;
}

/* A cubic as quadratics, within half a unit: split in n equal pieces, each
 * approximated by the quadratic through its ends with the control point
 * (3(c1 + c2) - (p0 + p3)) / 4. The error shrinks with the cube of n. */
static void cubic_to(Path *path, Point c1, Point c2, Point to) {
    Point p0 = path->at;
    double dx = to.x - 3 * c2.x + 3 * c1.x - p0.x, dy = to.y - 3 * c2.y + 3 * c1.y - p0.y;
    double error = sqrt(3) / 36 * sqrt(dx * dx + dy * dy);
    int n = (int)ceil(cbrt(error / 0.5));
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i) {
        double t0 = (double)i / n, t1 = (double)(i + 1) / n;
        Point q[4];
        double ts[2] = {t0, t1};
        /* The piece's points by de Casteljau at t0 and t1, and its control points. */
        Point e[2];
        for (int k = 0; k < 2; ++k) {
            double t = ts[k], u = 1 - t;
            e[k] = (Point){u * u * u * p0.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * to.x,
                           u * u * u * p0.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * to.y};
        }
        /* Derivatives give the piece's cubic control points. */
        double h = t1 - t0;
        for (int k = 0; k < 2; ++k) {
            double t = ts[k], u = 1 - t;
            Point d = {3 * (u * u * (c1.x - p0.x) + 2 * u * t * (c2.x - c1.x) + t * t * (to.x - c2.x)),
                       3 * (u * u * (c1.y - p0.y) + 2 * u * t * (c2.y - c1.y) + t * t * (to.y - c2.y))};
            q[k ? 2 : 1] = k ? (Point){e[1].x - d.x * h / 3, e[1].y - d.y * h / 3} : (Point){e[0].x + d.x * h / 3, e[0].y + d.y * h / 3};
        }
        q[0] = e[0];
        q[3] = e[1];
        Point control = {(3 * (q[1].x + q[2].x) - (q[0].x + q[3].x)) / 4, (3 * (q[1].y + q[2].y) - (q[0].y + q[3].y)) / 4};
        path->at = q[0];
        quad_to(path, control, q[3]);
    }
    path->at = to;
}

static void close_path(Path *path) {
    if (path->open) line_to(path, path->start);
    path->open = false;
}

static void move_to(Path *path, Point to) {
    close_path(path);
    path->at = path->start = to;
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

static void put16(unsigned char *p, int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)((unsigned)v >> 8); }
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }

static int16_t unit(double v) {
    long r = lround(v);
    if (r < INT16_MIN || r > INT16_MAX) fail("a coordinate does not fit 16 bits");
    return (int16_t)r;
}

static int by_code(const void *a, const void *b) {
    uint32_t x = ((const Built *)a)->code, y = ((const Built *)b)->code;
    return (x > y) - (x < y);
}

static void write_symbols(const char *out, uint16_t units, Built *built, size_t count, const Path *path) {
    qsort(built, count, sizeof(*built), by_code);
    for (size_t i = 1; i < count; ++i) if (built[i].code == built[i - 1].code) fail("a code is given twice");
    size_t size = VEGA_SYMBOLS_HEADER + count * VEGA_SYMBOL_BYTES + path->count * VEGA_CURVE_BYTES;
    unsigned char *bytes = calloc(1, size);
    if (!bytes) fail("out of memory");
    memcpy(bytes, "VEGASYM1", 8);
    put16(bytes + 8, units);
    put32(bytes + 12, (uint32_t)count);
    put32(bytes + 16, (uint32_t)path->count);
    put32(bytes + 20, (uint32_t)size);
    unsigned char *curves = bytes + VEGA_SYMBOLS_HEADER + count * VEGA_SYMBOL_BYTES;
    uint32_t next = 0;
    for (size_t i = 0; i < count; ++i) {
        int16_t left = 0, bottom = 0, right = 0, top = 0;
        for (size_t k = 0; k < built[i].count; ++k) {
            const Quad *q = &path->items[built[i].first + k];
            unsigned char *c = curves + (size_t)(next + k) * VEGA_CURVE_BYTES;
            for (int j = 0; j < 3; ++j) {
                int16_t x = unit(q->p[j].x), y = unit(q->p[j].y);
                put16(c + 4 * j, x);
                put16(c + 4 * j + 2, y);
                if (!k && !j) { left = right = x; bottom = top = y; }
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < bottom) bottom = y;
                if (y > top) top = y;
            }
        }
        unsigned char *s = bytes + VEGA_SYMBOLS_HEADER + i * VEGA_SYMBOL_BYTES;
        put32(s, built[i].code);
        put32(s + 4, next);
        put32(s + 8, (uint32_t)built[i].count);
        put16(s + 12, left);
        put16(s + 14, bottom);
        put16(s + 16, right);
        put16(s + 18, top);
        put16(s + 20, built[i].advance);
        next += (uint32_t)built[i].count;
    }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(bytes, 1, size, f) != size || fclose(f)) fail("cannot write the symbol file");
    free(bytes);
    char error[128];
    VegaSymbols check;
    size_t read_size;
    char *again = read_file(out, &read_size);
    if (!again || !vega_symbols_open(&check, (unsigned char *)again, read_size, error, sizeof(error))) fail(error);
    free(again);
    printf("{\"symbols\":%zu,\"curves\":%zu,\"bytes\":%zu}\n", count, path->count, size);
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
        long advance = lround(face->glyph->metrics.horiAdvance * path.font_scale);
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
    started = seconds();
    for (int r = 0; r < rounds; ++r)
        for (uint32_t i = 0; i < s->symbols; ++i) {
            VegaSymbol symbol;
            if (!vega_symbol(s, vega_symbol_at(s, i).code, &symbol)) fail("lookup");
            for (uint32_t k = symbol.first; k < symbol.first + symbol.count; ++k) touched += vega_curve(s, k).x2;
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

/* Each symbol's code, curves, bounds, advance and filled area (in square
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
        for (uint32_t k = symbol.first; k < symbol.first + symbol.count; ++k) {
            VegaCurve c = vega_curve(&s, k);
            /* x(t) = x0 - 2 bx t + ax t^2, y'(t) = -2 by + 2 ay t, over [0, 1]. */
            double ax = c.x0 - 2 * c.x1 + c.x2, bx = c.x0 - c.x1, ay = c.y0 - 2 * c.y1 + c.y2, by = c.y0 - c.y1;
            area += c.x0 * -2 * by + (c.x0 * 2 * ay + 4 * bx * by) / 2 + (-4 * bx * ay - 2 * ax * by) / 3 + ax * ay / 2;
        }
        printf("%s{\"code\":%u,\"curves\":%u,\"bounds\":[%d,%d,%d,%d],\"advance\":%d,\"area\":%.1f}", i ? "," : "",
               symbol.code, symbol.count, symbol.left, symbol.bottom, symbol.right, symbol.top, symbol.advance, area);
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

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "build")) return build(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "compare")) return compare(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "info")) return info(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "render")) return render(argc, argv);
    fail("vega-symbols build|compare|info|render ...");
    return 2;
}
