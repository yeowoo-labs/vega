/* D-Bus, the part Vega speaks (surfaces/dbus.h), from the specification:
 * authentication is the text protocol's EXTERNAL mechanism; a message is a
 * 12-byte fixed header (endianness 'l', type, flags, version 1, body
 * length, serial), an array of header fields (code, variant), padding to 8,
 * then the body; every value is aligned to its size from the message's
 * start (strings to 4, structs to 8). */
#define _GNU_SOURCE
#include "dbus.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- Bodies */

static void reserve(VegaDbusBody *b, size_t n) {
    if (b->failed || b->used + n <= b->size) return;
    size_t size = b->size ? b->size : 256;
    while (size < b->used + n) size *= 2;
    unsigned char *grown = realloc(b->bytes, size);
    if (!grown) {
        b->failed = true;
        return;
    }
    b->bytes = grown;
    b->size = size;
}

static void append(VegaDbusBody *b, const void *bytes, size_t n) {
    reserve(b, n);
    if (b->failed) return;
    memcpy(b->bytes + b->used, bytes, n);
    b->used += n;
}

static void pad(VegaDbusBody *b, unsigned alignment) {
    static const unsigned char zero[8] = {0};
    size_t n = (alignment - b->used % alignment) % alignment;
    append(b, zero, n);
}

void vega_dbus_put_byte(VegaDbusBody *b, uint8_t v) { append(b, &v, 1); }

