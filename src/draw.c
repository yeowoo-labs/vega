#include "vega.h"
#include <stdlib.h>
#include <string.h>

static const char *const tone_names[VEGA_TONES] = {"normal", "muted", "accent", "good", "warning", "danger"};

/* ---- CLI ------------------------------------------------------------ */

/* Text for a pipe: control characters become their pictures so a value never
 * breaks a line. */
static void put_text(FILE *out, const char *text, size_t length) {
    for (size_t at = 0; at < length;) {
        uint32_t codepoint;
        size_t size = vega_decode(text, length, at, &codepoint);
        if (vega_visible(codepoint) == codepoint) fwrite(text + at, 1, size, out);
        else {
            uint32_t picture = vega_visible(codepoint);
            fputc(0xE0 | (int)(picture >> 12), out);
            fputc(0x80 | (int)(picture >> 6 & 0x3F), out);
            fputc(0x80 | (int)(picture & 0x3F), out);
        }
        at += size;
    }
}

static void pad(FILE *out, unsigned count) {
    while (count--) fputc(' ', out);
}

static const char *const colors[VEGA_TONES] = {"", "\033[2m", "\033[1;36m", "\033[32m", "\033[33m", "\033[31m"};

static void tone_start(FILE *out, VegaTone tone, bool color) {
    if (color && tone) fputs(colors[tone], out);
}

static void tone_end(FILE *out, VegaTone tone, bool color) {
    if (color && tone) fputs("\033[0m", out);
    else if (!color && tone) fprintf(out, " (%s)", tone_names[tone]);
}

static void print_list(FILE *out, PureApp *app, const VegaPart *part, bool color) {
    char text[VEGA_TEXT];
    size_t length, rows = vega_rows(app, part), focus = vega_focus(app, part);
    for (size_t row = 0; row < rows; ++row) {
        VegaTone tone = vega_tone(app, part, row);
        fputs(row == focus ? "> " : "  ", out);
        tone_start(out, tone, color);
        if (vega_text(app, (uint32_t)part->label, row, text, &length)) put_text(out, text, length);
        if (part->badge >= 0 && vega_text(app, (uint32_t)part->badge, row, text, &length) && length) {
            fputs(" [", out);
            put_text(out, text, length);
            fputc(']', out);
        }
        if (part->detail >= 0 && vega_text(app, (uint32_t)part->detail, row, text, &length) && length) {
            fputs(" - ", out);
            put_text(out, text, length);
        }
        tone_end(out, tone, color);
        fputc('\n', out);
    }
}

static void print_table(FILE *out, PureApp *app, const VegaPart *part, bool color) {
    const PureModule *m = app->module;
    size_t rows = vega_rows(app, part), focus = vega_focus(app, part);
    unsigned widths[PURE_MODULE_VIEWS];
    char text[VEGA_TEXT];
    size_t length;
    for (unsigned c = 0; c < part->column_count; ++c) {
        const char *name = m->view_names[part->columns[c]];
        widths[c] = vega_text_width(name, strlen(name));
        for (size_t row = 0; row < rows; ++row)
            if (vega_text(app, part->columns[c], row, text, &length)) {
                unsigned width = vega_text_width(text, length);
                if (width > widths[c]) widths[c] = width;
            }
    }
    fputs("  ", out);
    for (unsigned c = 0; c < part->column_count; ++c) {
        const char *name = m->view_names[part->columns[c]];
        fputs(name, out);
        if (c + 1 < part->column_count) pad(out, widths[c] - (unsigned)strlen(name) + 2);
    }
    fputc('\n', out);
    for (size_t row = 0; row < rows; ++row) {
        VegaTone tone = vega_tone(app, part, row);
        fputs(row == focus ? "> " : "  ", out);
        tone_start(out, tone, color);
        for (unsigned c = 0; c < part->column_count; ++c) {
            bool ok = vega_text(app, part->columns[c], row, text, &length);
            if (!ok) { memcpy(text, "-", 2); length = 1; }
            put_text(out, text, length);
            if (c + 1 < part->column_count) pad(out, widths[c] - vega_text_width(text, length) + 2);
        }
        tone_end(out, tone, color);
        fputc('\n', out);
    }
}

