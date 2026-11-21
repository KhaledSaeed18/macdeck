# MacDeck build. Requires the Android NDK (any recent version; r28 used) and Python 3.
#   make            build build/deck (tablet) and build/deck-preview (Mac)
#   make image      build build/macdeck-os-v$MACDECK_VERSION.img (needs MACDECK_STOCK_BOOT, see README)
#   make preview    render every deck page with sample data to build/preview-<page>.png,
#                   plus the page picker open over Agents (build/preview-picker.png)
#   make clean
NDK ?= $(firstword $(ANDROID_NDK_HOME) $(wildcard $(HOME)/Library/Android/sdk/ndk/*))
HOST_TAG ?= $(shell uname -s | tr A-Z a-z)-x86_64
TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin
CC := $(TOOLCHAIN)/armv7a-linux-androideabi21-clang
STRIP := $(TOOLCHAIN)/llvm-strip
CFLAGS := -static -O2 -Wall -Wextra

DECK_SRC := $(addprefix os/deck/,util.c gfx.c model.c link.c power.c pages.c \
             page_home.c page_agents.c page_stats.c page_shortcuts.c page_mac.c page_tablet.c)
DECK_HDR := $(wildcard os/deck/*.h)

all: build/deck build/deck-preview

build/deck: $(DECK_SRC) os/deck/input.c os/deck/main.c $(DECK_HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -Ios/deck -o build/deck.debug $(DECK_SRC) os/deck/input.c os/deck/main.c
	$(STRIP) -o $@ build/deck.debug

# Host-side UI preview (Mac cc): the same modules minus input/main, rendering to an image
build/deck-preview: $(DECK_SRC) os/deck/preview.c $(DECK_HDR)
	@mkdir -p build
	cc -O2 -Wall -Wextra -Ios/deck -o $@ $(DECK_SRC) os/deck/preview.c

PAGES := home agents stats shortcuts mac tablet
preview: build/deck-preview
	@i=0; for p in $(PAGES); do \
	  build/deck-preview build/preview-$$p.ppm $$i >/dev/null && \
	  sips -s format png build/preview-$$p.ppm --out build/preview-$$p.png >/dev/null && echo build/preview-$$p.png; \
	  i=$$((i+1)); done
	@MACDECK_PICKER=1 build/deck-preview build/preview-picker.ppm 1 >/dev/null && \
	  sips -s format png build/preview-picker.ppm --out build/preview-picker.png >/dev/null && echo build/preview-picker.png

# Regenerate the font header from the Spleen BDF sources (third_party/spleen)
font:
	python3 scripts/bdf2c.py os/deck/font_spleen.h font_s=third_party/spleen/spleen-12x24.bdf \
	  font_m=third_party/spleen/spleen-16x32.bdf font_l=third_party/spleen/spleen-32x64.bdf

image: build/deck
	python3 scripts/build-os-image.py build/deck

spikes: build/fbtouch
build/fbtouch: spikes/fbtouch/fbtouch.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@.debug $<
	$(STRIP) -o $@ $@.debug

clean:
	rm -rf build

.PHONY: all image preview font spikes clean
