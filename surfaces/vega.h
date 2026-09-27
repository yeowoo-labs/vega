#ifndef VEGA_H
#define VEGA_H
#include "module.h"
#include "symbols.h"
#include <poll.h>
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
    int pointer;                    /* the index state that view shows, or -1: a surface may move it */
    int label, tone, detail, badge; /* columns, or -1 */
    uint32_t columns[PURE_MODULE_VIEWS]; /* a table's shown columns, in order */
    unsigned column_count;
} VegaPart;

typedef struct {
    const PureModule *module;
    int title;  /* text view, or -1 */
    int prompt; /* the first text input, or -1 */
    bool ticks; /* it handles frames: a surface runs them, about 60 a second */
    VegaPart parts[VEGA_PARTS];
    unsigned count;
} VegaScreen;

void vega_screen(VegaScreen *, const PureModule *);

/* The wall-clock time events see on a live surface: milliseconds since 1970
 * (UTC). Scripts set their own (time MS). */
double vega_now(void);
/* When each timer input ticks next. run ticks every one due at now (a tick
 * sees now) and gives the milliseconds until the next, or -1 without timers;
 * ticked says whether any did. Missed ticks are not made up. */
typedef struct { double next[PURE_MODULE_INPUTS]; } VegaTimers;
void vega_timers_start(VegaTimers *, const PureModule *, double now);
long vega_timers_run(VegaTimers *, PureApp *, double now, bool *ticked);
/* What a module seems to mean but does not meet: a list label that is not
 * text, a _focus view without its table, a column a list never shows, a
 * second text input. Each finding names the view or input it is about;
 * returns how many there were. */
unsigned vega_check(const PureModule *, void (*report)(void *, const char *name, const char *message), void *context);

/* A view (row ignored) or a column at a row as text; numbers in Pure's
 * shortest form. false when it has no value (for example a missing item). */
bool vega_text(PureApp *, uint32_t view, size_t row, char out[VEGA_TEXT], size_t *length);
bool vega_number(PureApp *, uint32_t view, size_t row, double *out);
size_t vega_rows(PureApp *, const VegaPart *);
/* The row of the focused item: the focus view names an item (an index's
 * position), shown on the row that holds it; VEGA_NO_ROW when a filtered
 * list leaves that item out. Clamped to the rows without a filter; 0
 * without a focus view. */
#define VEGA_NO_ROW SIZE_MAX
size_t vega_focus(PureApp *, const VegaPart *);
/* Focus a row, as a user pointing at it (a click); false when the part's
 * focus is not a plain index position or the row is not there. */
bool vega_point(PureApp *, const VegaPart *, size_t row);
/* The first row to show of `rows` when `visible` fit: the least scrolling
 * from first that keeps the focused row in view (any, without one). */
size_t vega_scroll(size_t first, size_t focus, size_t visible, size_t rows);
/* The key input answering a key name (Pure's names, as KeyboardEvent.code
 * spells them), or -1; press sends its press and release. */
int vega_key_input(const PureModule *, const char *key);
bool vega_press(PureApp *, const char *key);
/* Commit text to the screen's text input. */
bool vega_commit(PureApp *, const VegaScreen *, const char *text, size_t length);
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
/* GUI: the screen as a web page on listen:port (0: any free port, printed as
 * {"port":N}); runs until SIGINT or SIGTERM. reload, when given, is asked
 * a few times a second whether the module was replaced (the screen with it). */
int vega_web(PureApp *, const VegaScreen *, const char *listen, unsigned port, bool (*reload)(void));

/* TUI: a grid of cells, redrawn from the app on every change. first keeps each
 * list's scroll between frames; the focused row stays visible. shown is what
 * the terminal shows, so a paint sends only the cells that changed (all of
 * them until the first paint after a resize). */
