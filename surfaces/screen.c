#define _POSIX_C_SOURCE 200809L
#include "vega.h"
#include "module-text.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

double vega_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return floor((double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6);
}

double vega_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return floor((double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6);
}

void vega_timers_start(VegaTimers *timers, const PureModule *m, double clock) {
    if (timers->count < m->inputs) {
        double *grown = realloc(timers->next, m->inputs * sizeof(*grown));
        if (grown) timers->next = grown, timers->count = m->inputs; /* else its timers past count never tick */
    }
    for (uint32_t i = 0; i < m->inputs && i < timers->count; ++i)
        timers->next[i] = m->input[i].binding == PURE_BIND_TIMER ? clock + m->input[i].period : 0;
}

long vega_timers_run(VegaTimers *timers, PureApp *app, double clock, bool *ticked) {
    const PureModule *m = app->module;
    double soonest = -1;
    for (uint32_t i = 0; i < m->inputs && i < timers->count; ++i) {
        if (m->input[i].binding != PURE_BIND_TIMER) continue;
        if (timers->next[i] <= clock) {
            app->now = vega_now();
            pure_app_message(app, i, (PureSpan){0}); /* a failed tick changes nothing, as any failed event */
            *ticked = true;
            timers->next[i] = clock + m->input[i].period;
        }
        if (soonest < 0 || timers->next[i] - clock < soonest) soonest = timers->next[i] - clock;
    }
    return soonest < 0 ? -1 : (long)ceil(soonest);
}

static unsigned long evaluations;

unsigned long vega_evaluations(void) { return evaluations; }

static int column(const PureModule *m, uint32_t table, const char *name, uint8_t type) {
    int view = pure_module_column(m, table, name);
    return view >= 0 && m->view_type[view] == type ? view : -1;
}

void vega_screen(VegaScreen *screen, const PureModule *m) {
    memset(screen, 0, sizeof(*screen));
    screen->module = m;
    screen->title = pure_module_view(m, "title");
    if (screen->title >= 0 && m->view_type[screen->title] != PURE_VIEW_TEXT) screen->title = -1;
    screen->prompt = screen->frame = -1;
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].binding == PURE_BIND_TEXT && screen->prompt < 0) screen->prompt = (int)i;
        if (m->input[i].binding == PURE_BIND_FRAME) screen->frame = (int)i;
    }
    /* A <table>_focus view belongs to its table, not to the fields. */
    bool *focus_view = calloc(m->views + 1, sizeof(*focus_view)); /* (without room, none is) */
    for (uint32_t i = 0; i < m->views; ++i) {
        if (m->view_type[i] != PURE_VIEW_LIST || m->view_table[i]) continue;
        char name[PURE_MODULE_NAME + 8];
        snprintf(name, sizeof(name), "%s_focus", m->view_names[i]);
        int focus = pure_module_view(m, name);
        if (focus >= 0 && m->view_type[focus] == PURE_VIEW_NUM && focus_view) focus_view[focus] = true;
    }
    for (uint32_t i = 0; i < m->views && screen->count < VEGA_PARTS; ++i) {
        if (m->view_table[i] || (int)i == screen->title || (focus_view && focus_view[i])) continue;
        VegaPart *part = &screen->parts[screen->count++];
        *part = (VegaPart){.kind = VEGA_FIELD, .view = i, .focus = -1, .pointer = -1, .label = -1, .tone = -1, .detail = -1,
                           .badge = -1};
        if (m->view_type[i] != PURE_VIEW_LIST) continue;
        char name[PURE_MODULE_NAME + 8];
        snprintf(name, sizeof(name), "%s_focus", m->view_names[i]);
        part->focus = pure_module_view(m, name);
        if (part->focus >= 0 && !(focus_view && focus_view[part->focus])) part->focus = -1;
        for (uint32_t k = 0; part->focus >= 0 && k < m->inputs; ++k)
            if (m->input[k].binding == PURE_BIND_POINT && m->input[k].view == i) part->pointer = (int)k;
        part->label = column(m, i, "label", PURE_VIEW_TEXT);
        part->tone = column(m, i, "tone", PURE_VIEW_NUM);
        part->kind = part->label >= 0 ? VEGA_LIST : VEGA_TABLE;
        if (part->kind == VEGA_LIST) {
            part->detail = column(m, i, "detail", PURE_VIEW_TEXT);
            part->badge = column(m, i, "badge", PURE_VIEW_TEXT);
        }
        for (uint32_t k = 0; k < m->views; ++k)
            if (m->view_table[k] == i + 1 && (int)k != part->tone && part->column_count < VEGA_COLUMNS)
                part->columns[part->column_count++] = k;
    }
    free(focus_view);
}

static bool is_view(const PureModule *m, int view, uint8_t type) {
    return view >= 0 && m->view_type[view] == type;
}

