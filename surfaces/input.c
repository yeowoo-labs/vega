#define _DEFAULT_SOURCE
#include "vega.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Input from evdev (the kernel's input events): keyboards, mice and touch
 * screens, read without a library. Keys come out as Pure's key names (as
 * KeyboardEvent.code spells them) and the text they type on a US layout;
 * pointers as presses at a pixel. */

enum { DEVICES = 32 };

typedef struct {
    int fd;
    bool touch;                 /* absolute positions, scaled to the screen */
    int min[2], max[2];         /* the device's range on x and y */
} Device;

struct VegaInput {
    Device devices[DEVICES];
    unsigned count;
    unsigned width, height;     /* the screen, for pointers */
    int x, y;                   /* the pointer */
    bool shift, touched, moved;
};

/* Key names by code, with what a key types on a US layout (unshifted, shifted). */
static const struct { unsigned short code; const char *name; char plain, shifted; } keys[] = {
    {KEY_ESC, "Escape", 0, 0}, {KEY_1, "Digit1", '1', '!'}, {KEY_2, "Digit2", '2', '@'}, {KEY_3, "Digit3", '3', '#'},
    {KEY_4, "Digit4", '4', '$'}, {KEY_5, "Digit5", '5', '%'}, {KEY_6, "Digit6", '6', '^'}, {KEY_7, "Digit7", '7', '&'},
    {KEY_8, "Digit8", '8', '*'}, {KEY_9, "Digit9", '9', '('}, {KEY_0, "Digit0", '0', ')'}, {KEY_MINUS, "Minus", '-', '_'},
    {KEY_EQUAL, "Equal", '=', '+'}, {KEY_BACKSPACE, "Backspace", 0, 0}, {KEY_TAB, "Tab", 0, 0},
    {KEY_Q, "KeyQ", 'q', 'Q'}, {KEY_W, "KeyW", 'w', 'W'}, {KEY_E, "KeyE", 'e', 'E'}, {KEY_R, "KeyR", 'r', 'R'},
    {KEY_T, "KeyT", 't', 'T'}, {KEY_Y, "KeyY", 'y', 'Y'}, {KEY_U, "KeyU", 'u', 'U'}, {KEY_I, "KeyI", 'i', 'I'},
    {KEY_O, "KeyO", 'o', 'O'}, {KEY_P, "KeyP", 'p', 'P'}, {KEY_LEFTBRACE, "BracketLeft", '[', '{'},
    {KEY_RIGHTBRACE, "BracketRight", ']', '}'}, {KEY_ENTER, "Enter", 0, 0}, {KEY_A, "KeyA", 'a', 'A'},
    {KEY_S, "KeyS", 's', 'S'}, {KEY_D, "KeyD", 'd', 'D'}, {KEY_F, "KeyF", 'f', 'F'}, {KEY_G, "KeyG", 'g', 'G'},
    {KEY_H, "KeyH", 'h', 'H'}, {KEY_J, "KeyJ", 'j', 'J'}, {KEY_K, "KeyK", 'k', 'K'}, {KEY_L, "KeyL", 'l', 'L'},
    {KEY_SEMICOLON, "Semicolon", ';', ':'}, {KEY_APOSTROPHE, "Quote", '\'', '"'}, {KEY_GRAVE, "Backquote", '`', '~'},
    {KEY_BACKSLASH, "Backslash", '\\', '|'}, {KEY_Z, "KeyZ", 'z', 'Z'}, {KEY_X, "KeyX", 'x', 'X'},
    {KEY_C, "KeyC", 'c', 'C'}, {KEY_V, "KeyV", 'v', 'V'}, {KEY_B, "KeyB", 'b', 'B'}, {KEY_N, "KeyN", 'n', 'N'},
    {KEY_M, "KeyM", 'm', 'M'}, {KEY_COMMA, "Comma", ',', '<'}, {KEY_DOT, "Period", '.', '>'},
    {KEY_SLASH, "Slash", '/', '?'}, {KEY_SPACE, "Space", ' ', ' '}, {KEY_F1, "F1", 0, 0}, {KEY_F2, "F2", 0, 0},
    {KEY_F3, "F3", 0, 0}, {KEY_F4, "F4", 0, 0}, {KEY_F5, "F5", 0, 0}, {KEY_F6, "F6", 0, 0}, {KEY_F7, "F7", 0, 0},
    {KEY_F8, "F8", 0, 0}, {KEY_F9, "F9", 0, 0}, {KEY_F10, "F10", 0, 0}, {KEY_F11, "F11", 0, 0}, {KEY_F12, "F12", 0, 0},
    {KEY_HOME, "Home", 0, 0}, {KEY_UP, "ArrowUp", 0, 0}, {KEY_PAGEUP, "PageUp", 0, 0}, {KEY_LEFT, "ArrowLeft", 0, 0},
    {KEY_RIGHT, "ArrowRight", 0, 0}, {KEY_END, "End", 0, 0}, {KEY_DOWN, "ArrowDown", 0, 0},
    {KEY_PAGEDOWN, "PageDown", 0, 0}, {KEY_INSERT, "Insert", 0, 0}, {KEY_DELETE, "Delete", 0, 0},
};