void vega_dbus_put_u32(VegaDbusBody *b, uint32_t v) {
    pad(b, 4);
    unsigned char bytes[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    append(b, bytes, 4);
}

void vega_dbus_put_i32(VegaDbusBody *b, int32_t v) { vega_dbus_put_u32(b, (uint32_t)v); }
void vega_dbus_put_bool(VegaDbusBody *b, bool v) { vega_dbus_put_u32(b, v ? 1 : 0); }

void vega_dbus_put_string(VegaDbusBody *b, const char *s) {
    size_t n = strlen(s);
    vega_dbus_put_u32(b, (uint32_t)n);
    append(b, s, n + 1);
}

void vega_dbus_put_signature(VegaDbusBody *b, const char *s) {
    size_t n = strlen(s);
    vega_dbus_put_byte(b, (uint8_t)n);
    append(b, s, n + 1);
}

void vega_dbus_put_struct(VegaDbusBody *b) { pad(b, 8); }

size_t vega_dbus_put_array(VegaDbusBody *b, unsigned alignment) {
    vega_dbus_put_u32(b, 0);
    size_t at = b->used - 4;
    pad(b, alignment);
    return at;
}

void vega_dbus_end_array(VegaDbusBody *b, size_t at, unsigned alignment) {
    if (b->failed) return;
    size_t start = at + 4;
    start += (alignment - start % alignment) % alignment;
    uint32_t length = (uint32_t)(b->used - start);
    for (int i = 0; i < 4; ++i) b->bytes[at + i] = (unsigned char)(length >> (8 * i));
}

void vega_dbus_body_free(VegaDbusBody *b) {
    free(b->bytes);
    *b = (VegaDbusBody){0};
}

/* ---- Reading */

void vega_dbus_reader(VegaDbusReader *r, const VegaDbusMessage *m) { *r = (VegaDbusReader){m->body, m->body_size, 0, false}; }

static bool skip_to(VegaDbusReader *r, unsigned alignment, size_t need) {
    size_t at = r->at + (alignment - r->at % alignment) % alignment;
    if (r->failed || at > r->size || need > r->size - at) {
        r->failed = true;
        return false;
    }
    r->at = at;
    return true;
}

uint32_t vega_dbus_get_u32(VegaDbusReader *r) {
    if (!skip_to(r, 4, 4)) return 0;
    const unsigned char *p = r->bytes + r->at;
    r->at += 4;
    return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int32_t vega_dbus_get_i32(VegaDbusReader *r) { return (int32_t)vega_dbus_get_u32(r); }

const char *vega_dbus_get_string(VegaDbusReader *r) {
    uint32_t n = vega_dbus_get_u32(r);
    if (r->failed || n >= r->size - r->at || r->bytes[r->at + n]) {
        r->failed = true;
        return "";
    }
    const char *s = (const char *)r->bytes + r->at;
    r->at += n + 1;
    return s;
}

const char *vega_dbus_get_signature(VegaDbusReader *r) {
    if (!skip_to(r, 1, 1)) return "";
    size_t n = r->bytes[r->at++];
    if (n >= r->size - r->at || r->bytes[r->at + n]) {
        r->failed = true;
        return "";
    }
    const char *s = (const char *)r->bytes + r->at;
    r->at += n + 1;
    return s;
}

void vega_dbus_get_struct(VegaDbusReader *r) { skip_to(r, 8, 0); }

/* ---- The connection */

static bool write_all(int fd, const void *bytes, size_t n) {
    const char *p = bytes;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            struct pollfd wait = {fd, POLLOUT, 0};
            if (poll(&wait, 1, 5000) <= 0) return false;
            continue;
        }
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* One line of the authentication protocol, into line (without \r\n). */
static bool read_line(int fd, char *line, size_t size) {
    size_t n = 0;
    while (n + 1 < size) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        if (c == '\n') {
            if (n && line[n - 1] == '\r') --n;
            line[n] = 0;
            return true;
        }
        line[n++] = c;
    }
    return false;
}

#define REFUSE(...)                                  \
    do {                                             \
        snprintf(error, error_size, __VA_ARGS__);    \
        vega_bus_close(bus);                         \
        return false;                                \
    } while (0)

bool vega_bus_open(VegaBus *bus, const char *address, char *error, size_t error_size) {
    *bus = (VegaBus){-1, 0, "", NULL, 0, 0, 0};
    /* The first unix address of the list: path=, or abstract= (a name
     * outside the file system). */
    struct sockaddr_un where = {.sun_family = AF_UNIX};
    socklen_t length = 0;
    for (const char *p = address; p && *p;) {
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!strncmp(p, "unix:", 5)) {
            for (const char *key = p + 5; key < p + n;) {
                const char *comma = memchr(key, ',', (size_t)(p + n - key));
                size_t k = comma ? (size_t)(comma - key) : (size_t)(p + n - key);
                bool path = k > 5 && !strncmp(key, "path=", 5), abstract = k > 9 && !strncmp(key, "abstract=", 9);
                if (path || abstract) {
                    size_t skip = path ? 5 : 9, name = k - skip;
                    if (name + 1 + abstract > sizeof(where.sun_path)) REFUSE("bus address too long");
                    memcpy(where.sun_path + abstract, key + skip, name);
                    length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + name + abstract + (path ? 1 : 0));
                }
                key += k + (comma ? 1 : 0);
            }
            if (length) break;
        }
        p = end ? end + 1 : NULL;
    }
    if (!length) REFUSE("no unix address in %s", address);
    bus->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (bus->fd < 0 || connect(bus->fd, (struct sockaddr *)&where, length) < 0) REFUSE("cannot connect to %s", address);
    /* EXTERNAL: who we are is what the socket says; we name our uid (in
     * decimal, hex-encoded). */
    char uid[32], hex[80], line[256];
    snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    for (size_t i = 0; uid[i]; ++i) snprintf(hex + 2 * i, 3, "%02x", (unsigned char)uid[i]);
    char auth[128];
    int n = snprintf(auth, sizeof(auth), "AUTH EXTERNAL %s\r\n", hex);
    if (!write_all(bus->fd, "", 1) || !write_all(bus->fd, auth, (size_t)n) || !read_line(bus->fd, line, sizeof(line)) ||
        strncmp(line, "OK ", 3) || !write_all(bus->fd, "BEGIN\r\n", 7))
        REFUSE("the bus did not take us");
    VegaDbusMessage reply;
    if (!vega_bus_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL, NULL,
                       &reply, NULL, NULL) ||
        reply.type != VEGA_DBUS_RETURN)
        REFUSE("no Hello from the bus");
    VegaDbusReader r;
    vega_dbus_reader(&r, &reply);
    const char *name = vega_dbus_get_string(&r);
    if (r.failed || strlen(name) >= sizeof(bus->name)) REFUSE("a strange name from the bus");
    strcpy(bus->name, name);
    return true;
}