unsigned vega_check(const PureModule *m, void (*report)(void *, const char *name, const char *message), void *context) {
    unsigned found = 0;
    char name[2 * PURE_MODULE_NAME + 8], message[256];
#define FIND(...) do { snprintf(message, sizeof(message), __VA_ARGS__); report(context, name, message); ++found; } while (0)
    int title = pure_module_view(m, "title");
    snprintf(name, sizeof(name), "title");
    if (title >= 0 && m->view_type[title] != PURE_VIEW_TEXT) FIND("a title is text; this one shows as a field");
    for (uint32_t i = 0; i < m->views; ++i) {
        if (m->view_table[i]) continue;
        const char *view = m->view_names[i];
        size_t length = strlen(view);
        snprintf(name, sizeof(name), "%s", view);
        if (length > 6 && !strcmp(view + length - 6, "_focus")) {
            char table[PURE_MODULE_NAME];
            snprintf(table, sizeof(table), "%.*s", (int)(length - 6), view);
            int owner = pure_module_view(m, table);
            if (!is_view(m, owner, PURE_VIEW_LIST))
                FIND("names no list or table %s; it shows as a field", table);
            else if (m->view_type[i] != PURE_VIEW_NUM) FIND("the focused row is a number; %s shows no focus", table);
        }
        if (m->view_type[i] != PURE_VIEW_LIST) continue;
        int label = pure_module_column(m, i, "label"), tone = pure_module_column(m, i, "tone");
        bool list = is_view(m, label, PURE_VIEW_TEXT);
        unsigned shown = 0;
        for (uint32_t k = 0; k < m->views; ++k) {
            if (m->view_table[k] != i + 1) continue;
            const char *column = m->view_names[k];
            bool text = m->view_type[k] == PURE_VIEW_TEXT;
            snprintf(name, sizeof(name), "%s.%s", view, column);
            if ((int)k == label && !text) FIND("a list's label is text; %s shows as a table", view);
            else if ((int)k == tone && text) FIND("a tone is a number (vega.normal ... vega.danger); %s", list ?
                                                  "the list ignores it" : "it shows as a column");
            else if (list && (!strcmp(column, "badge") || !strcmp(column, "detail")) && !text)
                FIND("a list's %s is text; this one is not shown", column);
            else if (list && (int)k != label && (int)k != tone && strcmp(column, "badge") && strcmp(column, "detail"))
                FIND("a list shows label, badge, detail and tone; this column is not shown");
            shown += (int)k != tone || text;
        }
        snprintf(name, sizeof(name), "%s", view);
        if (!list && !shown) FIND("a table with no column to show");
    }
    int prompt = -1;
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].binding != PURE_BIND_TEXT) continue;
        snprintf(name, sizeof(name), "%s", m->input_names[i]);
        if (prompt >= 0) FIND("only the first text input (%s) has a place to type into", m->input_names[prompt]);
        else prompt = (int)i;
    }
#undef FIND
    return found;
}

typedef struct { char *out; size_t used; } Collect;

/* Bytes, as many as their hex digits fit in a value's text. */
static bool collect_bytes(void *context, PureSpan span) {
    Collect *c = context;
    size_t room = (VEGA_TEXT - 1) / 2 - c->used, take = span.size < room ? span.size : room;
    if (take) memcpy(c->out + c->used, span.data, take);
    c->used += take;
    return true;
}

static bool collect(void *context, PureSpan span) {
    Collect *c = context;
    size_t room = VEGA_TEXT - 1 - c->used;
    size_t take = span.size < room ? span.size : room;
    /* Never cut a character: back off to a UTF-8 boundary. */
    if (take < span.size) while (take && (span.data[take] & 0xC0) == 0x80) --take;
    if (take) memcpy(c->out + c->used, span.data, take);
    c->used += take;
    return true;
}

bool vega_text(PureApp *app, uint32_t view, size_t row, char out[VEGA_TEXT], size_t *length) {
    const PureModule *m = app->module;
    bool column = m->view_table[view] != 0;
    ++evaluations;
    *length = 0;
    out[0] = 0;
    if (m->view_type[view] == PURE_VIEW_I64) { /* every digit */
        int64_t value;
        if (!(column ? pure_app_cell_i64(app, view, row, &value) : pure_app_view_i64(app, view, &value))) return false;
        *length = (size_t)snprintf(out, VEGA_TEXT, "%" PRId64, value);
        return true;
    }
    if (m->view_type[view] == PURE_VIEW_LIST) { /* a list cell (027's 4e): how many items it holds */
        PureSpan items;
        PureCell cells[PURE_RECORD_FIELDS];
        size_t count = 0, at = 0;
        if (!column || pure_app_cell_items(app, view, row, &items) != PURE_OK) return false;
        while (pure_items_next(m, m->view_record[view] - 1u, items, &at, cells)) ++count;
        *length = (size_t)snprintf(out, VEGA_TEXT, "%zu item%s", count, count == 1 ? "" : "s");
        return true;
    }
    if (m->view_type[view] == PURE_VIEW_BYTES) { /* as hex digits, as Pure's tools write bytes */
        char bytes[VEGA_TEXT / 2];
        Collect c = {bytes, 0};
        PureResult result = column ? pure_app_cell_text(app, view, row, collect_bytes, &c)
                                   : pure_app_view_text(app, view, collect_bytes, &c);
        for (size_t i = 0; i < c.used; ++i) snprintf(out + 2 * i, 3, "%02x", (unsigned char)bytes[i]);
        *length = 2 * c.used;
        out[*length] = 0;
        return result == PURE_OK;
    }
    if (m->view_type[view] == PURE_VIEW_NUM || m->view_type[view] == PURE_VIEW_BOOL) {
        double value;
        if (!(column ? pure_app_cell(app, view, row, &value) : pure_app_view(app, view, &value))) return false;
        if (m->view_type[view] == PURE_VIEW_BOOL) { /* a bool is written true or false, as a string shows it */
            *length = value ? 4 : 5;
            memcpy(out, value ? "true" : "false", *length);
        } else *length = pure_format_number(value, out);
        out[*length] = 0;
        return true;
    }
    Collect c = {out, 0};
    PureResult result = column ? pure_app_cell_text(app, view, row, collect, &c) : pure_app_view_text(app, view, collect, &c);
    out[c.used] = 0;
    *length = c.used;
    return result == PURE_OK;
}

