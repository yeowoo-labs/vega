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
           surfaces/input.c surfaces/hangul.c surfaces/dbus.c surfaces/access.c surfaces/main.c

.PHONY: all check check-text check-harfbuzz FORCE
all: $(BUILD)/vega $(BUILD)/vega-symbols $(BUILD)/vega-surface.so

$(BUILD):
	mkdir -p $@

$(LIBPURE): FORCE
	$(MAKE) -C $(PURE) build/libpure.a

# The pixel surfaces set type: the text modules come in (TYPE, below).
TYPE = text/layout.c text/shape.c text/syllabic-data.c text/bidi.c text/linebreak.c text/grapheme.c text/unicode-data.c
TYPE_HEADERS = text/layout.h text/shape.h text/text.h text/syllabic.h
$(BUILD)/vega: $(SURFACES) surfaces/vega.h surfaces/dbus.h symbols/symbols.c symbols/symbols.h $(TYPE) $(TYPE_HEADERS) \
		$(LIBPURE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PURE)/core -I$(PURE)/serve -Isymbols -Itext $(SURFACES) symbols/symbols.c $(TYPE) \
		$(LIBPURE) $(LDFLAGS) -lm $(LDLIBS) -o $@

# The same host as a surface a Pure node maps (pure-serve --surface, Pure's
# serve/surface.h): the node's display. It calls the app model pure-serve
# exports, so it links no libpure.a of its own; it exports pure_surface_run
# alone, and what only the vega command uses is left out. It is built again
# when Pure's headers change: a node refuses a surface built for another
# layout of its app and module.
PURE_HEADERS = $(wildcard $(PURE)/core/*.h) $(PURE)/serve/surface.h
$(BUILD)/vega-surface.so: $(SURFACES) surfaces/vega.h surfaces/dbus.h symbols/symbols.c symbols/symbols.h $(TYPE) \
		$(TYPE_HEADERS) $(PURE_HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-function -fPIC -shared -fvisibility=hidden -ffunction-sections \
		-fdata-sections -DVEGA_SURFACE -I$(PURE)/core -I$(PURE)/serve -Isymbols -Itext $(SURFACES) symbols/symbols.c \
		$(TYPE) $(LDFLAGS) -Wl,--gc-sections -lm $(LDLIBS) -o $@

# Building symbol files reads fonts through FreeType, and compiles and checks
# their shaping with HarfBuzz; reading them needs neither.
FREETYPE_CFLAGS ?= $(shell pkg-config --cflags freetype2)
FREETYPE_LIBS ?= $(shell pkg-config --libs freetype2)
HARFBUZZ_CFLAGS ?= $(shell pkg-config --cflags harfbuzz)
HARFBUZZ_LIBS ?= $(shell pkg-config --libs harfbuzz)
# Drawing them on the GPU takes OpenGL ES 3.1 through EGL.
GL_LIBS ?= $(shell pkg-config --libs egl glesv2)
SYMBOLS = symbols/symbols.c symbols/symbols-gl.c
SHAPE = text/shape.c text/syllabic-data.c text/layout.c text/bidi.c text/linebreak.c text/grapheme.c text/unicode-data.c
$(BUILD)/vega-symbols: symbols/symbols-tool.c $(SYMBOLS) symbols/symbols.h symbols/symbols-gl.h $(SHAPE) text/shape-build.c \
		text/shape.h text/shape-build.h text/layout.h text/text.h text/syllabic.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FREETYPE_CFLAGS) $(HARFBUZZ_CFLAGS) -Itext symbols/symbols-tool.c $(SYMBOLS) $(SHAPE) \
		text/shape-build.c $(LDFLAGS) $(FREETYPE_LIBS) $(HARFBUZZ_LIBS) $(GL_LIBS) -lm $(LDLIBS) -o $@

TEXT = text/bidi.c text/normalize.c text/linebreak.c text/grapheme.c text/word.c text/sentence.c text/unicode-data.c
$(BUILD)/test-text: text/test-text.c $(TEXT) text/text.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itext text/test-text.c $(TEXT) $(LDFLAGS) $(LDLIBS) -o $@

LAYOUT = text/layout.c text/shape.c text/syllabic-data.c text/bidi.c text/linebreak.c text/grapheme.c text/unicode-data.c
$(BUILD)/test-layout: text/test-layout.c $(LAYOUT) text/layout.h text/shape.h text/text.h text/syllabic.h symbols/symbols.c \
		symbols/symbols.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itext -Isymbols text/test-layout.c $(LAYOUT) symbols/symbols.c $(LDFLAGS) -lm $(LDLIBS) -o $@

# Unicode's conformance tests, on the UCD the tables were made from: the one
# text/upstream.py fetches into build/upstream, else the installed one
# (Debian: unicode-data), which test-text passes over when its version is
# not the tables'. And shaping, where DejaVu Sans is: the build refuses a
# file whose shaping differs from HarfBuzz's on random runs of Latin, Greek,
# Cyrillic, Hebrew, Arabic and Common text, both ways.
UNICODE_VERSION := $(shell sed -n '1s/.*Database \([0-9.]*\):.*/\1/p' text/unicode-data.c)
UCD ?= $(if $(wildcard build/upstream/ucd-$(UNICODE_VERSION)/ReadMe.txt),build/upstream/ucd-$(UNICODE_VERSION),/usr/share/unicode)
SHAPED_FONT ?= /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
SHAPED_CODES = 0x20-0x7e,0xa0-0x24f,0x300-0x36f,0x370-0x3ff,0x400-0x4ff,0x591-0x5f4,0x600-0x6ff,0x2000-0x206f
# HarfBuzz's own shaping tests, where a HarfBuzz release's source is
# (HARFBUZZ_SRC: the same version the build links; build/upstream's of that
# version, when text/upstream.py fetched it): each case shaped by Vega and
# by HarfBuzz must agree, but for those text/harfbuzz-suite.known lists.
HARFBUZZ_SRC ?= $(wildcard build/upstream/harfbuzz-$(shell pkg-config --modversion harfbuzz))
check-harfbuzz: $(BUILD)/vega-symbols
	if [ -d "$(HARFBUZZ_SRC)/test/shape/data" ]; then python3 text/harfbuzz-suite.py $(HARFBUZZ_SRC) $(BUILD)/vega-symbols; \
	else echo "no HarfBuzz source at HARFBUZZ_SRC: its test suite skipped"; fi

# What text/upstream.py runs against a new HarfBuzz and UCD: the text alone.
check-text: $(BUILD)/vega-symbols $(BUILD)/test-text $(BUILD)/test-layout check-harfbuzz
	if [ -f $(UCD)/BidiTest.txt ]; then $(BUILD)/test-text $(UCD); else echo "no UCD at $(UCD): text tests skipped"; fi
	if [ -f $(SHAPED_FONT) ]; then $(BUILD)/vega-symbols build --font $(SHAPED_FONT) --codes $(SHAPED_CODES) \
		--shape Latn,Grek,Cyrl,Hebr,Arab,Zyyy --runs 5000 -o $(BUILD)/shaped.vsym && \
		$(BUILD)/test-layout $(BUILD)/shaped.vsym; else echo "no $(SHAPED_FONT): shaping tests skipped"; fi
	if [ -f $(SHAPED_FONT) ] && python3 -c 'import fontTools' 2>/dev/null; then \
		python3 text/test-kern.py $(BUILD)/vega-symbols $(SHAPED_FONT); else echo "no fontTools: kern class tests skipped"; fi

check: $(BUILD)/vega $(BUILD)/vega-surface.so check-text
	$(MAKE) -C $(AUP) build/aup
	$(MAKE) -C $(PURE) build/pure-serve build/clock-back.so
	python3 tests/check.py --vega $(BUILD)/vega --symbols $(BUILD)/vega-symbols --aup $(AUP)/build/aup --serve $(PURE)/build/pure-serve \
		--clock-back $(PURE)/build/clock-back.so