void vega_print(FILE *out, PureApp *app, const VegaScreen *screen, bool color) {
    const PureModule *m = app->module;
    char text[VEGA_TEXT];
    size_t length;
    if (screen->title >= 0 && vega_text(app, (uint32_t)screen->title, 0, text, &length)) {
        if (color) fputs("\033[1m", out);
        put_text(out, text, length);
        if (color) fputs("\033[0m", out);
        fputc('\n', out);
    }
    for (unsigned i = 0; i < screen->count; ++i) {
        const VegaPart *part = &screen->parts[i];
        const char *name = m->view_names[part->view];
        if (part->kind == VEGA_FIELD) {
            fprintf(out, "%s: ", name);
            if (vega_text(app, part->view, 0, text, &length)) put_text(out, text, length);
            else fputs("-", out);
            fputc('\n', out);
            continue;
        }
        fprintf(out, "%s (%zu)\n", name, vega_rows(app, part));
        if (part->kind == VEGA_LIST) print_list(out, app, part, color);
        else print_table(out, app, part, color);
    }
}

/* ---- NUI ------------------------------------------------------------ */

/* Spoken text: control characters become spaces, and trailing ones go. */
static void speak(FILE *out, const char *text, size_t length) {
    while (length && ((unsigned char)text[length - 1] <= 0x20)) --length;
    for (size_t i = 0; i < length; ++i) fputc((unsigned char)text[i] < 0x20 ? ' ' : text[i], out);
}

/* "Item 2 of 3: milk, done." Tones other than normal and muted are said. */
static void say_focus(FILE *out, PureApp *app, const VegaPart *part) {
    const PureModule *m = app->module;
    char text[VEGA_TEXT];
    size_t length, rows = vega_rows(app, part);
    if (!rows) { fprintf(out, "%s is empty.\n", m->view_names[part->view]); return; }
    size_t focus = vega_focus(app, part);
    fprintf(out, "Item %zu of %zu: ", focus + 1, rows);
    VegaTone tone = vega_tone(app, part, focus);
    if (tone >= VEGA_ACCENT) fprintf(out, "%s, ", tone_names[tone]);
    if (part->kind == VEGA_LIST) {
        if (vega_text(app, (uint32_t)part->label, focus, text, &length)) speak(out, text, length);
        int more[] = {part->badge, part->detail};
        for (unsigned k = 0; k < 2; ++k)
            if (more[k] >= 0 && vega_text(app, (uint32_t)more[k], focus, text, &length) && length) {
                fputs(", ", out);
                speak(out, text, length);
            }
    } else {
        for (unsigned c = 0; c < part->column_count; ++c) {
            fprintf(out, "%s%s ", c ? ", " : "", m->view_names[part->columns[c]]);
            if (vega_text(app, part->columns[c], focus, text, &length)) speak(out, text, length);
            else fputs("none", out);
        }
    }
    fputs(".\n", out);
}

void vega_say(FILE *out, PureApp *app, const VegaScreen *screen, bool all) {
    const PureModule *m = app->module;
    char text[VEGA_TEXT];
    size_t length;
    if (!all) {
        for (unsigned i = 0; i < screen->count; ++i)
            if (screen->parts[i].kind != VEGA_FIELD) { say_focus(out, app, &screen->parts[i]); return; }
        return;
    }
    if (screen->title >= 0 && vega_text(app, (uint32_t)screen->title, 0, text, &length)) {
        speak(out, text, length);
        fputs(".\n", out);
    }
    for (unsigned i = 0; i < screen->count; ++i) {
        const VegaPart *part = &screen->parts[i];
        const char *name = m->view_names[part->view];
        if (part->kind == VEGA_FIELD) {
            fprintf(out, "%s: ", name);
            if (vega_text(app, part->view, 0, text, &length)) speak(out, text, length);
            else fputs("none", out);
            fputs(".\n", out);
            continue;
        }
        size_t rows = vega_rows(app, part);
        fprintf(out, "%s, %zu item%s.\n", name, rows, rows == 1 ? "" : "s");
        if (rows) say_focus(out, app, part);
    }
}

/* ---- TUI ------------------------------------------------------------ */

bool vega_grid(VegaGrid *grid, unsigned width, unsigned height) {
    size_t count = (size_t)width * height;
    VegaCell *cells = calloc(count ? count : 1, sizeof(*cells)), *shown = calloc(count ? count : 1, sizeof(*shown));
    struct VegaRow *rows = calloc(height ? height : 1, sizeof(*rows));
    if (!cells || !shown || !rows) { free(cells); free(shown); free(rows); return false; }
    for (unsigned r = 0; r < height; ++r) rows[r].part = -1;
    free(grid->cells);
    free(grid->shown);
    free(grid->rows);
    grid->cells = cells;
    grid->shown = shown;
    grid->rows = rows;
    grid->painted = false;
    grid->width = width;
    grid->height = height;
    return true;
}

