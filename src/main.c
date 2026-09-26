#define _DEFAULT_SOURCE
#include "vega.h"
#include "module-store.h"
#include "store.h"
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* vega SURFACE MODULE [--store PATH]: one module, one screen, several surfaces.
 *   check            what the module seems to mean but does not meet (exit 1)
 *   tui              interactive terminal
 *   tui --dump WxH   the terminal's frames as text, driven by a script
 *   tui --paint WxH  the bytes each frame sends to the terminal, driven by a script
 *   cli              plain lines for pipes, driven by a script
 *   say              sentences (the first natural-language surface), driven by a script
 * With --store the app's states live in that tree store: the app continues
 * where it was, whichever surface runs it.
 * Script commands, one per line: key KEYNAME | text HEX|- | type HEX | enter |
 * tab | frame [N] | show. key presses and releases the input bound to that key;
 * text commits text to the screen's text input; type, enter and tab act on the
 * TUI's input line. */

static PureModule module;
static PureApp app;
static VegaScreen screen;
static char entry[VEGA_TEXT];
static size_t entry_length;
static bool typing;

static bool load_module(const char *path) {
    static unsigned char bytes[1 << 17];
    FILE *file = fopen(path, "rb");
    size_t size = file ? fread(bytes, 1, sizeof(bytes), file) : 0;
    bool ok = file && !ferror(file) && size < sizeof(bytes);
    if (file) fclose(file);
    if (!ok) { fprintf(stderr, "vega: cannot read %s\n", path); return false; }
    char error[128];
    if (pure_module_load(bytes, size, &module, error, sizeof(error)) != PURE_OK) {
        fprintf(stderr, "vega: %s: %s\n", path, error);
        return false;
    }
    return true;
}

static void print_finding(void *path, const char *name, const char *message) {
    printf("%s: %s: %s\n", (const char *)path, name, message);
}

static bool load(const char *path, const char *store) {
    if (!load_module(path)) return false;
    if (store && pure_store_init(store) != PURE_OK) { fprintf(stderr, "vega: cannot open store %s\n", store); return false; }
    PureResult started = pure_app_start(&app, &module, store ? pure_tree_data(store) : (PureData){0});
    if (started != PURE_OK) { fprintf(stderr, "vega: cannot start (%d)\n", (int)started); return false; }
    vega_screen(&screen, &module);
    typing = screen.prompt >= 0;
    return true;
}

static int key_input(const char *key) {
    for (uint32_t i = 0; i < module.inputs; ++i)
        if (module.input[i].kind == PURE_KIND_KEY && !strcmp(module.input_keys[i], key)) return (int)i;
    return -1;
}

static bool frames(void) {
    for (uint32_t h = 0; h < module.handlers; ++h) if (module.handler[h].event == 0) return true;
    return false;
}

/* A key press and release; false when the app has no such key or the press failed. */
static bool press(const char *key, int *input) {
    *input = key_input(key);
    if (*input < 0) return false;
    bool ok = pure_app_input(&app, (uint32_t)*input, PURE_EDGE_DOWN) == PURE_OK;
    pure_app_input(&app, (uint32_t)*input, PURE_EDGE_UP);
    return ok;
}

static bool commit(const char *text, size_t length) {
    if (screen.prompt < 0) return false;
    return pure_app_text(&app, (uint32_t)screen.prompt, (PureSpan){(const unsigned char *)text, length}) == PURE_OK;
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
    const char *leave = "\033[0m\033[?25h\033[?1049l";
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
    const char *enter = "\033[?1049h\033[?25l";
    return write(STDOUT_FILENO, enter, strlen(enter)) >= 0;
}

/* Whether the app takes Tab itself; otherwise Tab switches typing and keys. */
static bool tab_switches(void) { return screen.prompt >= 0 && key_input("Tab") < 0; }

