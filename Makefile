CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build
# Sibling checkouts: Pure runs the modules, aup builds them.
PURE ?= ../pure/prototypes/connections
AUP ?= ../aup

PURE_SOURCES = $(PURE)/module.c $(PURE)/module-list.c $(PURE)/module-save.c $(PURE)/module-store.c \
               $(PURE)/module-text.c $(PURE)/store-tree.c
SOURCES = src/screen.c src/draw.c src/width.c src/web.c src/main.c

.PHONY: all check
all: $(BUILD)/vega $(BUILD)/vega-symbols

$(BUILD):
	mkdir -p $@

$(BUILD)/vega: $(SOURCES) src/vega.h $(PURE_SOURCES) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(PURE) $(SOURCES) $(PURE_SOURCES) $(LDFLAGS) -lm $(LDLIBS) -o $@

# Building symbol files reads fonts through FreeType; reading them does not.
FREETYPE_CFLAGS ?= $(shell pkg-config --cflags freetype2)
FREETYPE_LIBS ?= $(shell pkg-config --libs freetype2)
$(BUILD)/vega-symbols: src/symbols-tool.c src/symbols.c src/symbols.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FREETYPE_CFLAGS) src/symbols-tool.c src/symbols.c $(LDFLAGS) $(FREETYPE_LIBS) -lm $(LDLIBS) -o $@

check: $(BUILD)/vega $(BUILD)/vega-symbols
	$(MAKE) -C $(AUP) build/aup
	$(MAKE) -C $(PURE) build/pure-serve
	python3 tests/check.py --vega $(BUILD)/vega --symbols $(BUILD)/vega-symbols --aup $(AUP)/build/aup --serve $(PURE)/build/pure-serve