void vega_grid_free(VegaGrid *grid) {
    free(grid->cells);
    free(grid->shown);
    free(grid->rows);
    *grid = (VegaGrid){0};
}

typedef struct { VegaGrid *grid; unsigned row, column, end; uint8_t tone; bool focus, strong; } Pen;

/* Write text from the pen's column up to its end; a wide character that
 * does not fit is left out, and a mark joins the character before it. */
static void write_text(Pen *pen, const char *text, size_t length) {
    VegaCell *line = pen->grid->cells + (size_t)pen->row * pen->grid->width;
    for (size_t at = 0; at < length;) {
        uint32_t codepoint;
        at += vega_decode(text, length, at, &codepoint);
        codepoint = vega_visible(codepoint);
        unsigned width = vega_width(codepoint);
        if (!width) {
            if (pen->column && !line[pen->column - 1].mark) {
                unsigned base = pen->column - 1;
                while (base && !line[base].ch) --base;
                line[base].mark = codepoint;
            }
            continue;
        }
        if (pen->column + width > pen->end) { pen->column = pen->end; return; }
        line[pen->column] = (VegaCell){codepoint, 0, pen->tone, pen->focus, pen->strong};
        if (width == 2) line[pen->column + 1] = (VegaCell){0, 0, pen->tone, pen->focus, pen->strong};
        pen->column += width;
    }
}

static void write_cstr(Pen *pen, const char *text) { write_text(pen, text, strlen(text)); }

/* Fill the rest of the pen's span with spaces in its style (a focused row
 * is highlighted across). */
static void fill(Pen *pen) {
    VegaCell *line = pen->grid->cells + (size_t)pen->row * pen->grid->width;
    while (pen->column < pen->end) line[pen->column++] = (VegaCell){' ', 0, pen->tone, pen->focus, pen->strong};
}

/* Text cut to width with an ellipsis. */
static void write_fit(Pen *pen, const char *text, size_t length, unsigned width) {
    unsigned end = pen->end;
    if (pen->column + width < end) pen->end = pen->column + width;
    if (vega_text_width(text, length) > pen->end - pen->column && pen->end > pen->column) {
        --pen->end;
        write_text(pen, text, length);
        pen->end = end;
        write_cstr(pen, "\xe2\x80\xa6");
    } else {
        write_text(pen, text, length);
        pen->end = end;
    }
}

static const char *key_symbol(const char *key) {
    static const struct { const char *key, *symbol; } symbols[] = {
        {"ArrowUp", "\xe2\x86\x91"}, {"ArrowDown", "\xe2\x86\x93"}, {"ArrowLeft", "\xe2\x86\x90"},
        {"ArrowRight", "\xe2\x86\x92"}, {"Space", "Space"}, {"Enter", "Enter"}, {"Backspace", "Bksp"},
        {"Delete", "Del"}, {"Escape", "Esc"}, {"PageUp", "PgUp"}, {"PageDown", "PgDn"}};
    for (size_t i = 0; i < sizeof(symbols) / sizeof(*symbols); ++i)
        if (!strcmp(symbols[i].key, key)) return symbols[i].symbol;
    if (!strncmp(key, "Key", 3)) return key + 3;
    if (!strncmp(key, "Digit", 5)) return key + 5;
    return key;
}

