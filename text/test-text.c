/* Vega's text against Unicode's conformance tests (docs/design/004-text.md):
 *     test-text UCD-DIRECTORY
 * BidiTest.txt (Bidi_Class sequences, each paragraph direction its bitset
 * names) and BidiCharacterTest.txt (code points, paired brackets included):
 * every line's levels and visual order. Prints one JSON line of counts;
 * exits 1 on the first failures (up to ten shown). */
#define _POSIX_C_SOURCE 200809L
#include "text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX = 512 };
static unsigned failures;

static void fail(const char *file, size_t line, const char *what) {
    if (failures++ < 10) fprintf(stderr, "%s:%zu: %s\n", file, line, what);
}

static const char *const names[VEGA_BIDI_CLASSES] = {
    "L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
    "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"};

static int class_named(const char *name) {
    for (int c = 0; c < VEGA_BIDI_CLASSES; ++c)
        if (!strcmp(names[c], name)) return c;
    return -1;
}

/* Levels as the tests write them ("x" for removed), into levels; how many. */
static size_t parse_levels(char *text, uint8_t *levels) {
    size_t n = 0;
    for (char *token = strtok(text, " \t\r\n"); token && n < MAX; token = strtok(NULL, " \t\r\n"))
        levels[n++] = token[0] == 'x' ? VEGA_BIDI_REMOVED : (uint8_t)atoi(token);
    return n;
}

static size_t parse_order(char *text, uint32_t *order) {
    size_t n = 0;
    for (char *token = strtok(text, " \t\r\n"); token && n < MAX; token = strtok(NULL, " \t\r\n"))
        order[n++] = (uint32_t)strtoul(token, NULL, 10);
    return n;
}

static bool same_result(const uint8_t *levels, const uint8_t *want, size_t count, const uint32_t *order,
                        size_t order_count) {
    if (memcmp(levels, want, count)) return false;
    uint32_t got[MAX];
    return vega_bidi_order(levels, count, got) == order_count && !memcmp(got, order, order_count * sizeof(*got));
}

static size_t bidi_test(const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) { perror(path); exit(2); }
    char line[4096];
    uint8_t want[MAX], levels[MAX];
    uint32_t order[MAX];
    size_t want_count = 0, order_count = 0, cases = 0, number = 0;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (!strncmp(line, "@Levels:", 8)) { want_count = parse_levels(line + 8, want); continue; }
        if (!strncmp(line, "@Reorder:", 9)) { order_count = parse_order(line + 9, order); continue; }
        if (line[0] == '@') continue;
        char *semicolon = strchr(line, ';');
        if (!semicolon) continue;
        *semicolon = 0;
        unsigned bitset = (unsigned)strtoul(semicolon + 1, NULL, 16);
        VegaBidiClass classes[MAX];
        size_t count = 0;
        for (char *token = strtok(line, " \t"); token && count < MAX; token = strtok(NULL, " \t")) {
            int c = class_named(token);
            if (c < 0) { fail(path, number, "unknown class"); break; }
            classes[count++] = (VegaBidiClass)c;
        }
        if (count != want_count) { fail(path, number, "levels of another length"); continue; }
        static const int directions[3] = {VEGA_BIDI_AUTO, VEGA_BIDI_LTR, VEGA_BIDI_RTL};
        for (unsigned d = 0; d < 3; ++d) {
            if (!(bitset >> d & 1)) continue;
            ++cases;
            if (vega_bidi_levels(classes, NULL, count, directions[d], levels) < 0) { fail(path, number, "no memory"); continue; }
            if (!same_result(levels, want, count, order, order_count)) {
                char what[64];
                snprintf(what, sizeof(what), "direction %u differs", d);
                fail(path, number, what);
            }
        }
    }
    fclose(in);
    return cases;
}

