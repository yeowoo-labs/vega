/* Accessibility on Linux desktops: the screen as AT-SPI2 objects on the
 * accessibility bus (surfaces/vega.h), for screen readers such as Orca,
 * spoken over Vega's own D-Bus (dbus.c), no toolkit or library.
 *
 * The objects follow the screen as the say surface does:
 *   application (the root) > frame (the title) > a heading, a label per
 *   field ("name: value"), a list box per list (a list item per row, the
 *   focused one selected, and focused while the list is the one keys move),
 *   a table per table (rows of cells), the text entry, and a tool bar of
 *   the keys as push buttons.
 * Each answer is worked out from the app when it is asked for, so nothing
 * is kept that could go stale. Assistive technology can also act: a key's
 * button presses it (Action), a row selected is focused (Selection). When
 * the app changes, the focus that moved is said as events. */
#define _POSIX_C_SOURCE 200809L
#include "dbus.h"
#include "vega.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOT "/org/a11y/atspi/accessible/root"
#define NULL_PATH "/org/a11y/atspi/null"
#define ACCESSIBLE "org.a11y.atspi.Accessible"

/* AT-SPI's roles (atspi-constants.h) and states (bit positions). */
enum { ROLE_FRAME = 23, ROLE_LABEL = 29, ROLE_LIST_ITEM = 32, ROLE_PUSH_BUTTON = 43, ROLE_TABLE = 55,
       ROLE_TABLE_CELL = 56, ROLE_TOOL_BAR = 63, ROLE_APPLICATION = 75, ROLE_ENTRY = 79, ROLE_HEADING = 83,
       ROLE_TABLE_ROW = 90, ROLE_LIST_BOX = 98 };
enum { STATE_ACTIVE = 1, STATE_EDITABLE = 7, STATE_ENABLED = 8, STATE_FOCUSABLE = 11, STATE_FOCUSED = 12,
       STATE_SELECTABLE = 22, STATE_SELECTED = 23, STATE_SENSITIVE = 24, STATE_SHOWING = 25, STATE_SINGLE_LINE = 26,
       STATE_VISIBLE = 30 };

struct VegaAccess {
    VegaBus bus;
    PureApp *app;
    const VegaScreen *screen;
    const VegaAccessHost *host;
    char parent_name[128], parent_path[128]; /* the registry's desktop, which embeds us */
    int32_t id;                              /* the registry's number for us */
    char focused[128];                       /* the object last said to have the focus */
};

/* ---- What an object is: its path taken apart */

typedef enum { ROOT_OBJECT, FRAME, TITLE, FIELD, PART, ROW, CELL, ENTRY, KEYS, KEY, NONE } Kind;
typedef struct {
    Kind kind;
    unsigned part, column, key; /* part: an index in screen->parts; key: an input */
    size_t row;
} Object;

static Object parse_path(const VegaAccess *a, const char *path) {
    Object o = {NONE, 0, 0, 0, 0};
    unsigned part, column, key;
    size_t row;
    int used = 0;
    if (!strcmp(path, ROOT)) o.kind = ROOT_OBJECT;
    else if (!strcmp(path, "/org/vega/frame")) o.kind = FRAME;
    else if (!strcmp(path, "/org/vega/title")) o.kind = TITLE;
    else if (!strcmp(path, "/org/vega/entry")) o.kind = ENTRY;
    else if (!strcmp(path, "/org/vega/keys")) o.kind = KEYS;
    else if (sscanf(path, "/org/vega/key/%u%n", &key, &used) == 1 && !path[used]) o = (Object){KEY, 0, 0, key, 0};
    else if (sscanf(path, "/org/vega/field/%u%n", &part, &used) == 1 && !path[used]) o = (Object){FIELD, part, 0, 0, 0};
    else if (sscanf(path, "/org/vega/part/%u/%zu/%u%n", &part, &row, &column, &used) == 3 && !path[used])
        o = (Object){CELL, part, column, 0, row};
    else if (sscanf(path, "/org/vega/part/%u/%zu%n", &part, &row, &used) == 2 && !path[used]) o = (Object){ROW, part, 0, 0, row};
    else if (sscanf(path, "/org/vega/part/%u%n", &part, &used) == 1 && !path[used]) o = (Object){PART, part, 0, 0, 0};
    /* what no longer exists is nothing */
    const VegaScreen *s = a->screen;
    if ((o.kind == FIELD || o.kind == PART || o.kind == ROW || o.kind == CELL) &&
        (o.part >= s->count || (o.kind == FIELD) != (s->parts[o.part].kind == VEGA_FIELD)))
        o.kind = NONE;
    if ((o.kind == ROW || o.kind == CELL) && o.row >= vega_rows(a->app, &s->parts[o.part])) o.kind = NONE;
    if (o.kind == CELL && (s->parts[o.part].kind != VEGA_TABLE || o.column >= s->parts[o.part].column_count)) o.kind = NONE;
    if (o.kind == ROW && s->parts[o.part].kind == VEGA_FIELD) o.kind = NONE;
    if (o.kind == TITLE && s->title < 0) o.kind = NONE;
    if (o.kind == ENTRY && s->prompt < 0) o.kind = NONE;
    if (o.kind == KEY && (o.key >= a->app->module->inputs || a->app->module->input[o.key].binding != PURE_BIND_PRESS)) o.kind = NONE;
    return o;
}

