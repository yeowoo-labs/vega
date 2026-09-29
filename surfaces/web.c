#define _GNU_SOURCE
#include "vega.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* vega web: the screen as a web page, a GUI surface. The page asks for the
 * screen as JSON with the rows it can show per list or table, so only the
 * cells on the page are evaluated; it sends keys (KeyboardEvent.code names are
 * Pure's), committed text and clicks on rows, and an event stream tells every
 * open page when the app changed.
 *
 *   GET  /                       the page
 *   GET  /screen?rows=R&first=F,G  title, fields, each list or table with R rows
 *                                from its last first row (F, G, ... by part), keys
 *   POST /key    KEYNAME         press and release the input bound to the key
 *   POST /text   UTF-8           commit text to the screen's text input
 *   POST /point  PART ROW        focus a row of a list or table (0 first)
 *   GET  /events                 server-sent events: one per change */

enum { CLIENTS = 64, REQUEST = 1 << 16, ROWS_MAX = 500 };

typedef struct { char *bytes; size_t used, capacity; } Buffer;

typedef struct {
    int fd;
    bool events; /* an open event stream */
    bool closing;
    char in[REQUEST];
    size_t in_used;
    Buffer out;
    size_t out_at;
} Client;

static const char *const tone_names[VEGA_TONES] = {"normal", "muted", "accent", "good", "warning", "danger"};
static volatile sig_atomic_t stopping;

static void on_stop(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static void put(Buffer *b, const void *bytes, size_t size) {
    if (b->used + size > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 4096;
        while (capacity < b->used + size) capacity *= 2;
        char *grown = realloc(b->bytes, capacity);
        if (!grown) return;
        b->bytes = grown;
        b->capacity = capacity;
    }
    memcpy(b->bytes + b->used, bytes, size);
    b->used += size;
}

static void puts_(Buffer *b, const char *text) { put(b, text, strlen(text)); }

static void putf(Buffer *b, const char *format, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void putf(Buffer *b, const char *format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0) put(b, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

/* A JSON string: UTF-8 as it is, quotes, backslashes and control characters escaped. */
static void json_string(Buffer *b, const char *text, size_t length) {
    put(b, "\"", 1);
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') { put(b, "\\", 1); put(b, &text[i], 1); }
        else if (c < 0x20 || c == 0x7F) putf(b, "\\u%04x", c);
        else put(b, &text[i], 1);
    }
    put(b, "\"", 1);
}

static void json_name(Buffer *b, const char *name) { json_string(b, name, strlen(name)); }

/* A view's text at a row, or null. */
static void json_view(Buffer *b, PureApp *app, uint32_t view, size_t row) {
    char text[VEGA_TEXT];
    size_t length;
    if (vega_text(app, view, row, text, &length)) json_string(b, text, length);
    else puts_(b, "null");
}

static void screen_json(Buffer *b, PureApp *app, const VegaScreen *screen, size_t visible, size_t *firsts) {
    const PureModule *m = app->module;
    puts_(b, "{\"title\":");
    if (screen->title >= 0) json_view(b, app, (uint32_t)screen->title, 0);
    else puts_(b, "null");
    puts_(b, ",\"fields\":[");
    bool comma = false;
    for (unsigned i = 0; i < screen->count; ++i) {
        const VegaPart *part = &screen->parts[i];
        if (part->kind != VEGA_FIELD) continue;
        puts_(b, comma ? ",{\"name\":" : "{\"name\":");
        json_name(b, m->view_names[part->view]);
        puts_(b, ",\"text\":");
        json_view(b, app, part->view, 0);
        puts_(b, "}");
        comma = true;
    }
    puts_(b, "],\"parts\":[");
    comma = false;
    for (unsigned i = 0; i < screen->count; ++i) {
        const VegaPart *part = &screen->parts[i];
        if (part->kind == VEGA_FIELD) continue;
        size_t rows = vega_rows(app, part), focus = vega_focus(app, part);
        size_t first = vega_scroll(firsts[i], focus, visible, rows);
        firsts[i] = first;
        puts_(b, comma ? ",{\"part\":" : "{\"part\":");
        putf(b, "%u,\"name\":", i);
        json_name(b, m->view_names[part->view]);
        putf(b, ",\"kind\":\"%s\",\"rows\":%zu,\"focus\":", part->kind == VEGA_LIST ? "list" : "table", rows);
        if (focus < rows) putf(b, "%zu", focus);
        else puts_(b, "null");
        putf(b, ",\"first\":%zu,\"pointable\":%s,\"columns\":[", first, part->pointer >= 0 ? "true" : "false");
        if (part->kind == VEGA_TABLE)
            for (unsigned c = 0; c < part->column_count; ++c) {
                if (c) puts_(b, ",");
                json_name(b, m->view_names[part->columns[c]]);
            }
        puts_(b, "],\"items\":[");
        for (size_t row = first; row < first + visible && row < rows; ++row) {
            putf(b, "%s{\"row\":%zu,\"tone\":\"%s\"", row > first ? "," : "", row,
                 tone_names[vega_tone(app, part, row)]);
            if (part->kind == VEGA_LIST) {
                const int columns[] = {part->label, part->badge, part->detail};
                const char *const names[] = {"label", "badge", "detail"};
                for (unsigned k = 0; k < 3; ++k) {
                    if (columns[k] < 0) continue;
                    putf(b, ",\"%s\":", names[k]);
                    json_view(b, app, (uint32_t)columns[k], row);
                }
            } else {
                puts_(b, ",\"cells\":[");
                for (unsigned c = 0; c < part->column_count; ++c) {
                    if (c) puts_(b, ",");
                    json_view(b, app, part->columns[c], row);
                }
                puts_(b, "]");
            }
            puts_(b, "}");
        }
        puts_(b, "]}");
        comma = true;
    }
    puts_(b, "],\"keys\":[");
    comma = false;
    for (uint32_t i = 0; i < m->inputs; ++i) {
        if (m->input[i].kind != PURE_KIND_KEY) continue;
        bool bound = false;
        for (uint32_t h = 0; h < m->handlers && !bound; ++h) bound = m->handler[h].event == 1 + 2 * i;
        if (!bound) continue;
        puts_(b, comma ? ",{\"input\":" : "{\"input\":");
        json_name(b, m->input_names[i]);
        puts_(b, ",\"key\":");
        json_name(b, m->input_keys[i]);
        puts_(b, "}");
        comma = true;
    }
    puts_(b, "],\"prompt\":");
    if (screen->prompt >= 0) json_name(b, m->input_names[screen->prompt]);
    else puts_(b, "null");
    putf(b, ",\"events\":%llu}", (unsigned long long)app->events);
}

/* The page: one file, no dependencies. It keeps each part's first row, asks
 * for as many rows as fit, and redraws when the event stream says so. */
static const char *const page_parts[] = {
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Vega</title><style>"
":root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fbfbfd;--line:#d2d2d7;--focus:#0a58ca;--muted:#86868b;"
"--accent:#0071e3;--good:#1e8e3e;--warning:#b06000;--danger:#d93025}"
"@media(prefers-color-scheme:dark){:root{--fg:#f5f5f7;--bg:#161618;--line:#3a3a3c;--focus:#4c8dff;--muted:#8e8e93;"
"--accent:#64a8ff;--good:#5ec26a;--warning:#f0a33a;--danger:#ff6b5e}}"
"body{margin:0;font:15px/1.5 system-ui,-apple-system,'Noto Sans KR',sans-serif;color:var(--fg);background:var(--bg)}"
"main{max-width:760px;margin:0 auto;padding:24px 16px}h1{font-size:22px;margin:0 0 12px}"
"dl{display:grid;grid-template-columns:max-content 1fr;gap:2px 16px;margin:0 0 16px}dt{color:var(--muted)}dd{margin:0}"
"section{margin:0 0 20px}h2{font-size:14px;font-weight:600;margin:0 0 6px}h2 span{color:var(--muted);font-weight:400}"
"ul{list-style:none;margin:0;padding:0;border:1px solid var(--line);border-radius:8px;overflow:hidden}"
"li,tbody tr{cursor:pointer}li{display:flex;gap:10px;padding:7px 12px;border-top:1px solid var(--line)}"
"li:first-child{border-top:0}.label{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:pre}"
".detail{color:var(--muted);white-space:pre}.badge{font-size:12px;padding:1px 8px;border:1px solid currentColor;"
"border-radius:10px;align-self:center}[aria-selected=true]{background:var(--focus);color:#fff}"
"table{width:100%;border-collapse:collapse;border:1px solid var(--line)}th{text-align:left;color:var(--muted);"
"font-weight:500}th,td{padding:6px 12px;border-top:1px solid var(--line);white-space:pre}"
".muted{color:var(--muted)}.accent{color:var(--accent)}.good{color:var(--good)}.warning{color:var(--warning)}"
".danger{color:var(--danger)}[aria-selected=true].muted,[aria-selected=true] *{color:#fff}"
"form{display:flex;gap:8px;margin-top:16px}input{flex:1;font:inherit;padding:8px 10px;border:1px solid var(--line);"
"border-radius:8px;background:transparent;color:inherit}button{font:inherit;padding:8px 16px;border:1px solid var(--line);"
"border-radius:8px;background:transparent;color:inherit;cursor:pointer}button:hover{border-color:var(--focus)}"
"footer{margin-top:14px;color:var(--muted);font-size:13px}"
"kbd{font:12px ui-monospace,monospace;border:1px solid var(--line);border-radius:4px;padding:0 5px;margin-right:4px}"
"#keys{display:flex;flex-wrap:wrap;gap:6px}#keys button{padding:2px 8px;font-size:13px;color:var(--muted)}"
".sr{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}"
"ul:focus,table:focus{outline:2px solid var(--focus);outline-offset:2px}"
"</style></head><body><main>",
"<h1 id=title></h1><dl id=fields></dl><div id=parts></div>"
"<form id=prompt hidden><input id=entry autocomplete=off><button>Enter</button></form>"
"<footer><div id=keys role=toolbar aria-label=Keys></div></footer></main>"
"<script>",
/* Accessible as the say surface is: a list is a listbox whose options
 * know their place in the whole list (the page shows only the rows that
 * fit), the focused row is the active descendant of a list that keeps the
 * keyboard focus, a table is a grid, a tone other than normal and muted is
 * said, keys are buttons. Everything is updated in place, so the focus a
 * screen reader follows survives a redraw. */
"'use strict';let screen=null,firsts=[],busy=false,again=false,held=null;"
"const rows=Math.max(5,Math.floor((innerHeight-220)/34));const $=id=>document.getElementById(id);"
"const el=(tag,props,...kids)=>{const e=document.createElement(tag);Object.assign(e,props||{});"
"for(const k of kids)if(k!=null)e.append(k);return e};"
"const said={accent:'accent',good:'good',warning:'warning',danger:'danger'};"
"async function post(path,body){await fetch(path,{method:'POST',body});refresh()}"
"async function refresh(){if(busy){again=true;return}busy=true;try{"
"const r=await fetch('/screen?rows='+rows+'&first='+firsts.join(','));screen=await r.json();render()}"
"finally{busy=false;if(again){again=false;refresh()}}}"
"function fit(parent,count,make){while(parent.children.length>count)parent.lastChild.remove();"
"while(parent.children.length<count)parent.append(make());return[...parent.children]}"
"function text(node,value){if(node.textContent!==value)node.textContent=value}"
"function attr(node,name,value){if(value==null)node.removeAttribute(name);"
"else if(node.getAttribute(name)!==String(value))node.setAttribute(name,value)}"
"const views=new Map();"
"function view(p){let v=views.get(p.part);if(v&&v.kind===p.kind)return v;"
"const head=el('h2',{},el('span',{className:'name'}),' ',el('span',{className:'count'}));"
"const box=p.kind==='list'?el('ul',{role:'listbox',tabIndex:0}):el('table',{role:'grid',tabIndex:0},el('thead',{},el('tr')),el('tbody'));"
"box.onfocus=()=>{held=p.part};"
"box.onwheel=e=>{e.preventDefault();const q=views.get(p.part).p;if(q.pointable)"
"post('/point',q.part+' '+Math.max(0,Math.min(q.rows-1,(q.focus?\?-1)+Math.sign(e.deltaY))))};"
"v={kind:p.kind,head,box,section:el('section',{},head,box)};views.set(p.part,v);return v}"
"function row(v,it){const p=v.p;return()=>{if(p.pointable)post('/point',p.part+' '+it.row)}}",
"function render(){const s=screen;text($('title'),s.title?\?'');document.title=s.title?\?'Vega';"
"const pairs=fit($('fields'),2*s.fields.length,()=>el('div'));"
"s.fields.forEach((f,i)=>{if(pairs[2*i].tagName!=='DT'){pairs[2*i].replaceWith(el('dt'));pairs[2*i+1].replaceWith(el('dd'))}});"
"const cells=[...$('fields').children];s.fields.forEach((f,i)=>{text(cells[2*i],f.name);text(cells[2*i+1],f.text?\?'-')});"
"firsts=s.parts.map(p=>p.first);const shown=s.parts.map(p=>{const v=view(p);v.p=p;"
"text(v.head.firstChild,p.name);text(v.head.lastChild,'('+p.rows+')');attr(v.box,'aria-label',p.name);"
"const id=r=>'p'+p.part+'r'+r;attr(v.box,'aria-activedescendant',p.focus==null?null:id(p.focus));"
"if(p.kind==='list'){fit(v.box,p.items.length,()=>el('li',{role:'option'},el('span',{className:'label'}),"
"el('span',{className:'detail'}),el('span',{className:'badge'}),el('span',{className:'sr'}))).forEach((li,k)=>{"
"const it=p.items[k];li.id=id(it.row);li.className=it.tone;li.onclick=row(v,it);"
"attr(li,'aria-selected',it.row===p.focus);attr(li,'aria-setsize',p.rows);attr(li,'aria-posinset',it.row+1);"
"const[label,detail,badge,tone]=li.children;text(label,it.label?\?'');text(detail,it.detail?\?'');text(badge,it.badge?\?'');"
"detail.hidden=!it.detail;badge.hidden=!it.badge;text(tone,said[it.tone]?', '+said[it.tone]:'')})}"
"else{attr(v.box,'aria-rowcount',p.rows+1);const head=v.box.tHead.rows[0];attr(head,'aria-rowindex',1);"
"fit(head,p.columns.length,()=>el('th',{scope:'col'})).forEach((th,c)=>text(th,p.columns[c]));"
"fit(v.box.tBodies[0],p.items.length,()=>el('tr')).forEach((tr,k)=>{const it=p.items[k];tr.id=id(it.row);"
"tr.className=it.tone;tr.onclick=row(v,it);attr(tr,'aria-selected',it.row===p.focus);attr(tr,'aria-rowindex',it.row+2);"
"fit(tr,it.cells.length+1,()=>el('td')).forEach((td,c)=>{if(c<it.cells.length)text(td,it.cells[c]?\?'-');"
"else{td.className='sr';text(td,said[it.tone]?\?'')}})})}"
"return v.section});",
"const current=[...$('parts').children];if(current.length!==shown.length||current.some((c,i)=>c!==shown[i]))"
"$('parts').replaceChildren(...shown);"
/* The keyboard stays with the list it was on, or goes to the first. */
"const keep=views.get(held)?.box;if(document.activeElement===document.body||!document.activeElement)"
"(keep&&keep.isConnected?keep:views.get(s.parts[0]?.part)?.box)?.focus({preventScroll:true});"
"$('prompt').hidden=!s.prompt;if(s.prompt){$('entry').placeholder=s.prompt;attr($('entry'),'aria-label',s.prompt)}",
"const symbols={ArrowUp:'↑',ArrowDown:'↓',ArrowLeft:'←',ArrowRight:'→',Backspace:'Bksp',"
"Delete:'Del',Escape:'Esc',PageUp:'PgUp',PageDown:'PgDn'};"
"fit($('keys'),s.keys.length,()=>el('button',{type:'button'},el('kbd'),el('span'))).forEach((b,i)=>{const k=s.keys[i];"
"text(b.firstChild,symbols[k.key]?\?k.key.replace(/^(Key|Digit)/,''));text(b.lastChild,k.input);"
"attr(b,'aria-keyshortcuts',k.key.replace(/^Key/,'').replace(/^Digit/,''));b.onclick=()=>post('/key',k.key)})}"
"$('prompt').onsubmit=e=>{e.preventDefault();const v=$('entry').value;if(v){$('entry').value='';post('/text',v)}};"
"addEventListener('keydown',e=>{if(!screen||e.ctrlKey||e.metaKey||e.altKey)return;"
"const typing=document.activeElement===$('entry');"
"if(typing&&!['ArrowUp','ArrowDown','PageUp','PageDown','Escape'].includes(e.code))return;"
"if(document.activeElement?.tagName==='BUTTON'&&['Enter','Space'].includes(e.code))return;"
"if(screen.keys.some(k=>k.key===e.code)){e.preventDefault();post('/key',e.code)}});"
"new EventSource('/events').onmessage=refresh;refresh();"
"</script></body></html>"};

/* The page, joined once from its pieces. */
static const Buffer *page(void) {
    static Buffer joined;
    if (!joined.used)
        for (size_t i = 0; i < sizeof(page_parts) / sizeof(*page_parts); ++i) puts_(&joined, page_parts[i]);
    return &joined;
}

static void respond(Client *c, int status, const char *reason, const char *type, const char *body, size_t length) {
    putf(&c->out, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%s\r\n",
         status, reason, type, length, c->closing ? "Connection: close\r\n" : "");
    put(&c->out, body, length);
}

static bool query_number(const char *query, const char *name, size_t *out) {
    size_t length = strlen(name);
    for (const char *at = query; at && *at; at = strchr(at, '&') ? strchr(at, '&') + 1 : NULL)
        if (!strncmp(at, name, length) && at[length] == '=') {
            *out = strtoul(at + length + 1, NULL, 10);
            return true;
        }
    return false;
}

/* first=F,G,...: the parts' first rows, in the order of the screen's parts. */
static void query_firsts(const char *query, const VegaScreen *screen, size_t *firsts) {
    const char *at = query ? strstr(query, "first=") : NULL;
    if (!at || (at != query && at[-1] != '&')) return;
    at += 6;
    for (unsigned i = 0; i < screen->count && *at && *at != '&'; ++i) {
        if (screen->parts[i].kind == VEGA_FIELD) continue;
        firsts[i] = strtoul(at, (char **)&at, 10);
        if (*at == ',') ++at;
    }
}

/* One request. Returns whether the app changed. */
static bool handle(Client *c, PureApp *app, const VegaScreen *screen, const char *method, char *target,
                   const char *body, size_t body_size) {
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    uint64_t before = app->events;
    if (get && !strcmp(target, "/")) {
        respond(c, 200, "OK", "text/html; charset=utf-8", page()->bytes, page()->used);
        return false;
    }
    if (get && !strcmp(target, "/screen")) {
        size_t visible = 50, firsts[VEGA_PARTS] = {0};
        query_number(query, "rows", &visible);
        if (visible > ROWS_MAX) visible = ROWS_MAX;
        query_firsts(query, screen, firsts);
        Buffer json = {0};
        screen_json(&json, app, screen, visible, firsts);
        respond(c, 200, "OK", "application/json", json.bytes ? json.bytes : "", json.used);
        free(json.bytes);
        return false;
    }
    if (get && !strcmp(target, "/events")) {
        puts_(&c->out, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n\r\n");
        putf(&c->out, "data: %llu\n\n", (unsigned long long)app->events);
        c->events = true;
        return false;
    }
    if (post && (!strcmp(target, "/key") || !strcmp(target, "/text") || !strcmp(target, "/point"))) {
        char text[VEGA_TEXT];
        size_t length = body_size < sizeof(text) - 1 ? body_size : sizeof(text) - 1;
        memcpy(text, body, length);
        text[length] = 0;
        bool ok = false;
        if (target[1] == 'k') ok = vega_press(app, text);
        else if (target[1] == 't') ok = vega_commit(app, screen, text, length);
        else {
            unsigned part = 0;
            long row = -1;
            ok = sscanf(text, "%u %ld", &part, &row) == 2 && part < screen->count && row >= 0 &&
                 screen->parts[part].kind != VEGA_FIELD && vega_point(app, &screen->parts[part], (size_t)row);
        }
        const char *answer = ok ? "{\"ok\":true}" : "{\"ok\":false}";
        respond(c, 200, "OK", "application/json", answer, strlen(answer));
        return app->events != before;
    }
    respond(c, 404, "Not Found", "text/plain", "not found\n", 10);
    return false;
}

/* The requests complete in c->in, answered in order. */
static bool serve(Client *c, PureApp *app, const VegaScreen *screen) {
    bool changed = false;
    while (!c->events && !c->closing) {
        char *end = memmem(c->in, c->in_used, "\r\n\r\n", 4);
        if (!end) {
            if (c->in_used == sizeof(c->in)) c->closing = true;
            return changed;
        }
        size_t head = (size_t)(end - c->in) + 4, body_size = 0;
        *end = 0;
        char *length = strcasestr(c->in, "\r\ncontent-length:");
        if (length) body_size = strtoul(length + 17, NULL, 10);
        if (body_size > sizeof(c->in) - head) { c->closing = true; return changed; }
        if (c->in_used < head + body_size) { *end = '\r'; return changed; }
        if (strcasestr(c->in, "\r\nconnection: close")) c->closing = true;
        char method[8] = "", target[1024] = "";
        if (sscanf(c->in, "%7s %1023s", method, target) == 2)
            changed = handle(c, app, screen, method, target, c->in + head, body_size) || changed;
        else c->closing = true;
        memmove(c->in, c->in + head + body_size, c->in_used - head - body_size);
        c->in_used -= head + body_size;
    }
    return changed;
}

static void drop(Client *c) {
    close(c->fd);
    free(c->out.bytes);
    c->fd = -1;
}

int vega_web(PureApp *app, const VegaScreen *screen, const char *listen_on, unsigned port, bool (*reload)(void)) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    socklen_t size = sizeof(address);
    if (listener < 0 || inet_pton(AF_INET, listen_on, &address.sin_addr) != 1 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 64) ||
        getsockname(listener, (struct sockaddr *)&address, &size)) {
        fprintf(stderr, "vega: cannot listen on %s:%u\n", listen_on, port);
        return 2;
    }
    printf("{\"port\":%u}\n", ntohs(address.sin_port));
    fflush(stdout);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    signal(SIGPIPE, SIG_IGN);
    static Client clients[CLIENTS];
    for (unsigned i = 0; i < CLIENTS; ++i) clients[i].fd = -1;
    VegaTimers timers;
    vega_timers_start(&timers, app->module, vega_clock());
    while (!stopping) {
        /* Tick what is due; every page hears of it this round, without waiting. */
        bool ticked = false;
        long due = vega_timers_run(&timers, app, vega_clock(), &ticked);
        struct pollfd fds[CLIENTS + 1];
        unsigned slot[CLIENTS + 1], count = 1;
        fds[0] = (struct pollfd){listener, POLLIN, 0};
        for (unsigned i = 0; i < CLIENTS; ++i) {
            Client *c = &clients[i];
            if (c->fd < 0) continue;
            fds[count] = (struct pollfd){c->fd, (short)(POLLIN | (c->out_at < c->out.used ? POLLOUT : 0)), 0};
            slot[count++] = i;
        }
        int timeout = ticked ? 0 : screen->ticks ? 33 : reload ? 200 : 1000;
        if (due >= 0 && due < timeout) timeout = (int)due;
        int ready = poll(fds, count, timeout);
        if (ready < 0 && errno != EINTR) break;
        bool changed = ticked;
        app->now = vega_now(); /* what the events of this round see */
        if (ready == 0 && screen->ticks) changed = pure_app_frame(app) == PURE_OK;
        if (reload && reload()) { /* a new module: every page draws it */
            changed = true;
            vega_timers_start(&timers, app->module, vega_clock());
        }
        if (fds[0].revents & POLLIN)
            for (int fd; (fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0;) {
                unsigned free_slot = CLIENTS;
                for (unsigned i = 0; i < CLIENTS && free_slot == CLIENTS; ++i) if (clients[i].fd < 0) free_slot = i;
                if (free_slot == CLIENTS) { close(fd); continue; }
                clients[free_slot] = (Client){.fd = fd};
            }
        for (unsigned k = 1; k < count; ++k) {
            Client *c = &clients[slot[k]];
            if (fds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t n = read(c->fd, c->in + c->in_used, sizeof(c->in) - c->in_used);
                if (n > 0) {
                    c->in_used += (size_t)n;
                    changed = serve(c, app, screen) || changed;
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { drop(c); continue; }
            }
        }
        /* Every open page hears of a change once. */
        if (changed)
            for (unsigned i = 0; i < CLIENTS; ++i)
                if (clients[i].fd >= 0 && clients[i].events)
                    putf(&clients[i].out, "data: %llu\n\n", (unsigned long long)app->events);
        for (unsigned i = 0; i < CLIENTS; ++i) {
            Client *c = &clients[i];
            if (c->fd < 0) continue;
            while (c->out_at < c->out.used) {
                ssize_t n = write(c->fd, c->out.bytes + c->out_at, c->out.used - c->out_at);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                    c->closing = true;
                    c->out_at = c->out.used;
                    break;
                }
                c->out_at += (size_t)n;
            }
            if (c->out_at == c->out.used) c->out_at = c->out.used = 0;
            if (c->closing && !c->out.used) drop(c);
        }
    }
    for (unsigned i = 0; i < CLIENTS; ++i) if (clients[i].fd >= 0) drop(&clients[i]);
    close(listener);
    return 0;
}
