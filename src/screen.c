#include "vega.h"
#include "module-text.h"
#include <math.h>
#include <string.h>

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
    screen->prompt = -1;
    for (uint32_t i = 0; i < m->inputs && screen->prompt < 0; ++i)
        if (m->input[i].kind == PURE_KIND_TEXT) screen->prompt = (int)i;
    /* A <table>_focus view belongs to its table, not to the fields. */
    bool focus_view[PURE_MODULE_VIEWS] = {false};
    for (uint32_t i = 0; i < m->views; ++i) {
        if (m->view_type[i] != PURE_VIEW_TABLE) continue;
        char name[PURE_MODULE_NAME + 8];
        snprintf(name, sizeof(name), "%s_focus", m->view_names[i]);
        int focus = pure_module_view(m, name);
        if (focus >= 0 && m->view_type[focus] == PURE_VIEW_NUM) focus_view[focus] = true;
    }
    for (uint32_t i = 0; i < m->views && screen->count < VEGA_PARTS; ++i) {
        if (m->view_table[i] || (int)i == screen->title || focus_view[i]) continue;
        VegaPart *part = &screen->parts[screen->count++];
        *part = (VegaPart){.kind = VEGA_FIELD, .view = i, .focus = -1, .label = -1, .tone = -1, .detail = -1, .badge = -1};
        if (m->view_type[i] != PURE_VIEW_TABLE) continue;
        char name[PURE_MODULE_NAME + 8];
        snprintf(name, sizeof(name), "%s_focus", m->view_names[i]);
        part->focus = pure_module_view(m, name);
        if (part->focus >= 0 && !focus_view[part->focus]) part->focus = -1;
        part->label = column(m, i, "label", PURE_VIEW_TEXT);
        part->tone = column(m, i, "tone", PURE_VIEW_NUM);
        part->kind = part->label >= 0 ? VEGA_LIST : VEGA_TABLE;
        if (part->kind == VEGA_LIST) {
            part->detail = column(m, i, "detail", PURE_VIEW_TEXT);
            part->badge = column(m, i, "badge", PURE_VIEW_TEXT);
        }
        for (uint32_t k = 0; k < m->views; ++k)
            if (m->view_table[k] == i + 1 && (int)k != part->tone) part->columns[part->column_count++] = k;
    }
}

typedef struct { char *out; size_t used; } Collect;

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
    if (m->view_type[view] == PURE_VIEW_NUM) {
        double value;
        if (!(column ? pure_app_cell(app, view, row, &value) : pure_app_view(app, view, &value))) return false;
        *length = pure_format_number(value, out);
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

size_t vega_rows(const PureApp *app, const VegaPart *part) {
    return part->kind == VEGA_FIELD ? 0 : pure_app_rows(app, part->view);
}

size_t vega_focus(PureApp *app, const VegaPart *part) {
    size_t rows = vega_rows(app, part);
    double focus;
    if (part->focus < 0 || !rows || !vega_number(app, (uint32_t)part->focus, 0, &focus) || !(focus >= 0)) return 0;
    return focus >= (double)(rows - 1) ? rows - 1 : (size_t)focus;
}

VegaTone vega_tone(PureApp *app, const VegaPart *part, size_t row) {
    double tone;
    if (part->tone < 0 || !vega_number(app, (uint32_t)part->tone, row, &tone)) return VEGA_NORMAL;
    return tone >= 0 && tone < VEGA_TONES && tone == floor(tone) ? (VegaTone)tone : VEGA_NORMAL;
}