static void path_of(const Object *o, char *out, size_t size) {
    switch (o->kind) {
    case ROOT_OBJECT: snprintf(out, size, ROOT); break;
    case FRAME: snprintf(out, size, "/org/vega/frame"); break;
    case TITLE: snprintf(out, size, "/org/vega/title"); break;
    case FIELD: snprintf(out, size, "/org/vega/field/%u", o->part); break;
    case PART: snprintf(out, size, "/org/vega/part/%u", o->part); break;
    case ROW: snprintf(out, size, "/org/vega/part/%u/%zu", o->part, o->row); break;
    case CELL: snprintf(out, size, "/org/vega/part/%u/%zu/%u", o->part, o->row, o->column); break;
    case ENTRY: snprintf(out, size, "/org/vega/entry"); break;
    case KEYS: snprintf(out, size, "/org/vega/keys"); break;
    case KEY: snprintf(out, size, "/org/vega/key/%u", o->key); break;
    default: snprintf(out, size, NULL_PATH); break;
    }
}

/* The keys the screen answers: key inputs a handler takes. */
static bool bound_key(const PureModule *m, uint32_t input) {
    if (m->input[input].binding != PURE_BIND_PRESS) return false;
    for (uint32_t h = 0; h < m->handlers; ++h)
        if (m->handler[h].event == input) return true;
    return false;
}

/* The list or table keys move: the first with rows. */
static int active_part(const VegaAccess *a) {
    for (unsigned i = 0; i < a->screen->count; ++i)
        if (a->screen->parts[i].kind != VEGA_FIELD && vega_rows(a->app, &a->screen->parts[i])) return (int)i;
    return -1;
}

/* An object's children, in order, into out (up to max); how many there are. */
static size_t children(const VegaAccess *a, const Object *o, Object *out, size_t max) {
    const VegaScreen *s = a->screen;
    const PureModule *m = a->app->module;
    size_t n = 0;
#define CHILD(...)                                  \
    do {                                            \
        if (n < max) out[n] = (Object){__VA_ARGS__}; \
        ++n;                                        \
    } while (0)
    switch (o->kind) {
    case ROOT_OBJECT: CHILD(FRAME, 0, 0, 0, 0); break;
    case FRAME:
        if (s->title >= 0) CHILD(TITLE, 0, 0, 0, 0);
        for (unsigned i = 0; i < s->count; ++i) CHILD(s->parts[i].kind == VEGA_FIELD ? FIELD : PART, i, 0, 0, 0);
        if (s->prompt >= 0) CHILD(ENTRY, 0, 0, 0, 0);
        for (uint32_t i = 0; i < m->inputs; ++i)
            if (bound_key(m, i)) {
                CHILD(KEYS, 0, 0, 0, 0);
                break;
            }
        break;
    case PART:
        for (size_t row = 0, rows = vega_rows(a->app, &s->parts[o->part]); row < rows; ++row) CHILD(ROW, o->part, 0, 0, row);
        break;
    case ROW:
        if (s->parts[o->part].kind == VEGA_TABLE)
            for (unsigned c = 0; c < s->parts[o->part].column_count; ++c) CHILD(CELL, o->part, c, 0, o->row);
        break;
    case KEYS:
        for (uint32_t i = 0; i < m->inputs; ++i)
            if (bound_key(m, i)) CHILD(KEY, 0, 0, i, 0);
        break;
    default: break;
    }
#undef CHILD
    return n;
}

static Object parent_of(const Object *o) {
    switch (o->kind) {
    case FRAME: return (Object){ROOT_OBJECT, 0, 0, 0, 0};
    case TITLE: case FIELD: case PART: case ENTRY: case KEYS: return (Object){FRAME, 0, 0, 0, 0};
    case ROW: return (Object){PART, o->part, 0, 0, 0};
    case CELL: return (Object){ROW, o->part, 0, 0, o->row};
    case KEY: return (Object){KEYS, 0, 0, 0, 0};
    default: return (Object){NONE, 0, 0, 0, 0};
    }
}

static size_t index_in_parent(const VegaAccess *a, const Object *o) {
    Object parent = parent_of(o);
    if (o->kind == ROW) return o->row;
    if (o->kind == CELL) return o->column;
    size_t n = children(a, &parent, NULL, 0), at = 0;
    Object *kids = malloc((n + 1) * sizeof(*kids));
    if (kids) n = children(a, &parent, kids, n);
    for (size_t i = 0; kids && i < n; ++i)
        if (kids[i].kind == o->kind && kids[i].part == o->part && kids[i].key == o->key) { at = i; break; }
    free(kids);
    return at;
}