static bool has(int fd, unsigned type, unsigned code) {
    unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits) < 0) return false;
    return bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long))) & 1;
}

/* A device worth reading: one with keys, a relative pointer, or touch. */
static bool add(VegaInput *input, int fd, bool probe) {
    Device device = {.fd = fd};
    if (probe) {
        bool keys_ = has(fd, EV_KEY, KEY_A) || has(fd, EV_KEY, KEY_ENTER), mouse = has(fd, EV_REL, REL_X);
        device.touch = has(fd, EV_ABS, ABS_MT_POSITION_X) || has(fd, EV_ABS, ABS_X);
        if (!keys_ && !mouse && !device.touch) return false;
        for (int axis = 0; device.touch && axis < 2; ++axis) {
            struct input_absinfo range;
            unsigned code = has(fd, EV_ABS, ABS_MT_POSITION_X) ? (axis ? ABS_MT_POSITION_Y : ABS_MT_POSITION_X)
                                                               : (axis ? ABS_Y : ABS_X);
            if (ioctl(fd, EVIOCGABS(code), &range) < 0 || range.maximum <= range.minimum) device.touch = false;
            else device.min[axis] = range.minimum, device.max[axis] = range.maximum;
        }
    } else device.touch = true, device.min[0] = device.min[1] = 0, device.max[0] = device.max[1] = 0; /* raw: screen pixels */
    if (input->count == DEVICES) return false;
    input->devices[input->count++] = device;
    return true;
}

VegaInput *vega_input_open(const char *const *paths, unsigned count, unsigned width, unsigned height) {
    VegaInput *input = calloc(1, sizeof(*input));
    if (!input) return NULL;
    input->width = width, input->height = height, input->x = (int)width / 2, input->y = (int)height / 2;
    if (!paths) return input; /* none */
    if (count) { /* files of recorded events, positions already in screen pixels */
        for (unsigned i = 0; i < count; ++i) {
            int fd = open(paths[i], O_RDONLY | O_CLOEXEC);
            if (fd < 0 || !add(input, fd, false)) { if (fd >= 0) close(fd); vega_input_close(input); return NULL; }
        }
        return input;
    }
    DIR *directory = opendir("/dev/input");
    struct dirent *entry;
    while (directory && (entry = readdir(directory))) {
        if (strncmp(entry->d_name, "event", 5)) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0 && !add(input, fd, true)) close(fd);
    }
    if (directory) closedir(directory);
    return input;
}

unsigned vega_input_fds(const VegaInput *input, struct pollfd *out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < input->count && n < max; ++i) out[n++] = (struct pollfd){input->devices[i].fd, POLLIN, 0};
    return n;
}