typedef struct {
    uint32_t ch, mark; /* a character and at most one combining mark; ch 0 continues a wide one */
    uint8_t tone;
    bool focus, strong;
} VegaCell;
typedef struct {
    unsigned width, height;
    VegaCell *cells, *shown;
    struct VegaRow { int part; size_t item; } *rows; /* the item each line shows, or part -1 */
    bool painted; /* shown holds what the terminal shows */
    size_t first[VEGA_PARTS];
    int cursor_row, cursor_column; /* where the terminal cursor goes, or -1 */
} VegaGrid;
bool vega_grid(VegaGrid *, unsigned width, unsigned height);
void vega_grid_free(VegaGrid *);
/* prompt is the text being typed for the screen's text input; typing says
 * whether keys go to it. */
void vega_draw(VegaGrid *, PureApp *, const VegaScreen *, const char *prompt, size_t prompt_length, bool typing);
/* The part and item a line of the last drawn grid shows. */
bool vega_hit(const VegaGrid *, unsigned line, unsigned *part, size_t *item);

/* Pixels: the grid drawn with a font's glyphs (a symbol file, pixels per em).
 * A cell is as wide as a digit and as tall as the font's line; each glyph is
 * drawn once, the first time a cell shows it. */
typedef struct {
    VegaGlyphs *glyphs;
    int cell_width, cell_height, baseline;
} VegaFace;
bool vega_face_open(VegaFace *, const VegaSymbols *, float pixels);
void vega_face_close(VegaFace *);
/* The grid into out: 0xRRGGBB pixels, grid->width * cell_width across and
 * grid->height * cell_height down, stride pixels apart. */
void vega_pixels(const VegaGrid *, VegaFace *, uint32_t *out, size_t stride);
/* width x height of out in the background's colour. */
void vega_pixels_clear(uint32_t *out, size_t stride, unsigned width, unsigned height);

/* A display through KMS: the first connected connector (or, with writeback,
 * a writeback connector: the frames come back to memory, so no screen is
 * needed to check them), its preferred mode or width x height. */
typedef struct VegaDisplay VegaDisplay;
VegaDisplay *vega_display_open(const char *card, bool writeback, unsigned width, unsigned height, char *error,
                               size_t error_size);
/* The buffer to draw the next frame into: 0xRRGGBB, stride pixels apart. */
uint32_t *vega_display_buffer(VegaDisplay *, unsigned *width, unsigned *height, size_t *stride);
/* Show it (and, on writeback, wait until the frame has come back). */
bool vega_display_show(VegaDisplay *);
/* Writeback: the last frame as the display controller put it out. */
const uint32_t *vega_display_captured(const VegaDisplay *, size_t *stride);
void vega_display_close(VegaDisplay *);

/* Input from evdev: keyboards (key names and the text they type on a US
 * layout), mice and touch screens (presses at a pixel of a width x height
 * screen). With paths, recorded events are read from those files instead,
 * positions in screen pixels. */
typedef enum { VEGA_EVENT_KEY, VEGA_EVENT_TEXT, VEGA_EVENT_PRESS, VEGA_EVENT_WHEEL } VegaEventKind;
typedef struct {
    VegaEventKind kind;
    char name[24], text[8]; /* a key's name; the text it types */
    int x, y, steps;        /* where a press or the wheel is; steps down (negative up) */
} VegaEvent;
typedef struct VegaInput VegaInput;
VegaInput *vega_input_open(const char *const *paths, unsigned count, unsigned width, unsigned height);
unsigned vega_input_fds(const VegaInput *, struct pollfd *out, unsigned max);
/* What the device on fd has for us; ended: it has no more (a file read out). */
unsigned vega_input_read(VegaInput *, int fd, VegaEvent *out, unsigned max, bool *ended);
/* Where the pointer is, and whether a mouse moved it (a finger's is not shown). */
void vega_input_pointer(const VegaInput *, int *x, int *y, bool *shown);
void vega_input_close(VegaInput *);
#endif