static int role_of(const VegaAccess *a, const Object *o) {
    switch (o->kind) {
    case ROOT_OBJECT: return ROLE_APPLICATION;
    case FRAME: return ROLE_FRAME;
    case TITLE: return ROLE_HEADING;
    case FIELD: return ROLE_LABEL;
    case PART: return a->screen->parts[o->part].kind == VEGA_LIST ? ROLE_LIST_BOX : ROLE_TABLE;
    case ROW: return a->screen->parts[o->part].kind == VEGA_LIST ? ROLE_LIST_ITEM : ROLE_TABLE_ROW;
    case CELL: return ROLE_TABLE_CELL;
    case ENTRY: return ROLE_ENTRY;
    case KEYS: return ROLE_TOOL_BAR;
    case KEY: return ROLE_PUSH_BUTTON;
    default: return 0;
    }
}

static const char *role_name(int role) {
    switch (role) {
    case ROLE_APPLICATION: return "application";
    case ROLE_FRAME: return "frame";
    case ROLE_HEADING: return "heading";
    case ROLE_LABEL: return "label";
    case ROLE_LIST_BOX: return "list box";
    case ROLE_LIST_ITEM: return "list item";
    case ROLE_TABLE: return "table";
    case ROLE_TABLE_ROW: return "table row";
    case ROLE_TABLE_CELL: return "table cell";
    case ROLE_ENTRY: return "entry";
    case ROLE_TOOL_BAR: return "tool bar";
    case ROLE_PUSH_BUTTON: return "push button";
    default: return "unknown";
    }
}

/* ---- Names: what the say surface says */

typedef struct { char text[4 * VEGA_TEXT]; size_t length; } Name;

static void add(Name *n, const char *text, size_t length) {
    for (size_t i = 0; i < length && n->length + 1 < sizeof(n->text); ++i)
        n->text[n->length++] = (unsigned char)text[i] < 0x20 ? ' ' : text[i]; /* control characters say nothing */
    n->text[n->length] = 0;
}
static void add_string(Name *n, const char *text) { add(n, text, strlen(text)); }
static void add_view(Name *n, PureApp *app, uint32_t view, size_t row) {
    char text[VEGA_TEXT];
    size_t length;
    if (vega_text(app, view, row, text, &length)) {
        while (length && (unsigned char)text[length - 1] <= 0x20) --length;
        add(n, text, length);
    }
}

static const char *const tone_words[VEGA_TONES] = {NULL, NULL, "accent", "good", "warning", "danger"};

static void name_of(const VegaAccess *a, const Object *o, Name *n) {
    const VegaScreen *s = a->screen;
    const PureModule *m = a->app->module;
    n->length = 0;
    n->text[0] = 0;
    switch (o->kind) {
    case ROOT_OBJECT:
    case FRAME:
    case TITLE:
        if (s->title >= 0) add_view(n, a->app, (uint32_t)s->title, 0);
        if (!n->length && o->kind != TITLE) add_string(n, "Vega");
        break;
    case FIELD:
        add_string(n, m->view_names[s->parts[o->part].view]);
        add_string(n, ": ");
        add_view(n, a->app, s->parts[o->part].view, 0);
        break;
    case PART: add_string(n, m->view_names[s->parts[o->part].view]); break;
    case ROW: {
        const VegaPart *part = &s->parts[o->part];
        if (part->kind == VEGA_LIST) {
            if (part->label >= 0) add_view(n, a->app, (uint32_t)part->label, o->row);
            int more[] = {part->badge, part->detail};
            for (unsigned k = 0; k < 2; ++k) {
                if (more[k] < 0) continue;
                Name extra = {{0}, 0};
                add_view(&extra, a->app, (uint32_t)more[k], o->row);
                if (extra.length) {
                    add_string(n, ", ");
                    add(n, extra.text, extra.length);
                }
            }
        } else
            for (unsigned c = 0; c < part->column_count; ++c) {
                if (c) add_string(n, ", ");
                add_string(n, m->view_names[part->columns[c]]);
                add_string(n, " ");
                add_view(n, a->app, part->columns[c], o->row);
            }
        const char *tone = tone_words[vega_tone(a->app, part, o->row)];
        if (tone) {
            add_string(n, ", ");
            add_string(n, tone);
        }
        break;
    }
    case CELL: add_view(n, a->app, s->parts[o->part].columns[o->column], o->row); break;
    case ENTRY: add_string(n, m->input_names[s->prompt]); break;
    case KEYS: add_string(n, "Keys"); break;
    case KEY:
        add_string(n, m->input_names[o->key]);
        add_string(n, " (");
        add_string(n, m->input_keys[o->key]);
        add_string(n, ")");
        break;
    default: break;
    }
}

