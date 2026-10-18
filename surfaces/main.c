#define _DEFAULT_SOURCE
#include "vega.h"
#include "module-store.h"
#include "store.h"
#include "surface.h"
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* vega SURFACE MODULE [--store PATH]: one module, one screen, several surfaces.
 *   check            what the module seems to mean but does not meet (exit 1)
 *   tui              interactive terminal
 *   tui --dump WxH   the terminal's frames as text, driven by a script
 *   tui --paint WxH  the bytes each frame sends to the terminal, driven by a script
 *   cli              plain lines for pipes, driven by a script
 *   say              sentences (the first natural-language surface), driven by a script
 *   web [--listen ADDRESS] [--port N]
 *                    a web page (a GUI), on 127.0.0.1 unless another address is given
 *   pixels --symbols FONT.vsym... [--size PX] --grid WxH --out PREFIX
 *                    the TUI's frames drawn in fonts (in order of preference;
 *                    set as type when the first shapes), driven by a script:
 *                    each show writes PREFIX-N.ppm after the text
 *   display --symbols FONT.vsym... [--size PX] [--card PATH] [--writeback WxH --out PREFIX]
 *           [--live | --input EVENTS]
 *                    the same on a display through KMS, as many cells as fit,
 *                    driven by a script, or live: keyboards, mice and touch
 *                    screens (evdev), or events recorded in a file (read to its
 *                    end, then the last frame shown as text); with writeback
 *                    each shown frame comes back from the display controller,
 *                    is set against what was drawn and written to PREFIX-N.ppm
 * With --store the app's states live in that tree store: the app continues
 * where it was, whichever surface runs it. With --watch (tui, web) a module
 * written again replaces the running one and states keep their values by
 * name: aup watch app.aup -o app.pmod beside it makes a saved edit show at once.
 * Script commands, one per line: key KEYNAME | text HEX|- | type HEX | enter |
 * tab | hangul | letters HEX | back | frame [N] | point VIEW ROW | click LINE COLUMN | time MS | tick TIMER |
 * swap MODULE | show. key presses and
 * releases the input bound to that key; text commits text to the screen's text
 * input; type, enter and tab act on the TUI's input line, and so do hangul (the
 * 한/영 key), letters (keys typed, each byte one, through Hangul when it is on)
 * and back (Backspace); point focuses a row
 * (0 first) of a list or table; click is a mouse click on the last shown TUI
 * frame (1 first, as terminals report it); time sets the time the next
 * events see (live surfaces use the clock) and tick ticks a timer (live
 * surfaces tick them on time); swap replaces the module as --watch does.
 *
 * Built with VEGA_SURFACE the same host is a library a Pure node maps
 * (pure-serve --surface vega-surface.so -- display ARGUMENTS, Pure's
 * serve/surface.h): the display shows the node's app as of each save, read
 * where it is, and what people type goes to the node's owner. */

static PureModule modules[2]; /* the running one (app.module) and the next */
static unsigned current;
static PureApp app;
/* In a node: the node, whose owner applies the events; app then shows its
 * last save (a reader) and applies none itself. */
static const PureSurface *node;
static VegaScreen screen;
static VegaGrid *drawn; /* the TUI's last frame, which a click lands on */
static char entry[VEGA_TEXT];
static size_t entry_length;
static bool typing;
/* Hangul typed on the keyboard (Lang1, the 한/영 key, or AltRight switches):
 * the syllable being composed shows after the entry line until it is done. */
static bool korean;
static VegaHangul hangul = {-1, -1, 0, 0, {{0, 0, 0}}};

/* The module file last read, and a digest of its bytes. A loaded module
 * keeps nothing of them, so one buffer, grown to the largest, serves every load. */
static unsigned char *module_bytes;
static size_t module_capacity;
static uint64_t module_digest;

/* Read a module file whole into module_bytes: its size, or -1. */
static long read_module(const char *path) {
    FILE *file = fopen(path, "rb");
    size_t size = 0;
    bool ok = file != NULL;
    while (ok && !feof(file)) {
        if (size == module_capacity) {
            size_t capacity = module_capacity ? module_capacity * 2 : 1 << 16;
            unsigned char *grown = capacity <= (8u << 20) ? realloc(module_bytes, capacity) : NULL;
            if (!grown) { ok = false; break; }
            module_bytes = grown;
            module_capacity = capacity;
        }
        size += fread(module_bytes + size, 1, module_capacity - size, file);
        ok = !ferror(file);
    }
    if (file) fclose(file);
    if (!ok) return -1;
    uint64_t h = UINT64_C(0xcbf29ce484222325) ^ size;
    for (size_t i = 0; i < size; ++i) h = (h ^ module_bytes[i]) * UINT64_C(0x100000001b3);
    module_digest = h;
    return (long)size;
}

/* Decode the module that read_module read from path. */
static bool decode(const char *path, long size, PureModule *module) {
    if (size < 0) { fprintf(stderr, "vega: cannot read %s\n", path); return false; }
    char error[128];
    if (pure_module_load(module_bytes, (size_t)size, module, error, sizeof(error)) != PURE_OK) {
        fprintf(stderr, "vega: %s: %s\n", path, error);
        return false;
    }
    return true;
}

static bool load_module(const char *path, PureModule *module) { return decode(path, read_module(path), module); }

static void print_finding(void *path, const char *name, const char *message) {
    printf("%s: %s: %s\n", (const char *)path, name, message);
}

static bool load(const char *path, const char *store) {
    if (!load_module(path, &modules[current])) return false;
    if (store && pure_store_init(store) != PURE_OK) { fprintf(stderr, "vega: cannot open store %s\n", store); return false; }
    PureResult started = pure_app_start(&app, &modules[current], store ? pure_tree_data(store) : (PureData){0});
    if (started != PURE_OK) {
        fprintf(stderr, "vega: cannot start (%d%s%s)\n", (int)started, app.why[0] ? ": " : "", app.why);
        return false;
    }
    vega_screen(&screen, &modules[current]);
    typing = screen.prompt >= 0;
    return true;
}

/* Replace the running app's module with the one read from path, states
 * keeping their values by name; a module that does not load or replace
 * leaves the app as it was. */
static bool swap(const char *path, long size) {
    PureModule *next = &modules[!current];
    if (!decode(path, size, next)) return false;
    PureResult replaced = pure_app_replace(&app, next);
    if (replaced != PURE_OK) {
        fprintf(stderr, "vega: keeping the running module: cannot replace it (%d%s%s)\n", (int)replaced,
                app.why[0] ? ": " : "", app.why);
        return false;
    }
    bool had_prompt = screen.prompt >= 0;
    current = !current;
    vega_screen(&screen, next);
    typing = screen.prompt >= 0 && (typing || !had_prompt);
    return true;
}

