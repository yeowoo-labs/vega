#ifndef VEGA_DBUS_H
#define VEGA_DBUS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The part of D-Bus Vega speaks (the D-Bus specification's wire format, no
 * libdbus): one connection to a bus over a Unix socket, authenticated as
 * the user (EXTERNAL), messages built and read little-endian. Accessibility
 * (access.c) is what uses it. */

enum { VEGA_DBUS_CALL = 1, VEGA_DBUS_RETURN = 2, VEGA_DBUS_ERROR = 3, VEGA_DBUS_SIGNAL = 4 };
enum { VEGA_DBUS_NO_REPLY = 1 };

/* What a message carries, built with the vega_dbus_put functions: each
 * value aligned as the wire format wants it, relative to the body's start
 * (a message's body starts 8-aligned). */
typedef struct {
    unsigned char *bytes;
    size_t used, size;
    bool failed;
} VegaDbusBody;
void vega_dbus_put_byte(VegaDbusBody *, uint8_t);
void vega_dbus_put_bool(VegaDbusBody *, bool);
void vega_dbus_put_i32(VegaDbusBody *, int32_t);
void vega_dbus_put_u32(VegaDbusBody *, uint32_t);
void vega_dbus_put_string(VegaDbusBody *, const char *);  /* also an object path */
void vega_dbus_put_signature(VegaDbusBody *, const char *);
void vega_dbus_put_struct(VegaDbusBody *);                /* aligns to 8, as a struct or dict entry starts */
/* An array: put_array gives where its length goes; end_array fills it
 * in. alignment is its elements' (8 for structs and dict entries). */
size_t vega_dbus_put_array(VegaDbusBody *, unsigned alignment);
void vega_dbus_end_array(VegaDbusBody *, size_t at, unsigned alignment);
void vega_dbus_body_free(VegaDbusBody *);

/* A message read: its header's fields (NULL when absent) and its body. */
typedef struct {
    uint8_t type, flags;
    uint32_t serial, reply_serial;
    const char *path, *interface, *member, *error, *destination, *sender, *signature;
    const unsigned char *body;
    size_t body_size;
} VegaDbusMessage;

/* Reading a body in order; a read past its end (or of the wrong shape)
 * fails the reader, and every read after gives zero. */
typedef struct {
    const unsigned char *bytes;
    size_t size, at;
    bool failed;
} VegaDbusReader;
void vega_dbus_reader(VegaDbusReader *, const VegaDbusMessage *);
uint32_t vega_dbus_get_u32(VegaDbusReader *);
int32_t vega_dbus_get_i32(VegaDbusReader *);
const char *vega_dbus_get_string(VegaDbusReader *); /* also an object path */
const char *vega_dbus_get_signature(VegaDbusReader *);
void vega_dbus_get_struct(VegaDbusReader *);

typedef struct {
    int fd;
    uint32_t serial;
    char name[128]; /* the unique name the bus gave us */
    unsigned char *in;
    size_t in_used, in_size;
    uint32_t kept; /* the call whose reply stays at the start of in */
} VegaBus;

/* Connect to address (unix:path=... or unix:abstract=...), authenticate,
 * say Hello; false with why. */
bool vega_bus_open(VegaBus *, const char *address, char *error, size_t error_size);
void vega_bus_close(VegaBus *);
/* Send a message; its serial goes to *serial when given. */
bool vega_bus_send(VegaBus *, uint8_t type, uint8_t flags, uint32_t reply_serial, const char *destination,
                   const char *path, const char *interface, const char *member, const char *error,
                   const char *signature, const VegaDbusBody *body, uint32_t *serial);
/* Call a method and wait for its reply (a return or an error) into reply,
 * which stays valid until the next read; messages that come first go to
 * handle. */
bool vega_bus_call(VegaBus *, const char *destination, const char *path, const char *interface, const char *member,
                   const char *signature, const VegaDbusBody *body, VegaDbusMessage *reply,
                   void (*handle)(void *, const VegaDbusMessage *), void *context);
/* Read what the socket has and hand each whole message to handle; false
 * when the bus is gone. */
bool vega_bus_read(VegaBus *, void (*handle)(void *, const VegaDbusMessage *), void *context);
#endif
