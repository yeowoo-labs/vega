/* The Unicode Bidirectional Algorithm (UAX #9, Unicode 15.1), for one
 * paragraph laid out as one line: text/text.h. Rules are named as in the
 * annex; the tests are Unicode's own (BidiTest.txt, BidiCharacterTest.txt). */
#include "text.h"
#include <stdlib.h>
#include <string.h>

enum { MAX_DEPTH = 125, BRACKETS = 63 };
typedef VegaBidiClass Class;

static bool initiator(Class c) { return c == VEGA_BIDI_LRI || c == VEGA_BIDI_RLI || c == VEGA_BIDI_FSI; }
static bool removed(Class c) {
    return c == VEGA_BIDI_RLE || c == VEGA_BIDI_LRE || c == VEGA_BIDI_RLO || c == VEGA_BIDI_LRO || c == VEGA_BIDI_PDF ||
           c == VEGA_BIDI_BN;
}
/* Neutral or isolate formatting character (NI). */
static bool neutral(Class c) {
    return c == VEGA_BIDI_B || c == VEGA_BIDI_S || c == VEGA_BIDI_WS || c == VEGA_BIDI_ON || initiator(c) ||
           c == VEGA_BIDI_PDI;
}
/* A strong direction for N0 and N1: L, or R for R, AL, EN and AN; else -1. */
static int strong(Class c) {
    if (c == VEGA_BIDI_L) return VEGA_BIDI_L;
    if (c == VEGA_BIDI_R || c == VEGA_BIDI_AL || c == VEGA_BIDI_EN || c == VEGA_BIDI_AN) return VEGA_BIDI_R;
    return -1;
}

/* P2-P3 over [from, to): 1 when the first strong character (skipping
 * isolates to their matching PDI) is R or AL, 0 when it is L, -1 when there
 * is none. */
static int first_strong(const Class *t, const size_t *match, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        if (t[i] == VEGA_BIDI_L) return 0;
        if (t[i] == VEGA_BIDI_R || t[i] == VEGA_BIDI_AL) return 1;
        if (initiator(t[i])) {
            if (match[i] >= to) return -1;
            i = match[i];
        }
    }
    return -1;
}

/* U+2329 and U+232A are canonically U+3008 and U+3009 (BD16). */
static uint32_t canonical(uint32_t code) { return code == 0x2329 ? 0x3008 : code == 0x232A ? 0x3009 : code; }

typedef struct { size_t open, close; } Pair; /* positions in a sequence of an opening bracket and its closing one */

typedef struct {
    const Class *original;
    Class *t;
    uint8_t *levels;
    const uint32_t *codes;
    size_t *seq; /* the isolating run sequence's characters */
    size_t count;
    uint8_t level;
    Class sos, eos;
    Pair *pairs; /* room for count / 2 + 1 */
} Sequence;

static int by_opening(const void *a, const void *b) {
    const Pair *x = a, *y = b;
    return (x->open > y->open) - (x->open < y->open);
}

static Class before(const Sequence *s, size_t k) { return k ? s->t[s->seq[k - 1]] : s->sos; }

static void weak_types(Sequence *s) {
    Class *t = s->t;
    size_t *seq = s->seq, m = s->count;
    for (size_t k = 0; k < m; ++k) /* W1 */
        if (t[seq[k]] == VEGA_BIDI_NSM) {
            Class previous = before(s, k);
            t[seq[k]] = initiator(previous) || previous == VEGA_BIDI_PDI ? VEGA_BIDI_ON : previous;
        }
    Class last = s->sos; /* W2, W3 */
    for (size_t k = 0; k < m; ++k) {
        Class c = t[seq[k]];
        if (c == VEGA_BIDI_L || c == VEGA_BIDI_R || c == VEGA_BIDI_AL) last = c;
        else if (c == VEGA_BIDI_EN && last == VEGA_BIDI_AL) t[seq[k]] = VEGA_BIDI_AN;
    }
    for (size_t k = 0; k < m; ++k)
        if (t[seq[k]] == VEGA_BIDI_AL) t[seq[k]] = VEGA_BIDI_R;
    for (size_t k = 1; k + 1 < m; ++k) { /* W4 */
        Class c = t[seq[k]], p = t[seq[k - 1]], n = t[seq[k + 1]];
        if (c == VEGA_BIDI_ES && p == VEGA_BIDI_EN && n == VEGA_BIDI_EN) t[seq[k]] = VEGA_BIDI_EN;
        else if (c == VEGA_BIDI_CS && p == n && (p == VEGA_BIDI_EN || p == VEGA_BIDI_AN)) t[seq[k]] = p;
    }
    for (size_t k = 0; k < m;) { /* W5 */
        if (t[seq[k]] != VEGA_BIDI_ET) { ++k; continue; }
        size_t end = k;
        while (end < m && t[seq[end]] == VEGA_BIDI_ET) ++end;
        bool number = (k && t[seq[k - 1]] == VEGA_BIDI_EN) || (end < m && t[seq[end]] == VEGA_BIDI_EN);
        for (; k < end; ++k)
            if (number) t[seq[k]] = VEGA_BIDI_EN;
    }
    for (size_t k = 0; k < m; ++k) { /* W6 */
        Class c = t[seq[k]];
        if (c == VEGA_BIDI_ES || c == VEGA_BIDI_ET || c == VEGA_BIDI_CS) t[seq[k]] = VEGA_BIDI_ON;
    }
    last = s->sos; /* W7 */
    for (size_t k = 0; k < m; ++k) {
        Class c = t[seq[k]];
        if (c == VEGA_BIDI_L || c == VEGA_BIDI_R) last = c;
        else if (c == VEGA_BIDI_EN && last == VEGA_BIDI_L) t[seq[k]] = VEGA_BIDI_L;
    }
}