static size_t bidi_character_test(const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) { perror(path); exit(2); }
    char line[8192];
    size_t cases = 0, number = 0;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *field[5];
        field[0] = line;
        for (unsigned f = 1; f < 5; ++f) {
            field[f] = strchr(field[f - 1], ';');
            if (!field[f]) break;
            *field[f]++ = 0;
        }
        uint32_t codes[MAX], order[MAX];
        VegaBidiClass classes[MAX];
        uint8_t want[MAX], levels[MAX];
        size_t count = 0;
        for (char *token = strtok(field[0], " \t"); token && count < MAX; token = strtok(NULL, " \t")) {
            codes[count] = (uint32_t)strtoul(token, NULL, 16);
            classes[count] = vega_bidi_class(codes[count]);
            ++count;
        }
        int direction = atoi(field[1]), paragraph = atoi(field[2]);
        size_t want_count = parse_levels(field[3], want), order_count = parse_order(field[4], order);
        ++cases;
        int got = vega_bidi_levels(classes, codes, count, direction == 0 ? VEGA_BIDI_LTR : direction == 1 ? VEGA_BIDI_RTL : VEGA_BIDI_AUTO,
                                   levels);
        if (want_count != count) fail(path, number, "levels of another length");
        else if (got != paragraph) fail(path, number, "paragraph level differs");
        else if (!same_result(levels, want, count, order, order_count)) fail(path, number, "levels or order differ");
    }
    fclose(in);
    return cases;
}

/* NormalizationTest.txt (through bzcat): for each line c1;c2;c3;c4;c5,
 * NFC(c1) = NFC(c2) = NFC(c3) = c2, NFC(c4) = NFC(c5) = c4, NFD(c1) =
 * NFD(c2) = NFD(c3) = c3 and NFD(c4) = NFD(c5) = c5; and every code point
 * part 1 does not list is its own NFC and NFD. */
static size_t parse_codes(const char *text, uint32_t *codes) {
    size_t n = 0;
    for (char *end; *text && n < MAX; text = end) {
        unsigned long code = strtoul(text, &end, 16);
        if (end == text) break;
        codes[n++] = (uint32_t)code;
    }
    return n;
}

static bool normalizes(size_t (*form)(const uint32_t *, size_t, uint32_t *), const uint32_t *in, size_t count,
                       const uint32_t *want, size_t want_count) {
    uint32_t out[4 * MAX];
    size_t n = form(in, count, out);
    return n == want_count && !memcmp(out, want, n * sizeof(*out));
}

static size_t normalization_test(const char *path, size_t *invariant) {
    char command[1200];
    snprintf(command, sizeof(command), "bzcat '%s'", path);
    FILE *in = popen(command, "r");
    if (!in) { perror(path); exit(2); }
    static bool listed[0x110000];
    char line[4096];
    size_t cases = 0, number = 0;
    int part = -1;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (!strncmp(line, "@Part", 5)) { part = atoi(line + 5); continue; }
        uint32_t c[5][MAX];
        size_t n[5];
        char *field = line;
        for (unsigned f = 0; f < 5; ++f) {
            char *end = strchr(field, ';');
            if (!end) break;
            *end = 0;
            n[f] = parse_codes(field, c[f]);
            field = end + 1;
        }
        if (part == 1 && n[0] == 1) listed[c[0][0]] = true;
        ++cases;
        bool ok = true;
        for (unsigned f = 0; f < 3; ++f) {
            ok = ok && normalizes(vega_nfc, c[f], n[f], c[1], n[1]);
            ok = ok && normalizes(vega_nfd, c[f], n[f], c[2], n[2]);
        }
        for (unsigned f = 3; f < 5; ++f) {
            ok = ok && normalizes(vega_nfc, c[f], n[f], c[3], n[3]);
            ok = ok && normalizes(vega_nfd, c[f], n[f], c[4], n[4]);
        }
        if (!ok) fail(path, number, "a form differs");
    }
    pclose(in);
    *invariant = 0;
    for (uint32_t code = 0; code < 0x110000; ++code) {
        if (listed[code] || (code >= 0xD800 && code <= 0xDFFF)) continue;
        ++*invariant;
        if (!normalizes(vega_nfc, &code, 1, &code, 1) || !normalizes(vega_nfd, &code, 1, &code, 1)) {
            char what[64];
            snprintf(what, sizeof(what), "U+%04X is not its own normal form", code);
            fail(path, 0, what);
        }
    }
    return cases;
}

/* LineBreakTest.txt (auxiliary/): code points with ÷ where a line may
 * break and × where it may not, the ends included. */
