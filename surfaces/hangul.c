#include "vega.h"
#include <string.h>

/* Hangul typed on a US keyboard as the standard two-set layout (두벌식): a
 * letter key gives a jamo, and jamo compose a syllable as they come. A
 * syllable is an initial consonant (19), a vowel (21) and an optional final
 * (27), which Unicode composes as U+AC00 + (initial * 21 + vowel) * 28 +
 * final. Two vowels or two finals may join into one (ㅗ+ㅏ ㅘ, ㄹ+ㄱ ㄺ); a
 * vowel after a final takes the final, or a joined final's second half, as
 * its own initial (도깨비불). A jamo alone shows as its compatibility letter. */

/* Each letter key's jamo: an initial's index + 1, or a vowel's index + 1 as
 * a negative number; lower case first, then with Shift (the doubled
 * consonants ㄲ ㄸ ㅃ ㅆ ㅉ and the vowels ㅒ ㅖ; other letters as lower case). */
static const signed char plain[26] = {
    /* a ㅁ */ 7, /* b ㅠ */ -18, /* c ㅊ */ 15, /* d ㅇ */ 12, /* e ㄷ */ 4, /* f ㄹ */ 6, /* g ㅎ */ 19,
    /* h ㅗ */ -9, /* i ㅑ */ -3, /* j ㅓ */ -5, /* k ㅏ */ -1, /* l ㅣ */ -21, /* m ㅡ */ -19, /* n ㅜ */ -14,
    /* o ㅐ */ -2, /* p ㅔ */ -6, /* q ㅂ */ 8, /* r ㄱ */ 1, /* s ㄴ */ 3, /* t ㅅ */ 10, /* u ㅕ */ -7,
    /* v ㅍ */ 18, /* w ㅈ */ 13, /* x ㅌ */ 17, /* y ㅛ */ -13, /* z ㅋ */ 16,
};
static signed char shifted(char upper) {
    switch (upper) {
    case 'Q': return 9;   /* ㅃ */
    case 'W': return 14;  /* ㅉ */
    case 'E': return 5;   /* ㄸ */
    case 'R': return 2;   /* ㄲ */
    case 'T': return 11;  /* ㅆ */
    case 'O': return -4;  /* ㅒ */
    case 'P': return -8;  /* ㅖ */
    default: return plain[upper - 'A'];
    }
}

/* An initial as a final (0: it cannot end a syllable: ㄸ ㅃ ㅉ), and a
 * single final as an initial. */
static const unsigned char final_of[19] = {1, 2, 4, 7, 0, 8, 16, 17, 0, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27};
static signed char initial_of(unsigned final) {
    for (int i = 0; i < 19; ++i)
        if (final_of[i] == final && final) return (signed char)i;
    return -1;
}

/* Joined finals: the first final, the initial added, the joined final. */
static const unsigned char joined_finals[][3] = {
    {1, 9, 3},   {4, 12, 5},  {4, 18, 6},  {8, 0, 9},   {8, 6, 10},  {8, 7, 11},
    {8, 9, 12},  {8, 16, 13}, {8, 17, 14}, {8, 18, 15}, {17, 9, 18},
};
/* Joined vowels: the first vowel, the vowel added, the joined vowel. */
static const unsigned char joined_vowels[][3] = {{8, 0, 9}, {8, 1, 10}, {8, 20, 11}, {13, 4, 14}, {13, 5, 15},
                                                 {13, 20, 16}, {18, 20, 19}};

/* The compatibility letters (U+3131..) of the initials; the vowels' are
 * U+314F + index. */
static const uint16_t initial_letters[19] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141,
                                             0x3142, 0x3143, 0x3145, 0x3146, 0x3147, 0x3148, 0x3149,
                                             0x314A, 0x314B, 0x314C, 0x314D, 0x314E};

void vega_hangul_reset(VegaHangul *h) { *h = (VegaHangul){-1, -1, 0, 0, {{0}}}; }