void vega_bus_close(VegaBus *bus) {
    if (bus->fd >= 0) close(bus->fd);
    free(bus->in);
    *bus = (VegaBus){-1, 0, "", NULL, 0, 0, 0};
}

static void field(VegaDbusBody *h, uint8_t code, const char *type, const char *value, uint32_t number) {
    vega_dbus_put_struct(h);
    vega_dbus_put_byte(h, code);
    vega_dbus_put_signature(h, type);
    if (type[0] == 'u') vega_dbus_put_u32(h, number);
    else if (type[0] == 'g') vega_dbus_put_signature(h, value);
    else vega_dbus_put_string(h, value);
}

bool vega_bus_send(VegaBus *bus, uint8_t type, uint8_t flags, uint32_t reply_serial, const char *destination,
                   const char *path, const char *interface, const char *member, const char *error,
                   const char *signature, const VegaDbusBody *body, uint32_t *serial) {
    VegaDbusBody h = {0};
    uint32_t number = ++bus->serial ? bus->serial : ++bus->serial;
    unsigned char fixed[4] = {'l', type, flags, 1};
    append(&h, fixed, 4);
    vega_dbus_put_u32(&h, body ? (uint32_t)body->used : 0);
    vega_dbus_put_u32(&h, number);
    size_t fields = vega_dbus_put_array(&h, 8);
    if (path) field(&h, 1, "o", path, 0);
    if (interface) field(&h, 2, "s", interface, 0);
    if (member) field(&h, 3, "s", member, 0);
    if (error) field(&h, 4, "s", error, 0);
    if (reply_serial) field(&h, 5, "u", NULL, reply_serial);
    if (destination) field(&h, 6, "s", destination, 0);
    if (signature && *signature) field(&h, 8, "g", signature, 0);
    vega_dbus_end_array(&h, fields, 8);
    pad(&h, 8);
    if (body) append(&h, body->bytes, body->used);
    bool sent = !h.failed && (!body || !body->failed) && write_all(bus->fd, h.bytes, h.used);
    vega_dbus_body_free(&h);
    if (serial) *serial = number;
    return sent;
}

/* A whole message at the start of bus->in, parsed into m; its length, or 0
 * when it is not all there yet. A message that is not little-endian or
 * whose header does not parse is passed over (type 0). */
static size_t parse(const unsigned char *in, size_t used, VegaDbusMessage *m) {
    if (used < 16) return 0;
    uint32_t body = in[4] | in[5] << 8 | (uint32_t)in[6] << 16 | (uint32_t)in[7] << 24;
    uint32_t fields = in[12] | in[13] << 8 | (uint32_t)in[14] << 16 | (uint32_t)in[15] << 24;
    if (fields > (1u << 26) || body > (1u << 27)) return used; /* nonsense: drop what we have */
    size_t header = 16 + fields, total = header + (8 - header % 8) % 8 + body;
    if (used < total) return 0;
    *m = (VegaDbusMessage){0};
    if (in[0] != 'l') return total;
    m->type = in[1];
    m->flags = in[2];
    m->serial = in[8] | in[9] << 8 | (uint32_t)in[10] << 16 | (uint32_t)in[11] << 24;
    VegaDbusReader r = {in, header, 16, false};
    while (!r.failed && r.at < header) {
        vega_dbus_get_struct(&r);
        if (r.at >= header) break;
        uint8_t code = in[r.at++];
        const char *type = vega_dbus_get_signature(&r);
        if (type[0] == 'u' && !type[1]) {
            uint32_t v = vega_dbus_get_u32(&r);
            if (code == 5) m->reply_serial = v;
        } else if (type[0] == 'g' && !type[1]) {
            const char *v = vega_dbus_get_signature(&r);
            if (code == 8) m->signature = v;
        } else if ((type[0] == 's' || type[0] == 'o') && !type[1]) {
            const char *v = vega_dbus_get_string(&r);
            if (code == 1) m->path = v;
            else if (code == 2) m->interface = v;
            else if (code == 3) m->member = v;
            else if (code == 4) m->error = v;
            else if (code == 6) m->destination = v;
            else if (code == 7) m->sender = v;
        } else
            r.failed = true;
    }
    if (r.failed) m->type = 0;
    m->body = in + header + (8 - header % 8) % 8;
    m->body_size = body;
    if (!m->signature) m->signature = "";
    return total;
}

