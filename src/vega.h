#ifndef VEGA_H
#define VEGA_H
#include "module.h"
#include <stdio.h>

/* Vega reads a verified Pure module by convention (docs/design/001-first-component.md):
 *  - a text view named `title` is the screen's title;
 *  - a table view with a text column `label` is a list; any other table is a
 *    table. A number column `tone` gives a row its tone; text columns `detail`
 *    and `badge` add to a list row;
 *  - a number view named `<table>_focus` is the focused row of that table;
 *  - every other number or text view is a field.
 * A tone says what a value means (lib/vega.aup); each surface decides how it
 * looks or sounds. Surfaces evaluate only the cells they present. */
enum { VEGA_PARTS = 64, VEGA_TEXT = 1024 };
typedef enum { VEGA_NORMAL, VEGA_MUTED, VEGA_ACCENT, VEGA_GOOD, VEGA_WARNING, VEGA_DANGER, VEGA_TONES } VegaTone;
typedef enum { VEGA_FIELD, VEGA_LIST, VEGA_TABLE } VegaKind;

typedef struct {
    VegaKind kind;
    uint32_t view;
    int focus;                      /* number view with the focused row, or -1 */
    int label, tone, detail, badge; /* columns, or -1 */
    uint32_t columns[PURE_MODULE_VIEWS]; /* a table's shown columns, in order */
    unsigned column_count;
} VegaPart;

typedef struct {
    const PureModule *module;
    int title;  /* text view, or -1 */
    int prompt; /* the first text input, or -1 */
    VegaPart parts[VEGA_PARTS];
    unsigned count;
} VegaScreen;

void vega_screen(VegaScreen *, const PureModule *);

/* A view (row ignored) or a column at a row as text; numbers in Pure's
 * shortest form. false when it has no value (for example a missing item). */
bool vega_text(PureApp *, uint32_t view, size_t row, char out[VEGA_TEXT], size_t *length);
bool vega_number(PureApp *, uint32_t view, size_t row, double *out);
size_t vega_rows(const PureApp *, const VegaPart *);
/* The focused row, clamped to the rows; 0 without a focus view. */
size_t vega_focus(PureApp *, const VegaPart *);
VegaTone vega_tone(PureApp *, const VegaPart *, size_t row);
/* Values evaluated so far, for checking that surfaces stay lazy. */
unsigned long vega_evaluations(void);

/* Display cells of a code point in a terminal: 0 for combining and
 * zero-width characters, 2 for East Asian wide and emoji, else 1. */
unsigned vega_width(uint32_t codepoint);
/* The code point at text[at] (valid UTF-8) and the bytes it takes. */
size_t vega_decode(const char *text, size_t length, size_t at, uint32_t *codepoint);
/* A control character shown as its control picture (U+2400 block), else itself. */
uint32_t vega_visible(uint32_t codepoint);
unsigned vega_text_width(const char *text, size_t length);

/* CLI: everything, as plain lines for pipes; color only when asked. */
void vega_print(FILE *, PureApp *, const VegaScreen *, bool color);
/* NUI: the screen as sentences (all), or only the focused item of the first list. */
void vega_say(FILE *, PureApp *, const VegaScreen *, bool all);

/* TUI: a grid of cells, redrawn from the app on every change. first keeps each
 * list's scroll between frames; the focused row stays visible. */
typedef struct {
    uint32_t ch, mark; /* a character and at most one combining mark; ch 0 continues a wide one */
    uint8_t tone;
    bool focus, strong;
} VegaCell;
typedef struct {
    unsigned width, height;
    VegaCell *cells;
    size_t first[VEGA_PARTS];
    int cursor_row, cursor_column; /* where the terminal cursor goes, or -1 */
} VegaGrid;
bool vega_grid(VegaGrid *, unsigned width, unsigned height);
void vega_grid_free(VegaGrid *);
/* prompt is the text being typed for the screen's text input; typing says
 * whether keys go to it. */
void vega_draw(VegaGrid *, PureApp *, const VegaScreen *, const char *prompt, size_t prompt_length, bool typing);
#endif
