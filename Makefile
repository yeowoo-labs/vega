CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build
# Sibling checkouts: Pure runs the modules (libpure.a: the app model and the
# tree store, headers in core/), aup builds them.
PURE ?= ../pure
AUP ?= ../aup
LIBPURE = $(PURE)/build/libpure.a

SOURCES = src/screen.c src/draw.c src/width.c src/web.c src/main.c

.PHONY: all check FORCE
all: $(BUILD)/vega $(BUILD)/vega-symbols

$(BUILD):
	mkdir -p $@

$(LIBPURE): FORCE
	$(MAKE) -C $(PURE) build/libpure.a

$(BUILD)/vega: $(SOURCES) src/vega.h $(LIBPURE) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PURE)/core $(SOURCES) $(LIBPURE) $(LDFLAGS) -lm $(LDLIBS) -o $@

# Building symbol files reads fonts through FreeType; reading them does not.
FREETYPE_CFLAGS ?= $(shell pkg-config --cflags freetype2)
FREETYPE_LIBS ?= $(shell pkg-config --libs freetype2)
# Drawing them on the GPU takes OpenGL ES 3.1 through EGL.
GL_LIBS ?= $(shell pkg-config --libs egl glesv2)
$(BUILD)/vega-symbols: src/symbols-tool.c src/symbols.c src/symbols.h src/symbols-gl.c src/symbols-gl.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FREETYPE_CFLAGS) src/symbols-tool.c src/symbols.c src/symbols-gl.c $(LDFLAGS) \
		$(FREETYPE_LIBS) $(GL_LIBS) -lm $(LDLIBS) -o $@

check: $(BUILD)/vega $(BUILD)/vega-symbols
	$(MAKE) -C $(AUP) build/aup
	$(MAKE) -C $(PURE) build/pure-serve
	python3 tests/check.py --vega $(BUILD)/vega --symbols $(BUILD)/vega-symbols --aup $(AUP)/build/aup --serve $(PURE)/build/pure-serve