/* Bytes from the socket into bus->in, waiting up to timeout ms for them
 * (0: they are there); false when the bus is gone. */
static bool fill(VegaBus *bus, int timeout) {
    if (bus->in_used == bus->in_size) {
        size_t size = bus->in_size ? 2 * bus->in_size : 65536;
        unsigned char *grown = realloc(bus->in, size);
        if (!grown) return false;
        bus->in = grown;
        bus->in_size = size;
    }
    struct pollfd wait = {bus->fd, POLLIN, 0};
    if (timeout) {
        int ready = poll(&wait, 1, timeout);
        if (ready < 0 && errno != EINTR) return false;
        if (ready <= 0) return true; /* nothing yet */
    }
    ssize_t n = read(bus->fd, bus->in + bus->in_used, bus->in_size - bus->in_used);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
    if (n <= 0) return false;
    bus->in_used += (size_t)n;
    return true;
}

/* Hand over every whole message in bus->in; stop at the reply to serial
 * (when not 0), left at the start of bus->in and returned in *reply. */
static bool drain(VegaBus *bus, uint32_t serial, VegaDbusMessage *reply,
                  void (*handle)(void *, const VegaDbusMessage *), void *context) {
    size_t at = 0;
    bool found = false;
    for (;;) {
        VegaDbusMessage m;
        size_t n = parse(bus->in + at, bus->in_used - at, &m);
        if (!n) break;
        if (serial && m.reply_serial == serial && (m.type == VEGA_DBUS_RETURN || m.type == VEGA_DBUS_ERROR)) {
            /* keep it where it is, at the front, until the next read */
            memmove(bus->in, bus->in + at, bus->in_used - at);
            bus->in_used -= at;
            parse(bus->in, bus->in_used, reply);
            found = true;
            at = 0;
            break;
        }
        if (m.type && handle) handle(context, &m);
        at += n;
    }
    if (at) {
        memmove(bus->in, bus->in + at, bus->in_used - at);
        bus->in_used -= at;
    }
    return found;
}

/* A reply kept at the front of bus->in from the last call is done with. */
static void forget_reply(VegaBus *bus) {
    VegaDbusMessage m;
    size_t n = parse(bus->in, bus->in_used, &m);
    if (n && m.reply_serial && (m.type == VEGA_DBUS_RETURN || m.type == VEGA_DBUS_ERROR) && bus->kept == m.reply_serial) {
        memmove(bus->in, bus->in + n, bus->in_used - n);
        bus->in_used -= n;
    }
    bus->kept = 0;
}

bool vega_bus_call(VegaBus *bus, const char *destination, const char *path, const char *interface, const char *member,
                   const char *signature, const VegaDbusBody *body, VegaDbusMessage *reply,
                   void (*handle)(void *, const VegaDbusMessage *), void *context) {
    forget_reply(bus);
    uint32_t serial;
    if (!vega_bus_send(bus, VEGA_DBUS_CALL, 0, 0, destination, path, interface, member, NULL, signature, body, &serial))
        return false;
    /* Five seconds for the reply: a bus that takes longer is not there. */
    for (int waited = 0; waited < 50; ++waited) {
        if (drain(bus, serial, reply, handle, context)) {
            bus->kept = serial;
            return true;
        }
        if (!fill(bus, 100)) return false;
    }
    return false;
}

bool vega_bus_read(VegaBus *bus, void (*handle)(void *, const VegaDbusMessage *), void *context) {
    forget_reply(bus);
    if (!fill(bus, 0)) return false;
    drain(bus, 0, NULL, handle, context);
    return true;
}