/* The syllable (or lone jamo) as UTF-8 into out: its bytes (0 when none). */
static size_t spell(signed char initial, signed char vowel, unsigned char final, char out[4]) {
    uint32_t c = initial >= 0 && vowel >= 0 ? 0xAC00u + ((unsigned)initial * 21 + (unsigned)vowel) * 28 + final
                 : initial >= 0             ? initial_letters[initial]
                 : vowel >= 0               ? 0x314Fu + (unsigned)vowel
                                            : 0;
    if (!c) return 0;
    out[0] = (char)(0xE0 | c >> 12);
    out[1] = (char)(0x80 | (c >> 6 & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

size_t vega_hangul_composing(const VegaHangul *h, char out[4]) { return spell(h->initial, h->vowel, h->final, out); }

size_t vega_hangul_finish(VegaHangul *h, char out[4]) {
    size_t length = vega_hangul_composing(h, out);
    vega_hangul_reset(h);
    return length;
}

/* What it composed so far is kept for Backspace, then it changes. */
static void step(VegaHangul *h, signed char initial, signed char vowel, unsigned char final) {
    if (h->steps < sizeof(h->before) / sizeof(*h->before))
        h->before[h->steps++] = (struct VegaJamo){h->initial, h->vowel, h->final};
    h->initial = initial, h->vowel = vowel, h->final = final;
}

/* A new syllable from here: what was composed is finished into done. */
static size_t begin(VegaHangul *h, signed char initial, signed char vowel, char done[4]) {
    size_t length = vega_hangul_finish(h, done);
    step(h, initial, vowel, 0);
    return length;
}

bool vega_hangul_type(VegaHangul *h, char key, char done[4], size_t *done_length) {
    *done_length = 0;
    signed char jamo = key >= 'a' && key <= 'z' ? plain[key - 'a'] : key >= 'A' && key <= 'Z' ? shifted(key) : 0;
    if (!jamo) return false;
    if (jamo > 0) { /* a consonant */
        signed char consonant = (signed char)(jamo - 1);
        if (h->vowel < 0 || h->initial < 0) { /* nothing, a lone initial or a lone vowel: this one starts anew */
            if (h->initial < 0 && h->vowel < 0) step(h, consonant, -1, 0);
            else *done_length = begin(h, consonant, -1, done);
        } else if (!h->final && final_of[consonant]) step(h, h->initial, h->vowel, final_of[consonant]);
        else {
            unsigned char joined = 0;
            for (size_t i = 0; i < sizeof(joined_finals) / sizeof(*joined_finals) && !joined; ++i)
                if (joined_finals[i][0] == h->final && joined_finals[i][1] == (unsigned char)consonant) joined = joined_finals[i][2];
            if (joined) step(h, h->initial, h->vowel, joined);
            else *done_length = begin(h, consonant, -1, done);
        }
        return true;
    }
    signed char vowel = (signed char)(-jamo - 1);
    if (h->final) { /* 도깨비불: the final (or its second half) moves on to start the next syllable */
        signed char moved = -1;
        unsigned char kept = 0;
        for (size_t i = 0; i < sizeof(joined_finals) / sizeof(*joined_finals); ++i)
            if (joined_finals[i][2] == h->final) kept = joined_finals[i][0], moved = (signed char)joined_finals[i][1];
        if (moved < 0) moved = initial_of(h->final);
        char out[4];
        *done_length = spell(h->initial, h->vowel, kept, out);
        memcpy(done, out, *done_length);
        vega_hangul_reset(h);
        step(h, moved, -1, 0); /* so Backspace leaves the initial first */
        step(h, moved, vowel, 0);
        return true;
    }
    if (h->vowel >= 0) {
        unsigned char joined = 0;
        for (size_t i = 0; i < sizeof(joined_vowels) / sizeof(*joined_vowels) && !joined; ++i)
            if (joined_vowels[i][0] == (unsigned char)h->vowel && joined_vowels[i][1] == (unsigned char)vowel)
                joined = joined_vowels[i][2];
        if (joined) step(h, h->initial, (signed char)joined, 0);
        else *done_length = begin(h, -1, vowel, done);
        return true;
    }
    step(h, h->initial, vowel, 0); /* after an initial, or alone */
    return true;
}

bool vega_hangul_back(VegaHangul *h) {
    if (h->initial < 0 && h->vowel < 0) return false;
    if (!h->steps) { vega_hangul_reset(h); return true; }
    struct VegaJamo before = h->before[--h->steps];
    h->initial = before.initial, h->vowel = before.vowel, h->final = before.final;
    return true;
}
