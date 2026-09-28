/* Normalization forms D and C (UAX #15, Unicode 15.1): text/text.h. */
#include "text.h"
#include <string.h>

/* Hangul syllables are made of jamo arithmetically (Unicode section 3.12). */
enum { SBASE = 0xAC00, LBASE = 0x1100, VBASE = 0x1161, TBASE = 0x11A7, LCOUNT = 19, VCOUNT = 21, TCOUNT = 28,
       NCOUNT = VCOUNT * TCOUNT, SCOUNT = LCOUNT * NCOUNT };

size_t vega_nfd(const uint32_t *in, size_t count, uint32_t *out) {
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t code = in[i];
        if (code >= SBASE && code < SBASE + SCOUNT) {
            uint32_t s = code - SBASE;
            out[n++] = LBASE + s / NCOUNT;
            out[n++] = VBASE + s % NCOUNT / TCOUNT;
            if (s % TCOUNT) out[n++] = TBASE + s % TCOUNT;
            continue;
        }
        const uint32_t *parts;
        size_t length = vega_decomposition(code, &parts);
        if (length) {
            memcpy(out + n, parts, length * sizeof(*out));
            n += length;
        } else
            out[n++] = code;
    }
    /* Canonical order: each mark moves before the marks of a higher class
     * ahead of it, up to a starter (a stable sort of every run of marks). */
    for (size_t i = 1; i < n; ++i) {
        uint8_t class = vega_combining_class(out[i]);
        if (!class) continue;
        for (size_t j = i; j && vega_combining_class(out[j - 1]) > class; --j) {
            uint32_t swap = out[j - 1];
            out[j - 1] = out[j];
            out[j] = swap;
        }
    }
    return n;
}

static uint32_t compose(uint32_t first, uint32_t second) {
    if (first >= LBASE && first < LBASE + LCOUNT && second >= VBASE && second < VBASE + VCOUNT)
        return SBASE + ((first - LBASE) * VCOUNT + (second - VBASE)) * TCOUNT;
    if (first >= SBASE && first < SBASE + SCOUNT && (first - SBASE) % TCOUNT == 0 && second > TBASE &&
        second < TBASE + TCOUNT)
        return first + (second - TBASE);
    return vega_composition(first, second);
}

size_t vega_nfc(const uint32_t *in, size_t count, uint32_t *out) {
    size_t n = vega_nfd(in, count, out);
    if (!n) return 0;
    /* Each character after the last starter composes with it unless a
     * character between them blocks it (one of the same or a higher class,
     * or a starter). */
    size_t starter = 0, written = 1;
    int last = vega_combining_class(out[0]) ? 256 : 0;
    for (size_t i = 1; i < n; ++i) {
        uint32_t code = out[i];
        int class = vega_combining_class(code);
        uint32_t composite = compose(out[starter], code);
        if (composite && (last < class || last == 0)) {
            out[starter] = composite;
            continue;
        }
        if (!class) starter = written;
        last = class;
        out[written++] = code;
    }
    return written;
}