/* One key from the terminal, as typed text or as a Pure key name. */
static void handle_key(const char *name) {
    int input;
    if (!strcmp(name, "Tab") && tab_switches()) { typing = !typing; return; }
    if (typing && !strcmp(name, "Enter")) {
        if (entry_length && commit(entry, entry_length)) entry_length = 0;
        return;
    }
    if (typing && !strcmp(name, "Backspace")) {
        while (entry_length && ((unsigned char)entry[entry_length - 1] & 0xC0) == 0x80) --entry_length;
        if (entry_length) --entry_length;
        return;
    }
    press(name, &input);
}

static void handle_text(const char *bytes, size_t length) {
    if (typing) {
        if (entry_length + length < sizeof(entry)) { memcpy(entry + entry_length, bytes, length); entry_length += length; }
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

/* Parse terminal input: escape sequences, control keys and UTF-8 text. */
static bool handle_input(const unsigned char *in, size_t length) {
    static const struct { const char *sequence, *key; } sequences[] = {
        {"[A", "ArrowUp"}, {"[B", "ArrowDown"}, {"[C", "ArrowRight"}, {"[D", "ArrowLeft"}, {"[H", "Home"},
        {"[F", "End"}, {"OA", "ArrowUp"}, {"OB", "ArrowDown"}, {"OC", "ArrowRight"}, {"OD", "ArrowLeft"},
        {"OH", "Home"}, {"OF", "End"}, {"[1~", "Home"}, {"[7~", "Home"}, {"[4~", "End"}, {"[8~", "End"},
        {"[3~", "Delete"}, {"[5~", "PageUp"}, {"[6~", "PageDown"}};
    for (size_t at = 0; at < length;) {
        unsigned char c = in[at];
        if (c == 0x03 || c == 0x11) return false; /* Ctrl-C, Ctrl-Q */
        if (c == 0x1B) {
            size_t matched = 0;
            for (size_t i = 0; i < sizeof(sequences) / sizeof(*sequences) && !matched; ++i) {
                size_t n = strlen(sequences[i].sequence);
                if (at + 1 + n <= length && !memcmp(in + at + 1, sequences[i].sequence, n)) {
                    handle_key(sequences[i].key);
                    matched = 1 + n;
                }
            }
            if (!matched) { handle_key("Escape"); matched = 1; }
            at += matched;
            continue;
        }
        if (c == '\r' || c == '\n') handle_key("Enter");
        else if (c == 0x7F || c == 0x08) handle_key("Backspace");
        else if (c == '\t') handle_key("Tab");
        else if (c >= 0x20) {
            size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (at + n > length) n = length - at;
            handle_text((const char *)in + at, n);
            at += n;
            continue;
        }
        ++at;
    }
    return true;
}

static int interactive(void) {
    if (!raw_mode()) { fputs("vega: the tui needs a terminal (or use --dump WxH)\n", stderr); return 2; }
    signal(SIGWINCH, on_resize);
    VegaGrid grid = {0};
    bool ticking = frames();
    for (;;) {
        if (resized) {
            struct winsize size;
            resized = 0;
            if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) && size.ws_col && size.ws_row)
                vega_grid(&grid, size.ws_col, size.ws_row);
            else vega_grid(&grid, 80, 24);
        }
        vega_draw(&grid, &app, &screen, entry, entry_length, typing);
        paint_terminal(&grid);
        struct pollfd wait = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&wait, 1, ticking ? 16 : -1);
        if (ready == 0) { pure_app_frame(&app); continue; }
        if (ready < 0) continue; /* a signal, such as a resize */
        unsigned char in[256];
        ssize_t got = read(STDIN_FILENO, in, sizeof(in));
        if (got <= 0 || !handle_input(in, (size_t)got)) break;
    }
    restore_terminal();
    vega_grid_free(&grid);
    return 0;
}

/* ---- scripts -------------------------------------------------------- */

typedef enum { SURFACE_CLI, SURFACE_SAY, SURFACE_TUI } Surface;

