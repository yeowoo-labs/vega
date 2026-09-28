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

int main(int argc, char **argv) {
    const char *ucd = argc > 1 ? argv[1] : "/usr/share/unicode";
    char path[1024];
    snprintf(path, sizeof(path), "%s/BidiTest.txt", ucd);
    size_t bidi = bidi_test(path);
    snprintf(path, sizeof(path), "%s/BidiCharacterTest.txt", ucd);
    size_t characters = bidi_character_test(path);
    printf("{\"bidi_test_cases\":%zu,\"bidi_character_test_cases\":%zu,\"failures\":%u}\n", bidi, characters, failures);
    return failures ? 1 : 0;
}