/* --watch: the module file's bytes when last read (their digest), so a
 * module written again is seen however soon and whatever its size or file
 * times. Read at most five times a second, however often asked; bytes that
 * do not load are not tried again. */
static const char *watched;
static uint64_t watched_digest;
static struct timespec watched_at;

static bool module_changed(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!watched || (now.tv_sec - watched_at.tv_sec) * 1000000000L + now.tv_nsec - watched_at.tv_nsec < 200000000L)
        return false;
    watched_at = now;
    long size = read_module(watched);
    if (size < 0 || module_digest == watched_digest) return false; /* a missing file is looked for again */
    watched_digest = module_digest;
    return swap(watched, size);
}

static int key_input(const char *key) { return vega_key_input(app.module, key); }

/* A key press and release; false when the app has no such key or the press failed. */
static bool press(const char *key, int *input) {
    *input = key_input(key);
    if (*input >= 0 && node) return node->press(node->host, (uint32_t)*input) == PURE_OK;
    return *input >= 0 && vega_press(&app, key);
}

static bool commit(const char *text, size_t length) {
    if (node)
        return screen.prompt >= 0 &&
               node->type(node->host, (uint32_t)screen.prompt, (PureSpan){(const unsigned char *)text, length}) == PURE_OK;
    return vega_commit(&app, &screen, text, length);
}

static const VegaPart *part_named(const char *name) {
    for (unsigned i = 0; i < screen.count; ++i)
        if (screen.parts[i].kind != VEGA_FIELD && !strcmp(app.module->view_names[screen.parts[i].view], name))
            return &screen.parts[i];
    return NULL;
}

/* A mouse button on a line of the last frame: a click focuses the row there;
 * the wheel moves the focus of the list or table under it. */
static bool mouse(unsigned button, unsigned line) {
    unsigned index;
    size_t item;
    if (!drawn || !vega_hit(drawn, line, &index, &item)) return false;
    const VegaPart *part = &screen.parts[index];
    if (button == 64 || button == 65) {
        size_t focus = vega_focus(&app, part), rows = vega_rows(&app, part);
        if (focus < rows) { /* else the wheel focuses the row under it */
            if (button == 64 ? !focus : focus + 1 >= rows) return false;
            item = button == 64 ? focus - 1 : focus + 1;
        }
    } else if (button) return false;
    return !node && vega_point(&app, part, item); /* a node takes keys and text, not pointing yet */
}

static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static size_t unhex(const char *digits, char *out, size_t size) {
    size_t length = 0;
    if (!strcmp(digits, "-")) return 0;
    for (size_t i = 0; digits[i] && digits[i + 1] && length < size; i += 2)
        if (hex(digits[i]) >= 0 && hex(digits[i + 1]) >= 0) out[length++] = (char)(hex(digits[i]) << 4 | hex(digits[i + 1]));
    return length;
}

/* ---- terminal ------------------------------------------------------- */

typedef struct { char *bytes; size_t used, capacity; } Out;

static void out_put(Out *out, const char *bytes, size_t size) {
    if (out->used + size > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 4096;
        while (capacity < out->used + size) capacity *= 2;
        char *grown = realloc(out->bytes, capacity);
        if (!grown) return;
        out->bytes = grown;
        out->capacity = capacity;
    }
    memcpy(out->bytes + out->used, bytes, size);
    out->used += size;
}

static void out_codepoint(Out *out, uint32_t c) {
    char b[4];
    size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (n == 1) b[0] = (char)c;
    else if (n == 2) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); }
    else if (n == 3) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); }
    else {
        b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 0x3F));
        b[2] = (char)(0x80 | (c >> 6 & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F));
    }
    out_put(out, b, n);
}

static const char *const tone_names[VEGA_TONES] = {"normal", "muted", "accent", "good", "warning", "danger"};

/* The grid as text, one line per row: '>' marks a focused row, and a row's
 * tone (of its first character) follows a tab when it is not normal. */
static void dump(const VegaGrid *grid) {
    Out out = {0};
    for (unsigned r = 0; r < grid->height; ++r) {
        const VegaCell *line = grid->cells + (size_t)r * grid->width;
        bool focus = false;
        int tone = -1;
        size_t start = out.used, end;
        for (unsigned c = 0; c < grid->width; ++c) {
            focus = focus || line[c].focus;
            if (tone < 0 && line[c].ch && line[c].ch != ' ') tone = line[c].tone;
        }
        out_put(&out, focus ? ">" : "|", 1);
        for (unsigned c = 0; c < grid->width; ++c) {
            if (!line[c].ch) continue;
            out_codepoint(&out, line[c].ch);
            if (line[c].mark) out_codepoint(&out, line[c].mark);
        }
        end = out.used;
        while (end > start + 1 && out.bytes[end - 1] == ' ') --end;
        out.used = end;
        if (tone > 0) { out_put(&out, "\t", 1); out_put(&out, tone_names[tone], strlen(tone_names[tone])); }
        out_put(&out, "\n", 1);
    }
    fwrite(out.bytes, 1, out.used, stdout);
    free(out.bytes);
}

static const char *const sgr_tones[VEGA_TONES] = {"", ";2", ";1;36", ";32", ";33", ";31"};

static bool same_cell(const VegaCell *a, const VegaCell *b) {
    return a->ch == b->ch && a->mark == b->mark && a->tone == b->tone && a->focus == b->focus && a->strong == b->strong;
}

/* The terminal bytes that turn what it shows into the grid: each run of
 * changed cells after a cursor move (a short unchanged gap stays in the run,
 * being cheaper than another move), everything after a resize. */
static void paint(VegaGrid *grid, Out *out) {
    bool all = !grid->painted;
    int style = -1;
    char code[48];
    if (all) out_put(out, "\033[2J", 4);
    for (unsigned r = 0; r < grid->height; ++r) {
        const VegaCell *line = grid->cells + (size_t)r * grid->width, *old = grid->shown + (size_t)r * grid->width;
        for (unsigned c = 0; c < grid->width;) {
            if (!all && same_cell(&line[c], &old[c])) { ++c; continue; }
            unsigned start = c, end = c + 1;
            for (unsigned k = c + 1, gap = 0; k < grid->width && gap <= 6; ++k) {
                if (all || !same_cell(&line[k], &old[k])) { end = k + 1; gap = 0; }
                else ++gap;
            }
            while (start && !line[start].ch) --start; /* the second half of a wide character: from its first */
            out_put(out, code, (size_t)snprintf(code, sizeof(code), "\033[%u;%uH", r + 1, start + 1));
            for (unsigned k = start; k < end; ++k) {
                if (!line[k].ch) continue;
                int next = line[k].tone | line[k].focus << 4 | line[k].strong << 5;
                if (next != style) {
                    out_put(out, code, (size_t)snprintf(code, sizeof(code), "\033[0%s%s%sm", line[k].strong ? ";1" : "",
                                                         line[k].focus ? ";7" : "",
                                                         sgr_tones[line[k].tone < VEGA_TONES ? line[k].tone : 0]));
                    style = next;
                }
                out_codepoint(out, line[k].ch);
                if (line[k].mark) out_codepoint(out, line[k].mark);
            }
            c = end;
        }
    }
    if (style > 0) out_put(out, "\033[0m", 4);
    if (grid->cursor_row >= 0)
        out_put(out, code, (size_t)snprintf(code, sizeof(code), "\033[%d;%dH\033[?25h", grid->cursor_row + 1,
                                             grid->cursor_column + 1));
    else out_put(out, "\033[?25l", 6);
    memcpy(grid->shown, grid->cells, (size_t)grid->width * grid->height * sizeof(*grid->cells));
    grid->painted = true;
}

