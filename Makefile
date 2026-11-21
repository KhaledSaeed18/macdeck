# MacDeck build. Requires the Android NDK (any recent version; r28 used) and Python 3.
#   make            build build/deck (tablet) and build/deck-preview (Mac)
#   make image      build build/macdeck-os-v1.img (needs MACDECK_STOCK_BOOT, see README)
#   make preview    render the deck UI with sample data to build/preview.png
#   make clean
NDK ?= $(firstword $(ANDROID_NDK_HOME) $(wildcard $(HOME)/Library/Android/sdk/ndk/*))
HOST_TAG ?= $(shell uname -s | tr A-Z a-z)-x86_64
TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin
CC := $(TOOLCHAIN)/armv7a-linux-androideabi21-clang
STRIP := $(TOOLCHAIN)/llvm-strip
CFLAGS := -static -O2 -Wall -Wextra

all: build/deck build/deck-preview

build/deck: os/deck/deck.c os/deck/font_spleen.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ios/deck -o build/deck.debug $<
	$(STRIP) -o $@ build/deck.debug

# Host-side UI preview (Mac cc, renders to an image instead of the framebuffer)
build/deck-preview: os/deck/preview.c os/deck/deck.c os/deck/font_spleen.h
	@mkdir -p build
	cc -O2 -Wall -Wno-unused-function -Ios/deck -o $@ os/deck/preview.c

preview: build/deck-preview
	build/deck-preview build/preview.ppm 0 && sips -s format png build/preview.ppm --out build/preview.png >/dev/null
	build/deck-preview build/preview-tablet.ppm 1 && sips -s format png build/preview-tablet.ppm --out build/preview-tablet.png >/dev/null

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