static void draw_list(VegaGrid *grid, PureApp *app, const VegaPart *part, int index, size_t first, unsigned top,
                      unsigned height) {
    char text[VEGA_TEXT];
    size_t length, rows = vega_rows(app, part), focus = vega_focus(app, part);
    for (unsigned r = 0; r < height && first + r < rows; ++r) {
        size_t row = first + r;
        grid->rows[top + r] = (struct VegaRow){index, row};
        bool focused = row == focus;
        VegaTone tone = vega_tone(app, part, row);
        Pen pen = {grid, top + r, 0, grid->width, (uint8_t)tone, focused, false};
        write_cstr(&pen, focused ? "> " : "  ");
        char badge[VEGA_TEXT];
        size_t badge_length = 0;
        if (part->badge >= 0 && !vega_text(app, (uint32_t)part->badge, row, badge, &badge_length)) badge_length = 0;
        unsigned badge_width = badge_length ? vega_text_width(badge, badge_length) + 2 : 0;
        unsigned room = grid->width > pen.column + badge_width ? grid->width - pen.column - badge_width : 0;
        if (vega_text(app, (uint32_t)part->label, row, text, &length)) write_fit(&pen, text, length, room);
        if (part->detail >= 0 && vega_text(app, (uint32_t)part->detail, row, text, &length) && length) {
            unsigned used = pen.column;
            uint8_t saved = pen.tone;
            pen.tone = pen.tone ? pen.tone : VEGA_MUTED;
            write_cstr(&pen, "  ");
            unsigned left = grid->width > badge_width + pen.column ? grid->width - badge_width - pen.column : 0;
            if (pen.column - used == 2) write_fit(&pen, text, length, left);
            pen.tone = saved;
        }
        if (badge_length && grid->width >= badge_width) {
            /* The badge ends at the last column, two cells after the label at least. */
            unsigned start = grid->width - (badge_width - 2);
            if (pen.column < start) fill(&(Pen){grid, pen.row, pen.column, start, pen.tone, focused, false});
            pen.column = start > pen.column ? start : pen.column;
            write_fit(&pen, badge, badge_length, badge_width - 2);
        }
        fill(&pen);
    }
}

static void draw_table(VegaGrid *grid, PureApp *app, const VegaPart *part, int index, size_t first, unsigned top,
                       unsigned height) {
    const PureModule *m = app->module;
    char text[VEGA_TEXT];
    size_t length, rows = vega_rows(app, part), focus = vega_focus(app, part);
    unsigned visible = height ? height - 1 : 0, widths[PURE_MODULE_VIEWS], total = 2;
    /* Columns as wide as their header and the rows on screen, shared out when too wide. */
    for (unsigned c = 0; c < part->column_count; ++c) {
        const char *name = m->view_names[part->columns[c]];
        widths[c] = vega_text_width(name, strlen(name));
        for (unsigned r = 0; r < visible && first + r < rows; ++r)
            if (vega_text(app, part->columns[c], first + r, text, &length)) {
                unsigned width = vega_text_width(text, length);
                if (width > widths[c]) widths[c] = width;
            }
        total += widths[c] + 2;
    }
    if (part->column_count && total > grid->width) {
        unsigned share = grid->width > 2 + 2 * part->column_count ? (grid->width - 2) / part->column_count - 2 : 1;
        for (unsigned c = 0; c < part->column_count; ++c) if (widths[c] > share) widths[c] = share;
    }
    if (!height) return;
    Pen head = {grid, top, 2, grid->width, VEGA_MUTED, false, true};
    for (unsigned c = 0; c < part->column_count; ++c) {
        const char *name = m->view_names[part->columns[c]];
        unsigned start = head.column;
        write_fit(&head, name, strlen(name), widths[c]);
        head.column = start + widths[c] + 2 < grid->width ? start + widths[c] + 2 : grid->width;
    }
    for (unsigned r = 0; r < visible && first + r < rows; ++r) {
        size_t row = first + r;
        bool focused = row == focus;
        grid->rows[top + 1 + r] = (struct VegaRow){index, row};
        Pen pen = {grid, top + 1 + r, 0, grid->width, (uint8_t)vega_tone(app, part, row), focused, false};
        write_cstr(&pen, focused ? "> " : "  ");
        for (unsigned c = 0; c < part->column_count; ++c) {
            unsigned start = pen.column, end = start + widths[c] + 2 < grid->width ? start + widths[c] + 2 : grid->width;
            if (vega_text(app, part->columns[c], row, text, &length)) write_fit(&pen, text, length, widths[c]);
            else write_cstr(&pen, "-");
            if (pen.column < end) fill(&(Pen){grid, pen.row, pen.column, end, pen.tone, focused, false});
            pen.column = end;
        }
        fill(&pen);
    }
}

bool vega_hit(const VegaGrid *grid, unsigned line, unsigned *part, size_t *item) {
    if (line >= grid->height || grid->rows[line].part < 0) return false;
    *part = (unsigned)grid->rows[line].part;
    *item = grid->rows[line].item;
    return true;
}