/* painting: show writes the terminal bytes of the frame, not the grid. */
static int script(Surface surface, unsigned width, unsigned height, bool painting) {
    static char line[1 << 16];
    VegaGrid grid = {0};
    if (surface == SURFACE_TUI && !vega_grid(&grid, width, height)) return 2;
    bool color = surface == SURFACE_CLI && isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
    unsigned long counted = vega_evaluations();
    while (fgets(line, sizeof(line), stdin)) {
        char command[16] = "", argument[sizeof(line)] = "";
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%15s %65535s", command, argument) < 1) continue;
        bool acted = true, ok = true;
        int input = -1;
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
        } else if (!strcmp(command, "frame")) {
            long count = *argument ? strtol(argument, NULL, 10) : 1;
            for (long i = 0; i < count && i < 600; ++i) ok = pure_app_frame(&app) == PURE_OK && ok;
        } else if (!strcmp(command, "show")) {
            acted = false;
            if (surface == SURFACE_CLI) vega_print(stdout, &app, &screen, color);
            else if (surface == SURFACE_SAY) vega_say(stdout, &app, &screen, true);
            else if (painting) {
                Out out = {0};
                vega_draw(&grid, &app, &screen, entry, entry_length, typing);
                paint(&grid, &out);
                printf("paint %zu\n", out.used);
                fwrite(out.bytes, 1, out.used, stdout);
                putchar('\n');
                free(out.bytes);
            } else {
                vega_draw(&grid, &app, &screen, entry, entry_length, typing);
                dump(&grid);
                printf("cells %lu\n", vega_evaluations() - counted);
            }
            counted = vega_evaluations();
        } else {
            fprintf(stderr, "vega: unknown command %s\n", command);
            continue;
        }
        if (!ok && input >= 0) fprintf(stderr, "vega: %s failed\n", module.input_names[input]);
        if (surface == SURFACE_SAY && acted) {
            if (!ok) printf("Could not %s.\n", input >= 0 ? module.input_names[input] : "do that");
            else vega_say(stdout, &app, &screen, false);
        }
        fflush(stdout);
    }
    vega_grid_free(&grid);
    return 0;
}

int main(int argc, char **argv) {
    unsigned width = 0, height = 0;
    bool dumped = false, painting = false, usage = argc < 3 || argc % 2 == 0;
    const char *store = NULL;
    for (int i = 3; !usage && i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--store")) store = argv[i + 1];
        else if ((!strcmp(argv[i], "--dump") || !strcmp(argv[i], "--paint")) &&
                 sscanf(argv[i + 1], "%ux%u", &width, &height) == 2 && width && height && width <= 1000 && height <= 1000)
            dumped = true, painting = !strcmp(argv[i], "--paint");
        else usage = true;
    }
    if (usage || (dumped && strcmp(argv[1], "tui")) || (!strcmp(argv[1], "check") && argc != 3)) {
        fputs("vega tui|cli|say MODULE.pmod [--store PATH]\n"
              "vega tui MODULE.pmod [--store PATH] --dump|--paint WIDTHxHEIGHT < script\n"
              "vega check MODULE.pmod\n", stderr);
        return 2;
    }
    if (!strcmp(argv[1], "check")) {
        if (!load_module(argv[2])) return 2;
        return vega_check(&module, print_finding, argv[2]) ? 1 : 0;
    }
    Surface surface = !strcmp(argv[1], "cli") ? SURFACE_CLI : !strcmp(argv[1], "say") ? SURFACE_SAY :
                      !strcmp(argv[1], "tui") ? SURFACE_TUI : (Surface)-1;
    if ((int)surface < 0) { fprintf(stderr, "vega: unknown surface %s\n", argv[1]); return 2; }
    if (!load(argv[2], store)) return 2;
    int status = surface == SURFACE_TUI && !dumped ? interactive() : script(surface, width, height, painting);
    /* Frame changes are stored with events; keep the last ones too. */
    if (pure_app_save(&app) != PURE_OK) { fputs("vega: cannot save\n", stderr); status = status ? status : 1; }
    pure_app_stop(&app);
    return status;
}