static void paint_terminal(VegaGrid *grid) {
    Out out = {0};
    paint(grid, &out);
    if (write(STDOUT_FILENO, out.bytes, out.used) < 0) { /* the terminal went away; the loop ends on read */ }
    free(out.bytes);
}

static struct termios saved_terminal;
static bool raw_terminal;
static volatile sig_atomic_t resized = 1;

static void on_resize(int signal_number) { (void)signal_number; resized = 1; }

static void restore_terminal(void) {
    if (!raw_terminal) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_terminal);
    const char *leave = "\033[?1006l\033[?1000l\033[0m\033[?25h\033[?1049l";
    if (write(STDOUT_FILENO, leave, strlen(leave)) < 0) { /* nothing left to tell */ }
    raw_terminal = false;
}

static bool raw_mode(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &saved_terminal)) return false;
    struct termios t = saved_terminal;
    t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_cflag |= CS8;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t)) return false;
    raw_terminal = true;
    atexit(restore_terminal);
    const char *enter = "\033[?1049h\033[?25l\033[?1000h\033[?1006h"; /* mouse buttons, reported as SGR */
    return write(STDOUT_FILENO, enter, strlen(enter)) >= 0;
}

/* Whether the app takes Tab itself; otherwise Tab switches typing and keys. */
static bool tab_switches(void) { return screen.prompt >= 0 && key_input("Tab") < 0; }

static void append(const char *bytes, size_t length) {
    if (entry_length + length < sizeof(entry)) { memcpy(entry + entry_length, bytes, length); entry_length += length; }
}

/* The syllable being composed, done: into the entry line. */
static void finish_hangul(void) {
    char done[4];
    append(done, vega_hangul_finish(&hangul, done));
}

/* The entry line as shown: what was typed, then the syllable being composed. */
static size_t shown_entry(char out[VEGA_TEXT + 4]) {
    memcpy(out, entry, entry_length);
    return entry_length + vega_hangul_composing(&hangul, out + entry_length);
}

/* One key from the terminal, as typed text or as a Pure key name. */
static void handle_key(const char *name) {
    int input;
    if (!strcmp(name, "Lang1") || !strcmp(name, "AltRight")) { /* 한/영 */
        finish_hangul();
        korean = !korean;
        return;
    }
    if (!strcmp(name, "Tab") && tab_switches()) { finish_hangul(); typing = !typing; return; }
    if (typing && !strcmp(name, "Enter")) {
        finish_hangul();
        if (entry_length && commit(entry, entry_length)) entry_length = 0;
        return;
    }
    if (typing && !strcmp(name, "Backspace")) {
        if (vega_hangul_back(&hangul)) return; /* a jamo of the syllable being composed */
        while (entry_length && ((unsigned char)entry[entry_length - 1] & 0xC0) == 0x80) --entry_length;
        if (entry_length) --entry_length;
        return;
    }
    press(name, &input);
}

static void handle_text(const char *bytes, size_t length) {
    if (typing) {
        char done[4];
        size_t finished;
        if (korean && length == 1 && vega_hangul_type(&hangul, bytes[0], done, &finished)) {
            append(done, finished);
            return;
        }
        finish_hangul(); /* anything but a letter ends the syllable */
        append(bytes, length);
        return;
    }
    if (length != 1) return;
    char c = bytes[0], name[16];
    if (c == ' ') strcpy(name, "Space");
    else if (c >= 'a' && c <= 'z') snprintf(name, sizeof(name), "Key%c", c - 'a' + 'A');
    else if (c >= 'A' && c <= 'Z') snprintf(name, sizeof(name), "Key%c", c);
    else if (c >= '0' && c <= '9') snprintf(name, sizeof(name), "Digit%c", c);
    else return;
    handle_key(name);
}

/* Terminal input: escape sequences, control keys and UTF-8 text. A read may
 * end inside a key (the terminal's reads are not its keys): an escape
 * sequence, a mouse report or a character cut short waits in pending for
 * the next read. An escape alone that nothing follows within ESCAPE_WAIT ms
 * (vim's ttimeoutlen) is the Escape key; the rest of a key already begun
 * waits up to SEQUENCE_WAIT ms (interactive gives up waiting). */
enum { PENDING_MOST = 64, ESCAPE_WAIT = 100, SEQUENCE_WAIT = 1000 };
static unsigned char pending[PENDING_MOST];
static size_t pending_length;
static double pending_since; /* vega_clock, when what waits began to */

static double pending_wait(void) { return pending_length == 1 && pending[0] == 0x1B ? ESCAPE_WAIT : SEQUENCE_WAIT; }

/* The key at an escape (length bytes from it): the bytes it took, or 0 when
 * it may be cut short and more may come (final: none will). */
static size_t escape(const unsigned char *in, size_t length, bool final) {
    static const struct { const char *sequence, *key; } sequences[] = {
        {"[A", "ArrowUp"}, {"[B", "ArrowDown"}, {"[C", "ArrowRight"}, {"[D", "ArrowLeft"}, {"[H", "Home"},
        {"[F", "End"}, {"OA", "ArrowUp"}, {"OB", "ArrowDown"}, {"OC", "ArrowRight"}, {"OD", "ArrowLeft"},
        {"OH", "Home"}, {"OF", "End"}, {"[1~", "Home"}, {"[7~", "Home"}, {"[4~", "End"}, {"[8~", "End"},
        {"[3~", "Delete"}, {"[5~", "PageUp"}, {"[6~", "PageDown"}};
    if (length >= 3 && in[1] == '[' && in[2] == '<') {
        /* A mouse report: ESC [ < button ; column ; line, M on press, m on release. */
        unsigned fields[3] = {0, 0, 0}, field = 0;
        size_t end = 3;
        for (; end < length && field < 3; ++end) {
            if (in[end] >= '0' && in[end] <= '9') fields[field] = fields[field] * 10 + (unsigned)(in[end] - '0');
            else if (in[end] == ';') ++field;
            else break;
        }
        if (end == length) return final || length == PENDING_MOST ? length : 0; /* cut short (or no report: let go) */
        if (in[end] == 'M' && field == 2 && fields[2]) mouse(fields[0], fields[2] - 1);
        return end + 1;
    }
    bool longer = false; /* what came begins a sequence still to come whole */
    for (size_t i = 0; i < sizeof(sequences) / sizeof(*sequences); ++i) {
        size_t n = strlen(sequences[i].sequence);
        if (1 + n <= length && !memcmp(in + 1, sequences[i].sequence, n)) {
            handle_key(sequences[i].key);
            return 1 + n;
        }
        longer = longer || (1 + n > length && !memcmp(in + 1, sequences[i].sequence, length - 1));
    }
    if (longer && !final) return 0;
    handle_key("Escape");
    return 1;
}