/* ---- States */

static bool focused_row(const VegaAccess *a, const Object *o) {
    const VegaPart *part = &a->screen->parts[o->part];
    return vega_focus(a->app, part) == o->row;
}

static Object focus_of(const VegaAccess *a);

static bool same_object(const Object *x, const Object *y) {
    return x->kind == y->kind && x->part == y->part && x->row == y->row && x->column == y->column && x->key == y->key;
}

static void state_of(const VegaAccess *a, const Object *o, uint32_t bits[2]) {
    bits[0] = bits[1] = 0;
#define SET(state) (bits[(state) / 32] |= 1u << ((state) % 32))
    if (o->kind == NONE) return;
    SET(STATE_ENABLED);
    SET(STATE_SENSITIVE);
    SET(STATE_SHOWING);
    SET(STATE_VISIBLE);
    /* One object has the focus (focus_of); it alone is focused. */
    Object focus = focus_of(a);
    if (same_object(o, &focus)) SET(STATE_FOCUSED);
    switch (o->kind) {
    case FRAME: SET(STATE_ACTIVE); break;
    case PART: SET(STATE_FOCUSABLE); break;
    case ROW:
        SET(STATE_SELECTABLE);
        SET(STATE_FOCUSABLE);
        if (focused_row(a, o)) SET(STATE_SELECTED);
        break;
    case ENTRY:
        SET(STATE_EDITABLE);
        SET(STATE_FOCUSABLE);
        SET(STATE_SINGLE_LINE);
        break;
    case KEY: SET(STATE_FOCUSABLE); break;
    default: break;
    }
#undef SET
}

/* The object that has the focus: the entry while typing, else the focused
 * row of the list keys move, else that list, else the frame. */
static Object focus_of(const VegaAccess *a) {
    if (a->host->typing && a->host->typing(a->host->context) && a->screen->prompt >= 0) return (Object){ENTRY, 0, 0, 0, 0};
    int active = active_part(a);
    if (active < 0) return (Object){FRAME, 0, 0, 0, 0};
    const VegaPart *part = &a->screen->parts[active];
    size_t focus = vega_focus(a->app, part);
    if (focus < vega_rows(a->app, part)) return (Object){ROW, (unsigned)active, 0, 0, focus};
    return (Object){PART, (unsigned)active, 0, 0, 0};
}

static void interfaces_of(const VegaAccess *a, const Object *o, const char *out[6], size_t *n) {
    *n = 0;
    out[(*n)++] = ACCESSIBLE;
    if (o->kind == ROOT_OBJECT) out[(*n)++] = "org.a11y.atspi.Application";
    if (o->kind == PART) out[(*n)++] = "org.a11y.atspi.Selection";
    if (o->kind == KEY || (o->kind == ROW && a->screen->parts[o->part].pointer >= 0)) out[(*n)++] = "org.a11y.atspi.Action";
    if (o->kind == ENTRY) out[(*n)++] = "org.a11y.atspi.Text";
}

/* ---- Answers */

static void put_reference(VegaDbusBody *b, const char *name, const char *path) {
    vega_dbus_put_struct(b);
    vega_dbus_put_string(b, name);
    vega_dbus_put_string(b, path);
}

static void put_object(VegaAccess *a, VegaDbusBody *b, const Object *o) {
    char path[128];
    path_of(o, path, sizeof(path));
    put_reference(b, o->kind == NONE ? "" : a->bus.name, path);
}

static void put_parent(VegaAccess *a, VegaDbusBody *b, const Object *o) {
    if (o->kind == ROOT_OBJECT) put_reference(b, a->parent_name, a->parent_path[0] ? a->parent_path : NULL_PATH);
    else {
        Object parent = parent_of(o);
        put_object(a, b, &parent);
    }
}

static void reply(VegaAccess *a, const VegaDbusMessage *m, const char *signature, VegaDbusBody *b) {
    if (!(m->flags & VEGA_DBUS_NO_REPLY))
        vega_bus_send(&a->bus, VEGA_DBUS_RETURN, VEGA_DBUS_NO_REPLY, m->serial, m->sender, NULL, NULL, NULL, NULL,
                      signature, b, NULL);
    vega_dbus_body_free(b);
}

static void refuse(VegaAccess *a, const VegaDbusMessage *m, const char *error, const char *why) {
    VegaDbusBody b = {0};
    vega_dbus_put_string(&b, why);
    if (!(m->flags & VEGA_DBUS_NO_REPLY))
        vega_bus_send(&a->bus, VEGA_DBUS_ERROR, VEGA_DBUS_NO_REPLY, m->serial, m->sender, NULL, NULL, NULL, error, "s", &b,
                      NULL);
    vega_dbus_body_free(&b);
}