static size_t line_break_test(const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) { perror(path); exit(2); }
    static char line[65536]; /* the comments name every character */
    size_t cases = 0, number = 0;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        uint32_t codes[MAX];
        uint8_t want[MAX + 1], got[MAX + 1];
        size_t count = 0, marks = 0;
        for (char *p = line; *p && count < MAX;) {
            if (!strncmp(p, "\xC3\xB7", 2)) { want[marks++] = 1; p += 2; }        /* ÷ */
            else if (!strncmp(p, "\xC3\x97", 2)) { want[marks++] = 0; p += 2; }   /* × */
            else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')) codes[count++] = (uint32_t)strtoul(p, &p, 16);
            else ++p;
        }
        if (!count) continue;
        ++cases;
        if (marks != count + 1) { fail(path, number, "a line not read"); continue; }
        if (!vega_line_breaks(codes, count, got)) { fail(path, number, "no memory"); continue; }
        for (size_t i = 0; i <= count; ++i)
            if ((got[i] != VEGA_BREAK_NONE) != want[i]) {
                char what[64];
                snprintf(what, sizeof(what), "boundary %zu: %s", i, want[i] ? "a break missed" : "a break too many");
                fail(path, number, what);
                break;
            }
    }
    fclose(in);
    return cases;
}

/* GraphemeBreakTest.txt and WordBreakTest.txt: the same notation, for
 * grapheme clusters and words. */
static bool graphemes(const uint32_t *codes, size_t count, uint8_t *breaks) {
    vega_graphemes(codes, count, breaks);
    return true;
}

static size_t segment_test(const char *path, bool (*segment)(const uint32_t *, size_t, uint8_t *)) {
    FILE *in = fopen(path, "r");
    if (!in) { perror(path); exit(2); }
    static char line[65536];
    size_t cases = 0, number = 0;
    while (fgets(line, sizeof(line), in)) {
        ++number;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        uint32_t codes[MAX];
        uint8_t want[MAX + 1], got[MAX + 1];
        size_t count = 0, marks = 0;
        for (char *p = line; *p && count < MAX;) {
            if (!strncmp(p, "\xC3\xB7", 2)) { want[marks++] = 1; p += 2; }
            else if (!strncmp(p, "\xC3\x97", 2)) { want[marks++] = 0; p += 2; }
            else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')) codes[count++] = (uint32_t)strtoul(p, &p, 16);
            else ++p;
        }
        if (!count) continue;
        ++cases;
        if (marks != count + 1) { fail(path, number, "a line not read"); continue; }
        if (!segment(codes, count, got)) { fail(path, number, "no memory"); continue; }
        for (size_t i = 0; i <= count; ++i)
            if (got[i] != want[i]) {
                char what[64];
                snprintf(what, sizeof(what), "boundary %zu: %s", i, want[i] ? "a break missed" : "a break too many");
                fail(path, number, what);
                break;
            }
    }
    fclose(in);
    return cases;
}

int main(int argc, char **argv) {
    const char *ucd = argc > 1 ? argv[1] : "/usr/share/unicode";
    char path[1024];
    snprintf(path, sizeof(path), "%s/BidiTest.txt", ucd);
    size_t bidi = bidi_test(path);
    snprintf(path, sizeof(path), "%s/BidiCharacterTest.txt", ucd);
    size_t characters = bidi_character_test(path), invariant;
    snprintf(path, sizeof(path), "%s/NormalizationTest.txt.bz2", ucd);
    size_t normalized = normalization_test(path, &invariant);
    snprintf(path, sizeof(path), "%s/auxiliary/LineBreakTest.txt", ucd);
    size_t line_breaks = line_break_test(path);
    snprintf(path, sizeof(path), "%s/auxiliary/GraphemeBreakTest.txt", ucd);
    size_t grapheme_cases = segment_test(path, graphemes);
    snprintf(path, sizeof(path), "%s/auxiliary/WordBreakTest.txt", ucd);
    size_t word_cases = segment_test(path, vega_words);
    snprintf(path, sizeof(path), "%s/auxiliary/SentenceBreakTest.txt", ucd);
    size_t sentence_cases = segment_test(path, vega_sentences);
    printf("{\"bidi_test_cases\":%zu,\"bidi_character_test_cases\":%zu,\"normalization_test_cases\":%zu,"
           "\"normalization_invariant_code_points\":%zu,\"line_break_test_cases\":%zu,\"grapheme_test_cases\":%zu,"
           "\"word_test_cases\":%zu,\"sentence_test_cases\":%zu,\"failures\":%u}\n", bidi, characters, normalized,
           invariant, line_breaks, grapheme_cases, word_cases, sentence_cases, failures);
    return failures ? 1 : 0;
}