/* What came (after what waited): false on Ctrl-C or Ctrl-Q. */
static bool handle_input(const unsigned char *in, size_t length, bool final) {
    unsigned char bytes[PENDING_MOST + 256];
    size_t total = pending_length;
    memcpy(bytes, pending, pending_length);
    if (length > sizeof(bytes) - total) length = sizeof(bytes) - total;
    if (length) memcpy(bytes + total, in, length);
    total += length;
    pending_length = 0;
    for (size_t at = 0; at < total;) {
        unsigned char c = bytes[at];
        size_t left = total - at, used = 1;
        if (c == 0x03 || c == 0x11) return false; /* Ctrl-C, Ctrl-Q */
        if (c == 0x1B) used = escape(bytes + at, left, final);
        else if (c == '\r' || c == '\n') handle_key("Enter");
        else if (c == 0x7F || c == 0x08) handle_key("Backspace");
        else if (c == '\t') handle_key("Tab");
        else if (c >= 0x20) {
            used = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (used > left) used = final ? left : 0;
            if (used) handle_text((const char *)bytes + at, used);
        }
        if (!used) { /* the rest waits for the next read */
            memcpy(pending, bytes + at, left < PENDING_MOST ? left : PENDING_MOST);
            pending_length = left < PENDING_MOST ? left : PENDING_MOST;
            pending_since = vega_clock();
            return true;
        }
        at += used;
    }
    return true;
}

static int interactive(void) {
    if (!raw_mode()) { fputs("vega: the tui needs a terminal (or use --dump WxH)\n", stderr); return 2; }
    signal(SIGWINCH, on_resize);
    VegaGrid grid = {0};
    drawn = &grid;
    VegaTimers timers;
    vega_timers_start(&timers, app.module, vega_clock());
    for (;;) {
        if (resized) {
            struct winsize size;
            resized = 0;
            if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) && size.ws_col && size.ws_row)
                vega_grid(&grid, size.ws_col, size.ws_row);
            else vega_grid(&grid, 80, 24);
        }
        /* Tick what is due, draw, then wait for input, a frame or the next tick. */
        bool ticked = false;
        long due = vega_timers_run(&timers, &app, vega_clock(), &ticked);
        char line[VEGA_TEXT + 4];
        vega_draw(&grid, &app, &screen, line, shown_entry(line), typing);
        paint_terminal(&grid);
        struct pollfd wait = {STDIN_FILENO, POLLIN, 0};
        int timeout = screen.frame >= 0 ? 16 : watched ? 200 : -1;
        if (due >= 0 && (timeout < 0 || due < timeout)) timeout = (int)due;
        if (pending_length) { /* a key begun waits for its rest (an escape: or is the Escape key) */
            double left = pending_since + pending_wait() - vega_clock();
            if (left < 0) left = 0;
            if (timeout < 0 || left < timeout) timeout = (int)left + 1;
        }
        int ready = poll(&wait, 1, timeout);
        app.now = vega_now();
        if (ready == 0 && pending_length && vega_clock() - pending_since >= pending_wait() &&
            !handle_input(NULL, 0, true))
            break;
        if (ready == 0) {
            vega_frame(&app, &screen);
            if (module_changed()) vega_timers_start(&timers, app.module, vega_clock());
            continue;
        }
        if (ready < 0) continue; /* a signal, such as a resize */
        unsigned char in[256];
        ssize_t got = read(STDIN_FILENO, in, sizeof(in));
        if (got <= 0 || !handle_input(in, (size_t)got, false)) break;
    }
    restore_terminal();
    drawn = NULL;
    vega_grid_free(&grid);
    return 0;
}

/* ---- scripts -------------------------------------------------------- */

typedef enum { SURFACE_CLI, SURFACE_SAY, SURFACE_TUI, SURFACE_WEB, SURFACE_PIXELS, SURFACE_DISPLAY } Surface;

/* The pixel surface's font and where its frames go. */
static VegaFace face;
static const char *frames_prefix;
static unsigned frames;

/* A frame as a binary PPM (P6): prefix-N.ppm. */
static bool write_frame(const VegaGrid *grid, char path[512]) {
    int width = (int)grid->width * face.cell_width, height = (int)grid->height * face.cell_height;
    uint32_t *pixels = malloc((size_t)width * (size_t)height * sizeof(*pixels));
    unsigned char *bytes = malloc((size_t)width * (size_t)height * 3);
    bool ok = pixels && bytes;
    if (ok) {
        vega_pixels(grid, &face, pixels, (size_t)width);
        for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
            bytes[3 * i] = (unsigned char)(pixels[i] >> 16);
            bytes[3 * i + 1] = (unsigned char)(pixels[i] >> 8);
            bytes[3 * i + 2] = (unsigned char)pixels[i];
        }
        snprintf(path, 512, "%s-%u.ppm", frames_prefix, ++frames);
        FILE *f = fopen(path, "wb");
        ok = f && fprintf(f, "P6\n%d %d\n255\n", width, height) > 0 &&
             fwrite(bytes, 3, (size_t)width * (size_t)height, f) == (size_t)width * (size_t)height;
        if (f && fclose(f)) ok = false;
    }
    free(pixels);
    free(bytes);
    return ok;
}

static VegaDisplay *display;

/* A frame on the display; with writeback, what came back is set against it
 * and written (differing: how many pixels are not what was drawn). */
