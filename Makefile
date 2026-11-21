# MacDeck build. Requires the Android NDK (any recent version; r28 used) and Python 3.
#   make            build build/deck0
#   make image      build build/macdeck-os-v0.img (needs MACDECK_STOCK_BOOT, see README)
#   make clean
NDK ?= $(firstword $(ANDROID_NDK_HOME) $(wildcard $(HOME)/Library/Android/sdk/ndk/*))
HOST_TAG ?= $(shell uname -s | tr A-Z a-z)-x86_64
TOOLCHAIN := $(NDK)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin
CC := $(TOOLCHAIN)/armv7a-linux-androideabi21-clang
STRIP := $(TOOLCHAIN)/llvm-strip
CFLAGS := -static -O2 -Wall -Wextra

all: build/deck0

build/deck0: os/deck0/deck0.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/deck0.debug $<
	$(STRIP) -o $@ build/deck0.debug

image: build/deck0
	python3 scripts/build-os-image.py build/deck0

spikes: build/fbtouch
build/fbtouch: spikes/fbtouch/fbtouch.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@.debug $<
	$(STRIP) -o $@ $@.debug

clean:
	rm -rf build

.PHONY: all image spikes clean