/* A property as a variant (its signature, then its value); false for one
 * the object does not have. */
static bool put_property(VegaAccess *a, VegaDbusBody *b, const Object *o, const char *interface, const char *name) {
    Name text;
    if (!strcmp(interface, ACCESSIBLE)) {
        if (!strcmp(name, "Name")) {
            name_of(a, o, &text);
            vega_dbus_put_signature(b, "s");
            vega_dbus_put_string(b, text.text);
        } else if (!strcmp(name, "Description") || !strcmp(name, "HelpText") || !strcmp(name, "AccessibleId")) {
            vega_dbus_put_signature(b, "s");
            vega_dbus_put_string(b, "");
        } else if (!strcmp(name, "Locale")) {
            vega_dbus_put_signature(b, "s");
            vega_dbus_put_string(b, "");
        } else if (!strcmp(name, "Parent")) {
            vega_dbus_put_signature(b, "(so)");
            put_parent(a, b, o);
        } else if (!strcmp(name, "ChildCount")) {
            vega_dbus_put_signature(b, "i");
            vega_dbus_put_i32(b, (int32_t)children(a, o, NULL, 0));
        } else
            return false;
        return true;
    }
    if (!strcmp(interface, "org.a11y.atspi.Application") && o->kind == ROOT_OBJECT) {
        const char *value = !strcmp(name, "ToolkitName") ? "Vega" : !strcmp(name, "Version") ? "1"
                          : !strcmp(name, "AtspiVersion") ? "2.1" : NULL;
        if (value) {
            vega_dbus_put_signature(b, "s");
            vega_dbus_put_string(b, value);
            return true;
        }
        if (!strcmp(name, "Id")) {
            vega_dbus_put_signature(b, "i");
            vega_dbus_put_i32(b, a->id);
            return true;
        }
        return false;
    }
    if (!strcmp(interface, "org.a11y.atspi.Selection") && o->kind == PART && !strcmp(name, "NSelectedChildren")) {
        const VegaPart *part = &a->screen->parts[o->part];
        vega_dbus_put_signature(b, "i");
        vega_dbus_put_i32(b, vega_focus(a->app, part) < vega_rows(a->app, part));
        return true;
    }
    if (!strcmp(interface, "org.a11y.atspi.Action") && !strcmp(name, "NActions")) {
        vega_dbus_put_signature(b, "i");
        vega_dbus_put_i32(b, 1);
        return true;
    }
    if (!strcmp(interface, "org.a11y.atspi.Text") && o->kind == ENTRY) {
        size_t length = 0;
        const char *typed = a->host->typed ? a->host->typed(a->host->context, &length) : "";
        int32_t characters = 0;
        for (size_t i = 0; i < length; ++i) characters += ((unsigned char)typed[i] & 0xC0) != 0x80;
        if (!strcmp(name, "CharacterCount") || !strcmp(name, "CaretOffset")) {
            vega_dbus_put_signature(b, "i");
            vega_dbus_put_i32(b, characters);
            return true;
        }
    }
    return false;
}

/* What was said: the characters of typed from start to end (-1: the end). */
static void put_text_range(VegaDbusBody *b, const char *typed, size_t length, int32_t start, int32_t end) {
    size_t from = length, to = length;
    int32_t character = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i == length || ((unsigned char)typed[i] & 0xC0) != 0x80) {
            if (character == start) from = i;
            if (character == end) to = i;
            ++character;
        }
    }
    if (start <= 0) from = 0;
    if (end < 0 || to < from) to = end < 0 ? length : from;
    char out[4 * VEGA_TEXT];
    size_t n = to - from < sizeof(out) - 1 ? to - from : sizeof(out) - 1;
    memcpy(out, typed + from, n);
    out[n] = 0;
    vega_dbus_put_string(b, out);
}