static bool show_frame(const VegaGrid *grid, char path[512], unsigned long *differing, bool compose) {
    unsigned width, height;
    size_t stride, captured_stride;
    uint32_t *out = vega_display_buffer(display, &width, &height, &stride);
    if (compose) {
        vega_pixels_clear(out, stride, width, height);
        vega_pixels(grid, &face, out, stride);
    }
    if (!vega_display_show(display)) return false;
    const uint32_t *back = vega_display_captured(display, &captured_stride);
    *differing = 0;
    path[0] = 0;
    if (!back || !frames_prefix) return true;
    /* The capture read once, a row at a time (it is not cached memory either). */
    uint32_t *row = malloc((size_t)width * sizeof(*row));
    unsigned char *rgb = malloc((size_t)width * height * 3);
    bool ok = row && rgb;
    for (unsigned y = 0; ok && y < height; ++y) {
        memcpy(row, back + (size_t)y * captured_stride, (size_t)width * sizeof(*row));
        for (unsigned x = 0; x < width; ++x) {
            *differing += (row[x] & 0xffffff) != (out[(size_t)y * stride + x] & 0xffffff);
            unsigned char *p = rgb + ((size_t)y * width + x) * 3;
            p[0] = (unsigned char)(row[x] >> 16), p[1] = (unsigned char)(row[x] >> 8), p[2] = (unsigned char)row[x];
        }
    }
    snprintf(path, 512, "%s-%u.ppm", frames_prefix, ++frames);
    FILE *f = ok ? fopen(path, "wb") : NULL;
    ok = f && fprintf(f, "P6\n%u %u\n255\n", width, height) > 0 && fwrite(rgb, 3, (size_t)width * height, f) == (size_t)width * height;
    if (f && fclose(f)) ok = false;
    free(row);
    free(rgb);
    return ok;
}


/* ---- the display, live ---------------------------------------------- */

static volatile sig_atomic_t stopping;
static void on_stop(int signal) { (void)signal; stopping = 1; }

/* Accessibility (--access): what the surface tells it, and does for it. */
static bool accessible;
static bool access_typing(void *context) { (void)context; return typing; }
static const char *access_typed(void *context, size_t *length) {
    (void)context;
    *length = entry_length;
    return entry;
}
static bool access_press(void *context, const char *key) {
    (void)context;
    int input;
    return press(key, &input);
}
static bool access_acted; /* assistive technology changed the app: draw again */
static void access_changed(void *context) {
    (void)context;
    access_acted = true;
}
static const VegaAccessHost access_host = {NULL, access_typing, access_typed, access_press, access_changed};

static void show_pointer(uint32_t *out, size_t stride, int x, int y, unsigned width, unsigned height) {
    /* The cell under a mouse pointer, outlined. */
    int left = x / face.cell_width * face.cell_width, top = y / face.cell_height * face.cell_height;
    for (int i = 0; i < face.cell_width; ++i)
        for (int j = 0; j < face.cell_height; ++j)
            if ((!i || !j || i == face.cell_width - 1 || j == face.cell_height - 1) && left + i < (int)width && top + j < (int)height)
                out[(size_t)(top + j) * stride + (size_t)(left + i)] = 0xf5e0dc;
}

/* The last frame of recorded events: its text, and where its pixels went. */
static void print_last(const VegaGrid *grid, const char *path, unsigned long differing) {
    dump(grid);
    if (path[0]) printf("frame %s differing %lu grid %ux%u cell %dx%d\n", path, differing, grid->width, grid->height,
                        face.cell_width, face.cell_height);
    fflush(stdout);
}

/* Draw, show, then wait for input or the next tick, until a signal (or the
 * end of recorded events: the last frame is then printed as text). In a
 * node: draw its last save, wait for input or its next save, until it stops
 * (recorded events printed when they end, the saves drawn on); its owner
 * runs the timers, and the signals are its own. */