static int scaled(const Device *device, int axis, int value, unsigned size) {
    if (device->max[axis] <= device->min[axis]) return value; /* already in screen pixels */
    long long span = device->max[axis] - device->min[axis];
    return (int)((long long)(value - device->min[axis]) * (long long)(size - 1) / span);
}

static unsigned emit(VegaEvent *out, unsigned n, unsigned max, VegaEvent event) {
    if (n < max) out[n++] = event;
    return n;
}

/* One event of a device into what it means; returns the new count. */
static unsigned translate(VegaInput *input, const Device *device, const struct input_event *e, VegaEvent *out, unsigned n,
                          unsigned max) {
    if (e->type == EV_KEY && (e->code == KEY_LEFTSHIFT || e->code == KEY_RIGHTSHIFT)) {
        input->shift = e->value != 0;
        return n;
    }
    if (e->type == EV_KEY && (e->code == BTN_LEFT || e->code == BTN_TOUCH) && e->value == 1) {
        if (e->code == BTN_TOUCH) input->touched = true;
        return emit(out, n, max, (VegaEvent){VEGA_EVENT_PRESS, "", "", input->x, input->y, 0});
    }
    if (e->type == EV_KEY && e->value != 0) { /* a press or its repeat */
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i)
            if (keys[i].code == e->code) {
                VegaEvent event = {VEGA_EVENT_KEY, "", "", input->x, input->y, 0};
                snprintf(event.name, sizeof(event.name), "%s", keys[i].name);
                char c = input->shift ? keys[i].shifted : keys[i].plain;
                if (c) event.kind = VEGA_EVENT_TEXT, event.text[0] = c;
                return emit(out, n, max, event);
            }
        return n;
    }
    if (e->type == EV_REL && (e->code == REL_X || e->code == REL_Y)) {
        int *at = e->code == REL_X ? &input->x : &input->y, limit = (int)(e->code == REL_X ? input->width : input->height);
        *at += e->value;
        *at = *at < 0 ? 0 : *at >= limit ? limit - 1 : *at;
        input->moved = true;
        return n;
    }
    if (e->type == EV_REL && e->code == REL_WHEEL && e->value)
        return emit(out, n, max, (VegaEvent){VEGA_EVENT_WHEEL, "", "", input->x, input->y, e->value > 0 ? -1 : 1});
    if (e->type == EV_ABS && (e->code == ABS_MT_POSITION_X || e->code == ABS_X)) {
        input->x = scaled(device, 0, e->value, input->width);
        input->moved = true;
    } else if (e->type == EV_ABS && (e->code == ABS_MT_POSITION_Y || e->code == ABS_Y)) {
        input->y = scaled(device, 1, e->value, input->height);
        input->moved = true;
    }
    return n;
}

unsigned vega_input_read(VegaInput *input, int fd, VegaEvent *out, unsigned max, bool *ended) {
    Device *device = NULL;
    for (unsigned i = 0; i < input->count; ++i) if (input->devices[i].fd == fd) device = &input->devices[i];
    *ended = false;
    if (!device) return 0;
    struct input_event events[64];
    ssize_t got = read(fd, events, sizeof(events));
    if (got <= 0) {
        *ended = got == 0 || (errno != EAGAIN && errno != EINTR);
        return 0;
    }
    unsigned n = 0;
    for (size_t i = 0; i < (size_t)got / sizeof(*events); ++i) n = translate(input, device, &events[i], out, n, max);
    return n;
}

void vega_input_pointer(const VegaInput *input, int *x, int *y, bool *shown) {
    *x = input->x, *y = input->y;
    *shown = input->moved && !input->touched; /* a mouse has a pointer to see; a finger does not */
}

void vega_input_close(VegaInput *input) {
    if (!input) return;
    for (unsigned i = 0; i < input->count; ++i) close(input->devices[i].fd);
    free(input);
}