/* N0: paired brackets take the direction of what they enclose. */
static void brackets(Sequence *s) {
    if (!s->codes) return;
    Class *t = s->t;
    size_t *seq = s->seq, m = s->count;
    Pair *pairs = s->pairs;
    size_t found = 0;
    struct { uint32_t pair; size_t at; } open[BRACKETS];
    size_t depth = 0;
    for (size_t k = 0; k < m; ++k) {
        uint32_t pair, code = s->codes[seq[k]];
        int kind = t[seq[k]] == VEGA_BIDI_ON ? vega_bracket(code, &pair) : 0;
        if (kind == 1) {
            if (depth == BRACKETS) break; /* BD16: no more pairs in this sequence */
            open[depth].pair = canonical(pair);
            open[depth++].at = k;
        } else if (kind == 2) {
            for (size_t d = depth; d-- > 0;)
                if (open[d].pair == canonical(code)) {
                    pairs[found].open = open[d].at, pairs[found++].close = k;
                    depth = d;
                    break;
                }
        }
    }
    qsort(pairs, found, sizeof(*pairs), by_opening); /* in the order of the opening brackets */
    Class embedding = s->level & 1 ? VEGA_BIDI_R : VEGA_BIDI_L, opposite = embedding == VEGA_BIDI_L ? VEGA_BIDI_R : VEGA_BIDI_L;
    for (size_t p = 0; p < found; ++p) {
        size_t open_at = pairs[p].open, close_at = pairs[p].close;
        bool same = false, other = false;
        for (size_t k = open_at + 1; k < close_at; ++k) {
            int d = strong(t[seq[k]]);
            if (d == (int)embedding) same = true;
            else if (d >= 0) other = true;
        }
        Class resolved;
        if (same) resolved = embedding;
        else if (other) {
            Class context = s->sos;
            for (size_t k = open_at; k-- > 0;)
                if (strong(t[seq[k]]) >= 0) { context = (Class)strong(t[seq[k]]); break; }
            resolved = context == opposite ? opposite : embedding;
        } else
            continue;
        size_t ends[2] = {open_at, close_at};
        for (unsigned e = 0; e < 2; ++e) {
            t[seq[ends[e]]] = resolved;
            /* Marks that followed the bracket (made ON by W1) follow it. */
            for (size_t k = ends[e] + 1; k < m && s->original[seq[k]] == VEGA_BIDI_NSM; ++k) t[seq[k]] = resolved;
        }
    }
}

static void neutral_types(Sequence *s) {
    Class *t = s->t;
    size_t *seq = s->seq, m = s->count;
    Class embedding = s->level & 1 ? VEGA_BIDI_R : VEGA_BIDI_L;
    for (size_t k = 0; k < m;) { /* N1, N2 */
        if (!neutral(t[seq[k]])) { ++k; continue; }
        size_t end = k;
        while (end < m && neutral(t[seq[end]])) ++end;
        int left = k ? strong(t[seq[k - 1]]) : (int)s->sos, right = end < m ? strong(t[seq[end]]) : (int)s->eos;
        Class resolved = left >= 0 && left == right ? (Class)left : embedding;
        for (; k < end; ++k) t[seq[k]] = resolved;
    }
    for (size_t k = 0; k < m; ++k) { /* I1, I2 */
        Class c = t[seq[k]];
        uint8_t *level = &s->levels[seq[k]];
        if (!(*level & 1)) *level += c == VEGA_BIDI_R ? 1 : c == VEGA_BIDI_AN || c == VEGA_BIDI_EN ? 2 : 0;
        else *level += c == VEGA_BIDI_L || c == VEGA_BIDI_EN || c == VEGA_BIDI_AN ? 1 : 0;
    }
}