static int live(VegaInput *input, unsigned columns, unsigned rows, bool recorded) {
    VegaGrid grid = {0};
    if (!vega_grid(&grid, columns, rows)) return 2;
    drawn = &grid;
    if (!node) {
        signal(SIGINT, on_stop);
        signal(SIGTERM, on_stop);
    }
    VegaTimers timers;
    vega_timers_start(&timers, app.module, vega_clock());
    char path[512] = "";
    unsigned long differing = 0;
    bool open_input = true, printed = false;
    int status = 0;
    VegaAccess *access = NULL;
    if (accessible) {
        char error[256];
        if (!(access = vega_access_open(&app, &screen, &access_host, error, sizeof(error))))
            fprintf(stderr, "vega: no accessibility: %s\n", error);
    }
    /* A frame is drawn when something may have changed it: first, then
     * after input, ticks, a node's save, or what assistive technology did
     * (not for what it only asked). */
    bool dirty = true;
    while (!stopping && !(node && node->stopping(node->host))) {
        bool ticked = false;
        long due = node ? -1 : vega_timers_run(&timers, &app, vega_clock(), &ticked);
        PureSnapshot *snapshot = node ? node->read(node->host, &app) : NULL;
        dirty = dirty || ticked || snapshot || access_acted;
        access_acted = false;
        char line[VEGA_TEXT + 4];
        if (dirty && (!node || snapshot)) vega_draw(&grid, &app, &screen, line, shown_entry(line), typing);
        if (snapshot) node->release(node->host, snapshot);
        if (!dirty) {
            /* nothing to draw */
        } else if (!display) { /* a node's pixels: each frame into a file */
            if (!write_frame(&grid, path)) { fprintf(stderr, "vega: cannot write %s\n", path); status = 1; break; }
        } else {
            unsigned width, height;
            size_t stride;
            uint32_t *out = vega_display_buffer(display, &width, &height, &stride);
            vega_pixels_clear(out, stride, width, height);
            vega_pixels(&grid, &face, out, stride);
            int x, y;
            bool pointer;
            vega_input_pointer(input, &x, &y, &pointer);
            if (pointer) show_pointer(out, stride, x, y, width, height);
            if (!show_frame(&grid, path, &differing, false)) { fputs("vega: the display did not take the frame\n", stderr); status = 1; break; }
        }
        dirty = false;
        if (access) vega_access_changed(access);
        if (!open_input && !printed && recorded) {
            print_last(&grid, path, differing);
            printed = true;
        }
        if (!open_input && !node) break;
        struct pollfd waits[34];
        unsigned count = open_input ? vega_input_fds(input, waits, 32) : 0;
        if (node) waits[count++] = (struct pollfd){node->changes, POLLIN, 0}; /* the node saved: draw again */
        if (access) waits[count++] = (struct pollfd){vega_access_fd(access), POLLIN, 0}; /* a screen reader asks */
        int timeout = screen.frame >= 0 && !node ? 16 : -1;
        if (due >= 0 && (timeout < 0 || due < timeout)) timeout = (int)due;
        if (poll(waits, count, timeout) <= 0) {
            if (node) continue;
            app.now = vega_now();
            if (screen.frame >= 0) vega_frame(&app, &screen), dirty = true;
            continue;
        }
        if (!node) app.now = vega_now();
        for (unsigned i = 0; i < count; ++i) {
            if (!(waits[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (node && waits[i].fd == node->changes) {
                uint64_t saves;
                if (read(node->changes, &saves, sizeof(saves)) < 0) { /* cleared already */ }
                continue;
            }
            if (access && waits[i].fd == vega_access_fd(access)) {
                if (!vega_access_read(access)) {
                    fputs("vega: the accessibility bus went away\n", stderr);
                    vega_access_close(access);
                    access = NULL;
                }
                continue;
            }
            VegaEvent events[64];
            bool ended;
            unsigned n = vega_input_read(input, waits[i].fd, events, 64, &ended);
            if (n || ended) dirty = true;
            for (unsigned k = 0; k < n; ++k) {
                const VegaEvent *e = &events[k];
                unsigned line = e->y >= 0 ? (unsigned)(e->y / face.cell_height) : 0;
                if (e->kind == VEGA_EVENT_TEXT && typing) handle_text(e->text, strlen(e->text));
                else if (e->kind == VEGA_EVENT_TEXT || e->kind == VEGA_EVENT_KEY) handle_key(e->name);
                else if (e->kind == VEGA_EVENT_PRESS) mouse(0, line);
                else if (e->kind == VEGA_EVENT_WHEEL) mouse(e->steps < 0 ? 64 : 65, line);
            }
            if (ended && recorded) open_input = false; /* draw once more, then stop (or follow the node's saves) */
        }
    }
    if (recorded && !printed) print_last(&grid, path, differing);
    vega_access_close(access);
    drawn = NULL;
    vega_grid_free(&grid);
    return status;
}

/* painting: show writes the terminal bytes of the frame, not the grid. */
static int script(Surface surface, unsigned width, unsigned height, bool painting) {
    static char line[1 << 16];
    VegaGrid grid = {0};
    bool gridded = surface == SURFACE_TUI || surface == SURFACE_PIXELS || surface == SURFACE_DISPLAY;
    if (gridded && !vega_grid(&grid, width, height)) return 2;
    bool shown = false;
    drawn = gridded ? &grid : NULL;
    bool color = surface == SURFACE_CLI && isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
    unsigned long counted = vega_evaluations();
    while (fgets(line, sizeof(line), stdin)) {
        char command[16] = "", argument[sizeof(line)] = "";
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %65535s", command, argument) < 1) continue;
        bool acted = true, ok = true;
        int input = -1;
        const char *verb = "do that";
        if (!strcmp(command, "key")) {
            ok = press(argument, &input);
            if (input < 0) fprintf(stderr, "vega: no input answers %s\n", argument);
        } else if (!strcmp(command, "text")) {
            char text[VEGA_TEXT];
            ok = commit(text, unhex(argument, text, sizeof(text)));
        } else if (!strcmp(command, "type")) {
            entry_length += unhex(argument, entry + entry_length, sizeof(entry) - 1 - entry_length);
            acted = false;
        } else if (!strcmp(command, "enter")) {
            handle_key("Enter");
        } else if (!strcmp(command, "tab")) {
            handle_key("Tab");
            acted = false;
        } else if (!strcmp(command, "hangul") || !strcmp(command, "back")) {
            handle_key(!strcmp(command, "hangul") ? "Lang1" : "Backspace");
            acted = false;
        } else if (!strcmp(command, "letters")) {
            char keys[VEGA_TEXT];
            size_t count = unhex(argument, keys, sizeof(keys));
            for (size_t i = 0; i < count; ++i) handle_text(keys + i, 1);
            acted = false;
        } else if (!strcmp(command, "point")) {
            char name[PURE_MODULE_NAME];
            long row = -1;
            const VegaPart *part = sscanf(line, "%*s %31s %ld", name, &row) == 2 ? part_named(name) : NULL;
            verb = "point";
            if (!part) fprintf(stderr, "vega: no list or table %s\n", name);
            ok = part && row >= 0 && vega_point(&app, part, (size_t)row);
        } else if (!strcmp(command, "click")) {
            unsigned at_line = 0, at_column = 0;
            verb = "point";
            if (!gridded) { fputs("vega: click needs the tui or pixels\n", stderr); continue; }
            acted = sscanf(line, "%*s %u %u", &at_line, &at_column) == 2 && at_line && shown;
            ok = acted && mouse(0, at_line - 1);
        } else if (!strcmp(command, "time")) {
            /* The time the next events see: scripts replay any. */
            char *end;
            double now = strtod(argument, &end);
            acted = false;
            if (!*argument || *end || !(now >= 0 && now < 1e15)) fprintf(stderr, "vega: time takes milliseconds\n");
            else app.now = now;
        } else if (!strcmp(command, "tick")) {
            input = pure_module_input(app.module, argument);
            verb = "tick";
            if (input < 0 || app.module->input[input].binding != PURE_BIND_TIMER) {
                fprintf(stderr, "vega: no timer %s\n", argument);
                input = -1;
                ok = false;
            } else ok = pure_app_message(&app, (uint32_t)input, (PureSpan){0}) == PURE_OK;
        } else if (!strcmp(command, "swap")) {
            acted = false;
            if (!swap(argument, read_module(argument))) fprintf(stderr, "vega: kept the running module\n");
        } else if (!strcmp(command, "frame")) {
            long count = *argument ? strtol(argument, NULL, 10) : 1;
            for (long i = 0; i < count && i < 600; ++i) ok = (screen.frame < 0 || vega_frame(&app, &screen)) && ok;
        } else if (!strcmp(command, "show")) {
            acted = false;
            if (surface == SURFACE_CLI) vega_print(stdout, &app, &screen, color);
            else if (surface == SURFACE_SAY) vega_say(stdout, &app, &screen, true);
            else if ((shown = true) && painting) {
                Out out = {0};
                char prompt[VEGA_TEXT + 4];
                vega_draw(&grid, &app, &screen, prompt, shown_entry(prompt), typing);
                paint(&grid, &out);
                printf("paint %zu\n", out.used);
                fwrite(out.bytes, 1, out.used, stdout);
                putchar('\n');
                free(out.bytes);
            } else {
                char prompt[VEGA_TEXT + 4];
                vega_draw(&grid, &app, &screen, prompt, shown_entry(prompt), typing);
                dump(&grid);
                printf("cells %lu\n", vega_evaluations() - counted);
                char path[512];
                unsigned long differing;
                if (surface == SURFACE_PIXELS) {
                    if (!write_frame(&grid, path)) { fprintf(stderr, "vega: cannot write %s\n", path); return 1; }
                    printf("frame %s %dx%d\n", path, (int)grid.width * face.cell_width, (int)grid.height * face.cell_height);
                } else if (surface == SURFACE_DISPLAY) {
                    if (!show_frame(&grid, path, &differing, true)) { fputs("vega: the display did not take the frame\n", stderr); return 1; }
                    if (path[0]) printf("frame %s differing %lu grid %ux%u cell %dx%d\n", path, differing, grid.width, grid.height, face.cell_width,
                   face.cell_height);
                }
            }
            counted = vega_evaluations();
        } else {
            fprintf(stderr, "vega: unknown command %s\n", command);
            continue;
        }
        if (!ok && input >= 0) fprintf(stderr, "vega: %s failed\n", app.module->input_names[input]);
        if (surface == SURFACE_SAY && acted) {
            if (!ok) printf("Could not %s.\n", input >= 0 ? app.module->input_names[input] : verb);
            else vega_say(stdout, &app, &screen, false);
        }
        fflush(stdout);
    }
    drawn = NULL;
    vega_grid_free(&grid);
    return 0;
}

/* Surfaces that draw glyphs. */
static bool glyphed(const char *surface) { return !strcmp(surface, "pixels") || !strcmp(surface, "display"); }

/* The fonts a pixel surface draws with (--symbols, in order of
 * preference), read whole and kept. */
static const char *symbols_paths[VEGA_FACES];
static unsigned symbols_count;
static bool add_symbols(const char *path) {
    if (symbols_count == VEGA_FACES) return false;
    symbols_paths[symbols_count++] = path;
    return true;
}
static bool open_face(float size) {
    static VegaSymbols symbols[VEGA_FACES];
    const VegaSymbols *opened[VEGA_FACES];
    char error[160] = "cannot read it";
    for (unsigned k = 0; k < symbols_count; ++k) {
        const char *path = symbols_paths[k];
        FILE *f = fopen(path, "rb");
        long length = f && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
        unsigned char *bytes = length > 0 && !fseek(f, 0, SEEK_SET) ? malloc((size_t)length) : NULL;
        bool ok = bytes && fread(bytes, 1, (size_t)length, f) == (size_t)length;
        if (f) fclose(f);
        if (!ok || !vega_symbols_open(&symbols[k], bytes, (size_t)length, error, sizeof(error))) {
            fprintf(stderr, "vega: %s: %s\n", path, error);
            return false;
        }
        opened[k] = &symbols[k]; /* the bytes stay: the face reads them */
    }
    if (!vega_face_open(&face, opened, symbols_count, size, error, sizeof(error))) {
        fprintf(stderr, "vega: %s: %s\n", symbols_paths[0], error);
        return false;
    }
    return true;
}

#ifdef VEGA_SURFACE
/* Vega in a Pure node (serve/surface.h): pure-serve ... --surface
 * vega-surface.so -- display --symbols FONT.vsym [--size PX] [--card PATH]
 * [--writeback WIDTHxHEIGHT --out PREFIX] [--live | --input EVENTS], or
 * pixels --symbols FONT.vsym [--size PX] --grid WIDTHxHEIGHT --out PREFIX
 * [--input EVENTS]. The display (or each frame's pixels, into PREFIX-N.ppm)
 * shows the node's app as of each save; keys and text go to its owner.
 * Recorded events are read to their end, then the last frame is printed as
 * text (on the node's standard output); otherwise it runs until the node
 * stops, drawn again after each save. */
__attribute__((visibility("default"))) int pure_surface_run(const PureSurface *surface, int argc, char **argv) {
    if (surface->version != PURE_SURFACE_VERSION || surface->app_size != sizeof(PureApp) ||
        surface->module_size != sizeof(PureModule)) {
        fputs("vega: this surface was built for another Pure\n", stderr);
        return 2;
    }
    const char *card = "/dev/dri/card1", *recorded = NULL;
    unsigned width = 0, height = 0;
    float size = 16;
    bool on_display = argc > 1 && !strcmp(argv[1], "display"), pixels = argc > 1 && !strcmp(argv[1], "pixels");
    bool writeback = false, live_input = false, usage = !on_display && !pixels;
    for (int i = 2; !usage && i < argc; i += 2) {
        if (!strcmp(argv[i], "--live") && on_display) {
            live_input = true;
            --i;
            continue;
        }
        if (i + 1 >= argc) usage = true;
        else if (!strcmp(argv[i], "--symbols")) usage = !add_symbols(argv[i + 1]);
        else if (!strcmp(argv[i], "--size") && sscanf(argv[i + 1], "%f", &size) == 1) {}
        else if (!strcmp(argv[i], "--card") && on_display) card = argv[i + 1];
        else if (!strcmp(argv[i], "--input")) recorded = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) frames_prefix = argv[i + 1];
        else if ((!strcmp(argv[i], "--writeback") && on_display) || (!strcmp(argv[i], "--grid") && pixels)) {
            if (sscanf(argv[i + 1], "%ux%u", &width, &height) != 2 || !width || !height || (pixels && (width > 1000 || height > 1000)))
                usage = true;
            writeback = on_display;
        } else usage = true;
    }
    if (usage || !symbols_count || (writeback && !frames_prefix) || (pixels && (!width || !frames_prefix)) ||
        (live_input && recorded)) {
        fputs("vega-surface.so display --symbols FONT.vsym... [--size PX] [--card PATH] [--writeback WIDTHxHEIGHT --out PREFIX]\n"
              "                        [--live | --input EVENTS]\n"
              "vega-surface.so pixels --symbols FONT.vsym... [--size PX] --grid WIDTHxHEIGHT --out PREFIX [--input EVENTS]\n",
              stderr);
        return 2;
    }
    if (!open_face(size)) return 2;
    unsigned columns = width, rows = height, pixels_across = width * (unsigned)face.cell_width,
             pixels_down = height * (unsigned)face.cell_height;
    if (on_display) {
        char error[256];
        if (!(display = vega_display_open(card, writeback, writeback ? width : 0, writeback ? height : 0, error, sizeof(error)))) {
            fprintf(stderr, "vega: %s\n", error);
            return 2;
        }
        size_t stride;
        vega_display_buffer(display, &pixels_across, &pixels_down, &stride);
        columns = pixels_across / (unsigned)face.cell_width;
        rows = pixels_down / (unsigned)face.cell_height;
    }
    node = surface;
    vega_screen(&screen, surface->module);
    typing = screen.prompt >= 0;
    const char *paths[1] = {recorded};
    /* Without --live or --input, no devices: the node's saves alone draw it. */
    VegaInput *input = vega_input_open(live_input || recorded ? paths : NULL, recorded ? 1 : 0, pixels_across, pixels_down);
    int status = 2;
    if (!input) fprintf(stderr, "vega: cannot read %s\n", recorded ? recorded : "input devices");
    else {
        PureSnapshot *first = node->read(node->host, &app); /* app shows the node's states from here on */
        if (first) node->release(node->host, first);
        status = first ? live(input, columns, rows, recorded != NULL) : 2;
        vega_input_close(input);
    }
    pure_app_stop(&app);
    vega_display_close(display);
    display = NULL;
    node = NULL;
    return status;
}
#else
int main(int argc, char **argv) {
    unsigned width = 0, height = 0, port = 8080;
    const char *card = "/dev/dri/card1";
    float size = 16;
    bool writeback = false, live_input = false;
    const char *recorded = NULL;
    bool dumped = false, painting = false, watch = false, usage = argc < 3;
    const char *store = NULL, *listen_on = "127.0.0.1";
    for (int i = 3; !usage && i < argc; i += 2) {
        if (!strcmp(argv[i], "--watch") && (!strcmp(argv[1], "tui") || !strcmp(argv[1], "web"))) {
            watch = true;
            --i; /* a flag without a value */
            continue;
        }
        if (!strcmp(argv[i], "--live") && !strcmp(argv[1], "display")) {
            live_input = true;
            --i;
            continue;
        }
        if (!strcmp(argv[i], "--access") && !strcmp(argv[1], "display")) {
            accessible = true;
            --i;
            continue;
        }
        if (i + 1 >= argc) usage = true;
        else if (!strcmp(argv[i], "--store")) store = argv[i + 1];
        else if (!strcmp(argv[i], "--listen") && !strcmp(argv[1], "web")) listen_on = argv[i + 1];
        else if (!strcmp(argv[i], "--port") && !strcmp(argv[1], "web") && sscanf(argv[i + 1], "%u", &port) == 1 &&
                 port < 65536) {}
        else if ((!strcmp(argv[i], "--dump") || !strcmp(argv[i], "--paint") ||
                  (!strcmp(argv[i], "--grid") && !strcmp(argv[1], "pixels"))) &&
                 sscanf(argv[i + 1], "%ux%u", &width, &height) == 2 && width && height && width <= 1000 && height <= 1000)
            dumped = true, painting = !strcmp(argv[i], "--paint");
        else if (!strcmp(argv[i], "--writeback") && !strcmp(argv[1], "display") &&
                 sscanf(argv[i + 1], "%ux%u", &width, &height) == 2 && width && height)
            writeback = true;
        else if (!strcmp(argv[i], "--card") && !strcmp(argv[1], "display")) card = argv[i + 1];
        else if (!strcmp(argv[i], "--input") && !strcmp(argv[1], "display")) recorded = argv[i + 1];
        else if (!strcmp(argv[i], "--symbols") && glyphed(argv[1])) usage = !add_symbols(argv[i + 1]);
        else if (!strcmp(argv[i], "--out") && glyphed(argv[1])) frames_prefix = argv[i + 1];
        else if (!strcmp(argv[i], "--size") && glyphed(argv[1]) && sscanf(argv[i + 1], "%f", &size) == 1) {}
        else usage = true;
    }
    bool pixels = argc > 1 && !strcmp(argv[1], "pixels"), on_display = argc > 1 && !strcmp(argv[1], "display");
    if (usage || (dumped && strcmp(argv[1], "tui") && !pixels) || (!strcmp(argv[1], "check") && argc != 3) ||
        (pixels && (!dumped || !symbols_count || !frames_prefix)) || (on_display && (!symbols_count || (writeback && !frames_prefix)))) {
        fputs("vega tui|cli|say MODULE.pmod [--store PATH]\n"
              "vega tui MODULE.pmod [--store PATH] [--watch] | --dump|--paint WIDTHxHEIGHT < script\n"
              "vega web MODULE.pmod [--store PATH] [--listen ADDRESS] [--port N] [--watch]\n"
              "vega pixels MODULE.pmod [--store PATH] --symbols FONT.vsym... [--size PX] --grid WIDTHxHEIGHT --out PREFIX < script\n"
              "vega display MODULE.pmod [--store PATH] --symbols FONT.vsym... [--size PX] [--card PATH]\n"
              "             [--writeback WIDTHxHEIGHT --out PREFIX] [--live | --input EVENTS] [--access] [< script]\n"
              "vega check MODULE.pmod\n", stderr);
        return 2;
    }
    if ((pixels || on_display) && !open_face(size)) return 2;
    if (on_display) {
        char error[256];
        if (!(display = vega_display_open(card, writeback, writeback ? width : 0, writeback ? height : 0, error, sizeof(error)))) {
            fprintf(stderr, "vega: %s\n", error);
            return 2;
        }
        unsigned pixels_across, pixels_down;
        size_t stride;
        vega_display_buffer(display, &pixels_across, &pixels_down, &stride);
        width = pixels_across / (unsigned)face.cell_width;
        height = pixels_down / (unsigned)face.cell_height;
    }
    if (!strcmp(argv[1], "check")) {
        if (!load_module(argv[2], &modules[0])) return 2;
        return vega_check(&modules[0], print_finding, argv[2]) ? 1 : 0;
    }
    Surface surface = !strcmp(argv[1], "cli") ? SURFACE_CLI : !strcmp(argv[1], "say") ? SURFACE_SAY :
                      !strcmp(argv[1], "tui") ? SURFACE_TUI : !strcmp(argv[1], "web") ? SURFACE_WEB :
                      pixels ? SURFACE_PIXELS : on_display ? SURFACE_DISPLAY : (Surface)-1;
    if ((int)surface < 0) { fprintf(stderr, "vega: unknown surface %s\n", argv[1]); return 2; }
    if (!load(argv[2], store)) return 2;
    if (watch && !dumped) {
        watched = argv[2];
        watched_digest = module_digest; /* of what load read */
    }
    int status;
    if (on_display && (live_input || recorded)) {
        unsigned pixels_across, pixels_down;
        size_t stride;
        vega_display_buffer(display, &pixels_across, &pixels_down, &stride);
        const char *paths[1] = {recorded};
        VegaInput *input = vega_input_open(paths, recorded ? 1 : 0, pixels_across, pixels_down);
        if (!input) { fprintf(stderr, "vega: cannot read %s\n", recorded ? recorded : "input devices"); return 2; }
        status = live(input, width, height, recorded != NULL);
        vega_input_close(input);
    } else
        status = surface == SURFACE_WEB ? vega_web(&app, &screen, listen_on, port, watched ? module_changed : NULL) :
                 surface == SURFACE_TUI && !dumped ? interactive() : script(surface, width, height, painting);
    /* Frame changes are stored with events; keep the last ones too. */
    if (pure_app_save(&app) != PURE_OK) { fputs("vega: cannot save\n", stderr); status = status ? status : 1; }
    pure_app_stop(&app);
    vega_display_close(display);
    return status;
}
#endif