static void answer(VegaAccess *a, const VegaDbusMessage *m) {
    const char *interface = m->interface ? m->interface : "", *member = m->member ? m->member : "";
    VegaDbusReader r;
    vega_dbus_reader(&r, m);
    if (!strcmp(interface, "org.freedesktop.DBus.Peer") && !strcmp(member, "Ping")) {
        VegaDbusBody b = {0};
        reply(a, m, NULL, &b);
        return;
    }
    Object o = parse_path(a, m->path ? m->path : "");
    if (!strcmp(interface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
        VegaDbusBody b = {0};
        vega_dbus_put_string(&b, "<node/>");
        reply(a, m, "s", &b);
        return;
    }
    if (o.kind == NONE) {
        refuse(a, m, "org.freedesktop.DBus.Error.UnknownObject", "no such object");
        return;
    }
    VegaDbusBody b = {0};
    if (!strcmp(interface, "org.freedesktop.DBus.Properties")) {
        if (!strcmp(member, "Get")) {
            const char *on = vega_dbus_get_string(&r), *name = vega_dbus_get_string(&r);
            if (r.failed || !put_property(a, &b, &o, on, name)) {
                vega_dbus_body_free(&b);
                refuse(a, m, "org.freedesktop.DBus.Error.UnknownProperty", "no such property");
                return;
            }
            reply(a, m, "v", &b);
            return;
        }
        if (!strcmp(member, "GetAll")) {
            const char *on = vega_dbus_get_string(&r);
            static const char *const names[] = {"Name", "Description", "Parent", "ChildCount", "Locale", "AccessibleId",
                                                "HelpText", "ToolkitName", "Version", "AtspiVersion", "Id",
                                                "NSelectedChildren", "NActions", "CharacterCount", "CaretOffset"};
            size_t array = vega_dbus_put_array(&b, 8);
            for (size_t i = 0; !r.failed && i < sizeof(names) / sizeof(*names); ++i) {
                size_t before = b.used;
                vega_dbus_put_struct(&b);
                vega_dbus_put_string(&b, names[i]);
                if (!put_property(a, &b, &o, on, names[i])) b.used = before;
            }
            vega_dbus_end_array(&b, array, 8);
            reply(a, m, "a{sv}", &b);
            return;
        }
        if (!strcmp(member, "Set")) {
            /* the registry numbers us: Application.Id, an int in a variant */
            const char *on = vega_dbus_get_string(&r), *name = vega_dbus_get_string(&r), *type = vega_dbus_get_signature(&r);
            if (!r.failed && !strcmp(on, "org.a11y.atspi.Application") && !strcmp(name, "Id") && !strcmp(type, "i"))
                a->id = vega_dbus_get_i32(&r);
            reply(a, m, NULL, &b);
            return;
        }
    }
    if (!strcmp(interface, ACCESSIBLE)) {
        if (!strcmp(member, "GetRole")) {
            vega_dbus_put_u32(&b, (uint32_t)role_of(a, &o));
            reply(a, m, "u", &b);
        } else if (!strcmp(member, "GetRoleName") || !strcmp(member, "GetLocalizedRoleName")) {
            vega_dbus_put_string(&b, role_name(role_of(a, &o)));
            reply(a, m, "s", &b);
        } else if (!strcmp(member, "GetChildren")) {
            size_t count = children(a, &o, NULL, 0);
            Object *kids = malloc((count + 1) * sizeof(*kids));
            size_t array = vega_dbus_put_array(&b, 8);
            if (kids) {
                children(a, &o, kids, count);
                for (size_t i = 0; i < count; ++i) put_object(a, &b, &kids[i]);
            }
            vega_dbus_end_array(&b, array, 8);
            free(kids);
            reply(a, m, "a(so)", &b);
        } else if (!strcmp(member, "GetChildAtIndex")) {
            int32_t index = vega_dbus_get_i32(&r);
            size_t count = children(a, &o, NULL, 0);
            Object kid = {NONE, 0, 0, 0, 0};
            if (index >= 0 && (size_t)index < count) {
                Object *kids = malloc(count * sizeof(*kids));
                if (kids) {
                    children(a, &o, kids, count);
                    kid = kids[index];
                }
                free(kids);
            }
            put_object(a, &b, &kid);
            reply(a, m, "(so)", &b);
        } else if (!strcmp(member, "GetIndexInParent")) {
            vega_dbus_put_i32(&b, o.kind == ROOT_OBJECT ? -1 : (int32_t)index_in_parent(a, &o));
            reply(a, m, "i", &b);
        } else if (!strcmp(member, "GetState")) {
            uint32_t bits[2];
            state_of(a, &o, bits);
            size_t array = vega_dbus_put_array(&b, 4);
            vega_dbus_put_u32(&b, bits[0]);
            vega_dbus_put_u32(&b, bits[1]);
            vega_dbus_end_array(&b, array, 4);
            reply(a, m, "au", &b);
        } else if (!strcmp(member, "GetAttributes")) {
            size_t array = vega_dbus_put_array(&b, 8);
            vega_dbus_put_struct(&b);
            vega_dbus_put_string(&b, "toolkit");
            vega_dbus_put_string(&b, "Vega");
            if (o.kind == TITLE) {
                vega_dbus_put_struct(&b);
                vega_dbus_put_string(&b, "level");
                vega_dbus_put_string(&b, "1");
            }
            if (o.kind == ROW) {
                char text[32];
                snprintf(text, sizeof(text), "%zu", o.row + 1);
                vega_dbus_put_struct(&b);
                vega_dbus_put_string(&b, "posinset");
                vega_dbus_put_string(&b, text);
                snprintf(text, sizeof(text), "%zu", vega_rows(a->app, &a->screen->parts[o.part]));
                vega_dbus_put_struct(&b);
                vega_dbus_put_string(&b, "setsize");
                vega_dbus_put_string(&b, text);
            }
            vega_dbus_end_array(&b, array, 8);
            reply(a, m, "a{ss}", &b);
        } else if (!strcmp(member, "GetRelationSet")) {
            size_t array = vega_dbus_put_array(&b, 8);
            vega_dbus_end_array(&b, array, 8);
            reply(a, m, "a(ua(so))", &b);
        } else if (!strcmp(member, "GetApplication")) {
            Object root = {ROOT_OBJECT, 0, 0, 0, 0};
            put_object(a, &b, &root);
            reply(a, m, "(so)", &b);
        } else if (!strcmp(member, "GetInterfaces")) {
            const char *names[6];
            size_t n;
            interfaces_of(a, &o, names, &n);
            size_t array = vega_dbus_put_array(&b, 4);
            for (size_t i = 0; i < n; ++i) vega_dbus_put_string(&b, names[i]);
            vega_dbus_end_array(&b, array, 4);
            reply(a, m, "as", &b);
        } else {
            vega_dbus_body_free(&b);
            refuse(a, m, "org.freedesktop.DBus.Error.UnknownMethod", "not a method Vega answers");
        }
        return;
    }
    if (!strcmp(interface, "org.a11y.atspi.Application")) {
        if (!strcmp(member, "GetLocale")) vega_dbus_put_string(&b, "");
        reply(a, m, !strcmp(member, "GetLocale") ? "s" : NULL, &b);
        return;
    }
    if (!strcmp(interface, "org.a11y.atspi.Selection") && o.kind == PART) {
        const VegaPart *part = &a->screen->parts[o.part];
        size_t rows = vega_rows(a->app, part), focus = vega_focus(a->app, part);
        int32_t index = !strcmp(member, "SelectAll") || !strcmp(member, "ClearSelection") ? 0 : vega_dbus_get_i32(&r);
        if (!strcmp(member, "GetSelectedChild")) {
            Object kid = index == 0 && focus < rows ? (Object){ROW, o.part, 0, 0, focus} : (Object){NONE, 0, 0, 0, 0};
            put_object(a, &b, &kid);
            reply(a, m, "(so)", &b);
        } else if (!strcmp(member, "SelectChild")) {
            /* selecting a row focuses it, as a click on it does */
            bool done = index >= 0 && (size_t)index < rows && vega_point(a->app, part, (size_t)index);
            if (done && a->host->changed) a->host->changed(a->host->context);
            vega_dbus_put_bool(&b, done);
            reply(a, m, "b", &b);
        } else if (!strcmp(member, "IsChildSelected")) {
            vega_dbus_put_bool(&b, index >= 0 && (size_t)index == focus);
            reply(a, m, "b", &b);
        } else {
            vega_dbus_put_bool(&b, false); /* one row is always the selection: it cannot be cleared or grown */
            reply(a, m, "b", &b);
        }
        return;
    }
    if (!strcmp(interface, "org.a11y.atspi.Action") && (o.kind == KEY || o.kind == ROW)) {
        const char *what = o.kind == KEY ? "press" : "focus";
        if (!strcmp(member, "DoAction")) {
            bool done = false;
            if (o.kind == KEY) done = a->host->press && a->host->press(a->host->context, a->app->module->input_keys[o.key]);
            else done = vega_point(a->app, &a->screen->parts[o.part], o.row);
            if (done && a->host->changed) a->host->changed(a->host->context);
            vega_dbus_put_bool(&b, done);
            reply(a, m, "b", &b);
        } else if (!strcmp(member, "GetActions")) {
            size_t array = vega_dbus_put_array(&b, 8);
            vega_dbus_put_struct(&b);
            vega_dbus_put_string(&b, what);
            vega_dbus_put_string(&b, "");
            vega_dbus_put_string(&b, o.kind == KEY ? a->app->module->input_keys[o.key] : "");
            vega_dbus_end_array(&b, array, 8);
            reply(a, m, "a(sss)", &b);
        } else if (!strcmp(member, "GetName") || !strcmp(member, "GetLocalizedName")) {
            vega_dbus_put_string(&b, what);
            reply(a, m, "s", &b);
        } else if (!strcmp(member, "GetDescription")) {
            vega_dbus_put_string(&b, "");
            reply(a, m, "s", &b);
        } else if (!strcmp(member, "GetKeyBinding")) {
            vega_dbus_put_string(&b, o.kind == KEY ? a->app->module->input_keys[o.key] : "");
            reply(a, m, "s", &b);
        } else {
            vega_dbus_body_free(&b);
            refuse(a, m, "org.freedesktop.DBus.Error.UnknownMethod", "not a method Vega answers");
        }
        return;
    }
    if (!strcmp(interface, "org.a11y.atspi.Text") && o.kind == ENTRY && !strcmp(member, "GetText")) {
        int32_t start = vega_dbus_get_i32(&r), end = vega_dbus_get_i32(&r);
        size_t length = 0;
        const char *typed = a->host->typed ? a->host->typed(a->host->context, &length) : "";
        put_text_range(&b, typed, length, start, end);
        reply(a, m, "s", &b);
        return;
    }
    vega_dbus_body_free(&b);
    refuse(a, m, "org.freedesktop.DBus.Error.UnknownMethod", "not a method Vega answers");
}

static void handle(void *context, const VegaDbusMessage *m) {
    VegaAccess *a = context;
    if (m->type == VEGA_DBUS_CALL) answer(a, m);
}

/* ---- Events */

static void event(VegaAccess *a, const Object *o, const char *member, const char *detail, int32_t detail1) {
    char path[128];
    path_of(o, path, sizeof(path));
    VegaDbusBody b = {0};
    vega_dbus_put_string(&b, detail);
    vega_dbus_put_i32(&b, detail1);
    vega_dbus_put_i32(&b, 0);
    vega_dbus_put_signature(&b, "i");
    vega_dbus_put_i32(&b, 0);
    size_t array = vega_dbus_put_array(&b, 8);
    vega_dbus_end_array(&b, array, 8);
    vega_bus_send(&a->bus, VEGA_DBUS_SIGNAL, VEGA_DBUS_NO_REPLY, 0, NULL, path, "org.a11y.atspi.Event.Object", member,
                  NULL, "siiva{sv}", &b, NULL);
    vega_dbus_body_free(&b);
}

void vega_access_changed(VegaAccess *a) {
    Object now = focus_of(a);
    char path[128];
    path_of(&now, path, sizeof(path));
    if (!strcmp(path, a->focused)) return;
    Object before = parse_path(a, a->focused);
    if (before.kind != NONE) {
        event(a, &before, "StateChanged", "focused", 0);
        if (before.kind == ROW) event(a, &before, "StateChanged", "selected", 0);
    }
    if (now.kind == ROW) {
        event(a, &now, "StateChanged", "selected", 1);
        Object list = parent_of(&now);
        event(a, &list, "SelectionChanged", "", 0);
    }
    event(a, &now, "StateChanged", "focused", 1);
    snprintf(a->focused, sizeof(a->focused), "%s", path);
}

/* ---- The connection */

VegaAccess *vega_access_open(PureApp *app, const VegaScreen *screen, const VegaAccessHost *host, char *error,
                             size_t error_size) {
    const char *session = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!session || !*session) {
        snprintf(error, error_size, "no session bus");
        return NULL;
    }
    VegaAccess *a = calloc(1, sizeof(*a));
    if (!a) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    a->app = app;
    a->screen = screen;
    a->host = host;
    a->bus.fd = -1;
    /* The accessibility bus is its own: the session bus says where. */
    VegaBus bus;
    VegaDbusMessage m;
    if (!vega_bus_open(&bus, session, error, error_size)) {
        free(a);
        return NULL;
    }
    char address[512] = "";
    if (vega_bus_call(&bus, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", NULL, NULL, &m, NULL, NULL) &&
        m.type == VEGA_DBUS_RETURN) {
        VegaDbusReader r;
        vega_dbus_reader(&r, &m);
        snprintf(address, sizeof(address), "%s", vega_dbus_get_string(&r));
    }
    vega_bus_close(&bus);
    if (!address[0]) {
        snprintf(error, error_size, "no accessibility bus");
        free(a);
        return NULL;
    }
    if (!vega_bus_open(&a->bus, address, error, error_size)) {
        free(a);
        return NULL;
    }
    /* Be embedded in the desktop: the registry answers with its root, our
     * parent. It may ask us things first; they are answered meanwhile. */
    VegaDbusBody b = {0};
    put_reference(&b, a->bus.name, ROOT);
    bool embedded = vega_bus_call(&a->bus, "org.a11y.atspi.Registry", ROOT, "org.a11y.atspi.Socket", "Embed", "(so)", &b, &m,
                                  handle, a) &&
                    m.type == VEGA_DBUS_RETURN;
    vega_dbus_body_free(&b);
    if (!embedded) {
        snprintf(error, error_size, "the accessibility registry did not embed us");
        vega_access_close(a);
        return NULL;
    }
    VegaDbusReader r;
    vega_dbus_reader(&r, &m);
    vega_dbus_get_struct(&r);
    snprintf(a->parent_name, sizeof(a->parent_name), "%s", vega_dbus_get_string(&r));
    snprintf(a->parent_path, sizeof(a->parent_path), "%s", vega_dbus_get_string(&r));
    vega_access_changed(a);
    return a;
}

int vega_access_fd(const VegaAccess *a) { return a->bus.fd; }

bool vega_access_read(VegaAccess *a) { return vega_bus_read(&a->bus, handle, a); }

void vega_access_close(VegaAccess *a) {
    if (!a) return;
    vega_bus_close(&a->bus);
    free(a);
}
