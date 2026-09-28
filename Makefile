CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build
# Sibling checkouts: Pure runs the modules (libpure.a: the app model and the
# tree store, headers in core/), aup builds them.
PURE ?= ../pure
AUP ?= ../aup
LIBPURE = $(PURE)/build/libpure.a

# Parts: lib/ (the aup library: the interface vocabulary), surfaces/ (the vega
# host: TUI, CLI, NUI sentences, web), symbols/ (glyphs and icons as one
# format, drawn by CPU or GPU), text/ (Unicode's algorithms over tables
# text/unicode.py generates from the UCD: python3 text/unicode.py > text/unicode-data.c).
SURFACES = surfaces/screen.c surfaces/draw.c surfaces/width.c surfaces/web.c surfaces/pixels.c surfaces/display.c \
           surfaces/input.c surfaces/hangul.c surfaces/main.c

.PHONY: all check FORCE
all: $(BUILD)/vega $(BUILD)/vega-symbols $(BUILD)/vega-surface.so

$(BUILD):
	mkdir -p $@

$(LIBPURE): FORCE
	$(MAKE) -C $(PURE) build/libpure.a

$(BUILD)/vega: $(SURFACES) surfaces/vega.h symbols/symbols.c symbols/symbols.h $(LIBPURE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PURE)/core -I$(PURE)/serve -Isymbols $(SURFACES) symbols/symbols.c $(LIBPURE) $(LDFLAGS) -lm $(LDLIBS) -o $@

# The same host as a surface a Pure node maps (pure-serve --surface, Pure's
# serve/surface.h): the node's display. It calls the app model pure-serve
# exports, so it links no libpure.a of its own; it exports pure_surface_run
# alone, and what only the vega command uses is left out. It is built again
# when Pure's headers change: a node refuses a surface built for another
# layout of its app and module.
PURE_HEADERS = $(wildcard $(PURE)/core/*.h) $(PURE)/serve/surface.h
$(BUILD)/vega-surface.so: $(SURFACES) surfaces/vega.h symbols/symbols.c symbols/symbols.h $(PURE_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -fPIC -shared -fvisibility=hidden -ffunction-sections \
		-fdata-sections -DVEGA_SURFACE -I$(PURE)/core -I$(PURE)/serve -Isymbols $(SURFACES) symbols/symbols.c \
		$(LDFLAGS) -Wl,--gc-sections -lm $(LDLIBS) -o $@

# Building symbol files reads fonts through FreeType; reading them does not.
FREETYPE_CFLAGS ?= $(shell pkg-config --cflags freetype2)
FREETYPE_LIBS ?= $(shell pkg-config --libs freetype2)
# Drawing them on the GPU takes OpenGL ES 3.1 through EGL.
GL_LIBS ?= $(shell pkg-config --libs egl glesv2)
SYMBOLS = symbols/symbols.c symbols/symbols-gl.c
$(BUILD)/vega-symbols: symbols/symbols-tool.c $(SYMBOLS) symbols/symbols.h symbols/symbols-gl.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FREETYPE_CFLAGS) symbols/symbols-tool.c $(SYMBOLS) $(LDFLAGS) \
		$(FREETYPE_LIBS) $(GL_LIBS) -lm $(LDLIBS) -o $@

TEXT = text/bidi.c text/normalize.c text/unicode-data.c
$(BUILD)/test-text: text/test-text.c $(TEXT) text/text.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itext text/test-text.c $(TEXT) $(LDFLAGS) $(LDLIBS) -o $@

# Unicode's conformance tests, where the UCD is installed (Debian: unicode-data).
UCD ?= /usr/share/unicode
check: $(BUILD)/vega $(BUILD)/vega-symbols $(BUILD)/vega-surface.so $(BUILD)/test-text
	if [ -f $(UCD)/BidiTest.txt ]; then $(BUILD)/test-text $(UCD); else echo "no UCD at $(UCD): text tests skipped"; fi
	$(MAKE) -C $(AUP) build/aup
	$(MAKE) -C $(PURE) build/pure-serve
	python3 tests/check.py --vega $(BUILD)/vega --symbols $(BUILD)/vega-symbols --aup $(AUP)/build/aup --serve $(PURE)/build/pure-serve