int vega_bidi_levels(const VegaBidiClass *classes, const uint32_t *codes, size_t count, int direction, uint8_t *levels) {
    if (!count) return direction == VEGA_BIDI_RTL ? 1 : 0;
    size_t *match = malloc(count * sizeof(*match)), *runs = malloc((count + 1) * sizeof(*runs)),
           *run_of = malloc(count * sizeof(*run_of)), *seq = malloc(count * sizeof(*seq));
    Class *t = malloc(count * sizeof(*t));
    bool *matched_pdi = calloc(count, sizeof(*matched_pdi));
    Pair *pairs = malloc((count / 2 + 1) * sizeof(*pairs));
    uint8_t *embedding = malloc(count);
    int paragraph = -1;
    if (!match || !runs || !run_of || !seq || !t || !matched_pdi || !pairs || !embedding) goto done;
    memcpy(t, classes, count * sizeof(*t));
    /* BD9: each isolate initiator's matching PDI (count: none). */
    {
        size_t depth = 0;
        for (size_t i = 0; i < count; ++i) {
            match[i] = count;
            if (initiator(t[i])) runs[depth++] = i; /* runs as a stack here */
            else if (t[i] == VEGA_BIDI_PDI && depth) {
                match[runs[--depth]] = i;
                matched_pdi[i] = true;
            }
        }
    }
    /* P2, P3 */
    paragraph = direction == VEGA_BIDI_RTL ? 1 : direction == VEGA_BIDI_LTR ? 0 : first_strong(t, match, 0, count) == 1;
    /* X1-X8: explicit levels and directions. */
    struct { uint8_t level; Class override; bool isolate; } stack[MAX_DEPTH + 2];
    size_t depth = 0;
    unsigned overflow_isolates = 0, overflow_embeddings = 0, valid_isolates = 0;
    stack[depth].level = (uint8_t)paragraph, stack[depth].override = VEGA_BIDI_ON, stack[depth++].isolate = false;
    for (size_t i = 0; i < count; ++i) {
        Class c = classes[i];
        uint8_t level = stack[depth - 1].level;
        switch (c) {
        case VEGA_BIDI_RLE: case VEGA_BIDI_LRE: case VEGA_BIDI_RLO: case VEGA_BIDI_LRO: {
            bool rtl = c == VEGA_BIDI_RLE || c == VEGA_BIDI_RLO;
            uint8_t next = rtl ? (uint8_t)((level + 1) | 1) : (uint8_t)((level + 2) & ~1);
            levels[i] = level;
            if (next <= MAX_DEPTH && !overflow_isolates && !overflow_embeddings) {
                stack[depth].level = next;
                stack[depth].override = c == VEGA_BIDI_RLO ? VEGA_BIDI_R : c == VEGA_BIDI_LRO ? VEGA_BIDI_L : VEGA_BIDI_ON;
                stack[depth++].isolate = false;
            } else if (!overflow_isolates)
                ++overflow_embeddings;
            break;
        }
        case VEGA_BIDI_RLI: case VEGA_BIDI_LRI: case VEGA_BIDI_FSI: {
            levels[i] = level;
            if (stack[depth - 1].override != VEGA_BIDI_ON) t[i] = stack[depth - 1].override;
            bool rtl = c == VEGA_BIDI_RLI || (c == VEGA_BIDI_FSI && first_strong(classes, match, i + 1, match[i]) == 1);
            uint8_t next = rtl ? (uint8_t)((level + 1) | 1) : (uint8_t)((level + 2) & ~1);
            if (next <= MAX_DEPTH && !overflow_isolates && !overflow_embeddings) {
                ++valid_isolates;
                stack[depth].level = next, stack[depth].override = VEGA_BIDI_ON, stack[depth++].isolate = true;
            } else
                ++overflow_isolates;
            break;
        }
        case VEGA_BIDI_PDI:
            if (overflow_isolates) --overflow_isolates;
            else if (valid_isolates) {
                overflow_embeddings = 0;
                while (!stack[depth - 1].isolate) --depth;
                --depth;
                --valid_isolates;
            }
            levels[i] = stack[depth - 1].level;
            if (stack[depth - 1].override != VEGA_BIDI_ON) t[i] = stack[depth - 1].override;
            break;
        case VEGA_BIDI_PDF:
            levels[i] = level;
            if (overflow_isolates) break;
            if (overflow_embeddings) --overflow_embeddings;
            else if (!stack[depth - 1].isolate && depth >= 2) --depth;
            break;
        case VEGA_BIDI_B: levels[i] = (uint8_t)paragraph; break; /* X8 */
        case VEGA_BIDI_BN: levels[i] = level; break;
        default:
            levels[i] = level;
            if (stack[depth - 1].override != VEGA_BIDI_ON) t[i] = stack[depth - 1].override;
        }
    }
    /* X9, X10: level runs of the characters that stay, and the isolating run
     * sequences they make (a run ending in an isolate initiator continues
     * with the run its matching PDI begins), by the explicit levels (the
     * sequences' own resolution changes levels). */
    memcpy(embedding, levels, count);
    size_t run_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (removed(classes[i])) continue;
        bool starts = true;
        for (size_t j = i; j-- > 0;)
            if (!removed(classes[j])) { starts = embedding[j] != embedding[i]; break; }
        if (starts) runs[run_count++] = i;
        run_of[i] = run_count - 1;
    }
    for (size_t r = 0; r < run_count; ++r) {
        size_t first = runs[r];
        if (classes[first] == VEGA_BIDI_PDI && matched_pdi[first]) continue; /* it continues another sequence */
        Sequence s = {classes, t, levels, codes, seq, 0, embedding[first], VEGA_BIDI_L, VEGA_BIDI_L, pairs};
        for (size_t run = r;;) {
            size_t last = runs[run];
            for (size_t i = runs[run]; i < count && (removed(classes[i]) || run_of[i] == run); ++i)
                if (!removed(classes[i])) seq[s.count++] = last = i;
            if (!initiator(classes[last]) || match[last] >= count) break;
            run = run_of[match[last]];
        }
        size_t head = seq[0], tail = seq[s.count - 1];
        int previous = paragraph, next = paragraph;
        for (size_t j = head; j-- > 0;)
            if (!removed(classes[j])) { previous = embedding[j]; break; }
        if (!initiator(classes[tail]))
            for (size_t j = tail + 1; j < count; ++j)
                if (!removed(classes[j])) { next = embedding[j]; break; }
        s.sos = ((s.level > previous ? s.level : previous) & 1) ? VEGA_BIDI_R : VEGA_BIDI_L;
        s.eos = ((s.level > next ? s.level : next) & 1) ? VEGA_BIDI_R : VEGA_BIDI_L;
        weak_types(&s);
        brackets(&s);
        neutral_types(&s);
    }
    /* L1: separators, and whitespace and isolate controls before them or at
     * the end of the line, go back to the paragraph's level. */
    for (size_t i = count, trailing = 1; i-- > 0;) {
        Class c = classes[i];
        if (c == VEGA_BIDI_S || c == VEGA_BIDI_B) {
            levels[i] = (uint8_t)paragraph;
            trailing = 1;
        } else if (c == VEGA_BIDI_WS || initiator(c) || c == VEGA_BIDI_PDI || removed(c)) {
            if (trailing) levels[i] = (uint8_t)paragraph;
        } else
            trailing = 0;
    }
    for (size_t i = 0; i < count; ++i)
        if (removed(classes[i])) levels[i] = VEGA_BIDI_REMOVED;