bool vega_number(PureApp *app, uint32_t view, size_t row, double *out) {
    ++evaluations;
    return app->module->view_table[view] ? pure_app_cell(app, view, row, out) : pure_app_view(app, view, out);
}

size_t vega_rows(PureApp *app, const VegaPart *part) {
    return part->kind == VEGA_FIELD ? 0 : pure_app_rows(app, part->view);
}

size_t vega_focus(PureApp *app, const VegaPart *part) {
    size_t rows = vega_rows(app, part), row;
    double focus;
    if (part->focus < 0 || !rows || !vega_number(app, (uint32_t)part->focus, 0, &focus) || !(focus >= 0)) return 0;
    bool filtered = app->module->view_code[part->view].length > 0;
    size_t item = focus < (double)SIZE_MAX ? (size_t)focus : SIZE_MAX;
    if (pure_app_item_row(app, part->view, item, &row)) return row;
    return filtered ? VEGA_NO_ROW : rows - 1;
}

size_t vega_scroll(size_t first, size_t focus, size_t visible, size_t rows) {
    if (focus < rows && focus < first) first = focus;
    if (focus < rows && visible && focus >= first + visible) first = focus - visible + 1;
    if (first + visible > rows) first = rows > visible ? rows - visible : 0;
    return first;
}

static int bound(const PureModule *m, uint8_t binding, const char *key) {
    for (uint32_t i = 0; i < m->inputs; ++i)
        if (m->input[i].binding == binding && !strcmp(m->input_keys[i], key)) return (int)i;
    return -1;
}

int vega_key_input(const PureModule *m, const char *key) { return bound(m, PURE_BIND_PRESS, key); }

bool vega_press(PureApp *app, const char *key) {
    int input = vega_key_input(app->module, key), up = bound(app->module, PURE_BIND_RELEASE, key);
    if (input < 0) return false;
    bool ok = pure_app_message(app, (uint32_t)input, (PureSpan){0}) == PURE_OK;
    if (up >= 0) pure_app_message(app, (uint32_t)up, (PureSpan){0});
    return ok;
}

bool vega_commit(PureApp *app, const VegaScreen *screen, const char *text, size_t length) {
    return screen->prompt >= 0 &&
           pure_app_message(app, (uint32_t)screen->prompt, (PureSpan){(const unsigned char *)text, length}) == PURE_OK;
}

bool vega_point(PureApp *app, const VegaPart *part, size_t row) {
    size_t item; /* a filtered list's row shows some item of the list */
    if (part->pointer < 0 || !pure_app_row_item(app, part->view, row, &item) || item > UINT32_MAX) return false;
    unsigned char position[4] = {(unsigned char)item, (unsigned char)(item >> 8), (unsigned char)(item >> 16),
                                 (unsigned char)(item >> 24)};
    return pure_app_message(app, (uint32_t)part->pointer, (PureSpan){position, 4}) == PURE_OK;
}

bool vega_frame(PureApp *app, const VegaScreen *screen) {
    static const double dt = 1.0 / 60.0;
    if (screen->frame < 0) return false;
    bool deferred = app->deferred;
    pure_app_defer(app, true); /* a frame's changes are stored by the next event or a save, as before (027's 4d) */
    bool ok = pure_app_message(app, (uint32_t)screen->frame, (PureSpan){(const unsigned char *)&dt, sizeof(dt)}) == PURE_OK;
    pure_app_defer(app, deferred);
    return ok;
}

VegaTone vega_tone(PureApp *app, const VegaPart *part, size_t row) {
    double tone;
    if (part->tone < 0 || !vega_number(app, (uint32_t)part->tone, row, &tone)) return VEGA_NORMAL;
    return tone >= 0 && tone < VEGA_TONES && tone == floor(tone) ? (VegaTone)tone : VEGA_NORMAL;
}