void vega_draw(VegaGrid *grid, PureApp *app, const VegaScreen *screen, const char *prompt, size_t prompt_length,
               bool typing) {
    const PureModule *m = app->module;
    for (size_t i = 0; i < (size_t)grid->width * grid->height; ++i) grid->cells[i] = (VegaCell){' ', 0, 0, false, false};
    for (unsigned r = 0; r < grid->height; ++r) grid->rows[r].part = -1;
    grid->cursor_row = grid->cursor_column = -1;
    if (!grid->width || !grid->height) return;
    char text[VEGA_TEXT];
    size_t length;
    unsigned row = 0, bottom = grid->height - 1 - (screen->prompt >= 0 && grid->height > 1);
    if (screen->title >= 0 && row < bottom && vega_text(app, (uint32_t)screen->title, 0, text, &length)) {
        Pen pen = {grid, row++, 0, grid->width, VEGA_NORMAL, false, true};
        write_fit(&pen, text, length, grid->width);
    }
    unsigned groups = 0;
    for (unsigned i = 0; i < screen->count; ++i) {
        const VegaPart *part = &screen->parts[i];
        if (part->kind != VEGA_FIELD) { ++groups; continue; }
        if (row >= bottom) continue;
        Pen pen = {grid, row++, 0, grid->width, VEGA_MUTED, false, false};
        write_cstr(&pen, m->view_names[part->view]);
        write_cstr(&pen, "  ");
        pen.tone = VEGA_NORMAL;
        if (vega_text(app, part->view, 0, text, &length)) write_fit(&pen, text, length, grid->width);
        else { pen.tone = VEGA_MUTED; write_cstr(&pen, "-"); }
    }
    /* Lists and tables share the rows left; each keeps its focused row in view
     * and only its visible rows are evaluated. */
    unsigned left = bottom > row ? bottom - row : 0;
    for (unsigned i = 0, group = 0; i < screen->count; ++i) {
        const VegaPart *part = &screen->parts[i];
        if (part->kind == VEGA_FIELD) continue;
        unsigned share = left / groups + (group < left % groups);
        ++group;
        if (!share) continue;
        size_t rows = vega_rows(app, part), focus = vega_focus(app, part);
        Pen head = {grid, row, 0, grid->width, VEGA_NORMAL, false, true};
        write_cstr(&head, m->view_names[part->view]);
        char count[32];
        snprintf(count, sizeof(count), " (%zu)", rows);
        head.strong = false;
        head.tone = VEGA_MUTED;
        write_cstr(&head, count);
        unsigned area = share - 1, visible = part->kind == VEGA_TABLE && area ? area - 1 : area;
        size_t *first = &grid->first[i];
        if (focus < *first) *first = focus;
        if (visible && focus >= *first + visible) *first = focus - visible + 1;
        if (*first + visible > rows) *first = rows > visible ? rows - visible : 0;
        if (part->kind == VEGA_LIST) draw_list(grid, app, part, (int)i, *first, row + 1, area);
        else draw_table(grid, app, part, (int)i, *first, row + 1, area);
        row += share;
    }
    /* Keys the app answers to, then the text being typed. */
    Pen help = {grid, bottom, 0, grid->width, VEGA_MUTED, false, false};
    if (screen->prompt >= 0) write_cstr(&help, typing ? "Tab keys  " : "Tab type  ");
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].kind != PURE_KIND_KEY) continue;
        bool bound = false;
        for (uint32_t h = 0; h < m->handlers && !bound; ++h) bound = m->handler[h].event == 1 + 2 * i;
        if (!bound) continue;
        write_cstr(&help, key_symbol(m->input_keys[i]));
        write_cstr(&help, " ");
        write_cstr(&help, m->input_names[i]);
        write_cstr(&help, "  ");
    }
    if (screen->prompt >= 0 && grid->height > 1) {
        Pen pen = {grid, grid->height - 1, 0, grid->width, typing ? VEGA_ACCENT : VEGA_MUTED, false, false};
        write_cstr(&pen, m->input_names[screen->prompt]);
        write_cstr(&pen, "> ");
        pen.tone = typing ? VEGA_NORMAL : VEGA_MUTED;
        /* Show the end of a long entry. */
        size_t start = 0;
        unsigned room = grid->width > pen.column + 1 ? grid->width - pen.column - 1 : 0;
        while (start < prompt_length && vega_text_width(prompt + start, prompt_length - start) > room) {
            uint32_t skip;
            start += vega_decode(prompt, prompt_length, start, &skip);
        }
        write_text(&pen, prompt + start, prompt_length - start);
        if (typing) { grid->cursor_row = (int)pen.row; grid->cursor_column = (int)pen.column; }
    }
}