done:
    free(match);
    free(runs);
    free(run_of);
    free(seq);
    free(t);
    free(matched_pdi);
    free(pairs);
    free(embedding);
    return paragraph;
}

size_t vega_bidi_order(const uint8_t *levels, size_t count, uint32_t *order) {
    size_t n = 0;
    uint8_t highest = 0, lowest_odd = 0xff;
    for (size_t i = 0; i < count; ++i) {
        if (levels[i] == VEGA_BIDI_REMOVED) continue;
        order[n++] = (uint32_t)i;
        if (levels[i] > highest) highest = levels[i];
        if ((levels[i] & 1) && levels[i] < lowest_odd) lowest_odd = levels[i];
    }
    /* L2: from the highest level down to the lowest odd one, reverse every
     * run of characters at that level or higher. */
    for (int level = highest; level >= lowest_odd && lowest_odd != 0xff; --level)
        for (size_t k = 0; k < n;) {
            if (levels[order[k]] < level) { ++k; continue; }
            size_t end = k;
            while (end < n && levels[order[end]] >= level) ++end;
            for (size_t a = k, b = end - 1; a < b; ++a, --b) {
                uint32_t swap = order[a];
                order[a] = order[b];
                order[b] = swap;
            }
            k = end;
        }
    return n;
}
