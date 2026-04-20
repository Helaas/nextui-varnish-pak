SHELL := /bin/bash

CROSS_COMPILE ?= aarch64-nextui-linux-gnu-
CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
SRC := $(shell find /workspace/src -name '*.c' ! -name 'preload.c' -print | sort)
INCLUDES := -I/workspace/third_party/apostrophe/include -I/workspace/src
PKG_CONFIG := pkg-config
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LDFLAGS := $(shell $(PKG_CONFIG) --libs sdl2) -lSDL2_ttf -lSDL2_image

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
	-mcpu=cortex-a53 -mtune=cortex-a53 \
	-DPLATFORM_TG5040 \
	-ffunction-sections -fdata-sections -fstack-protector-strong \
	$(INCLUDES) $(SDL_CFLAGS)

LDFLAGS := $(SDL_LDFLAGS) -Wl,--gc-sections -lpthread -lm

all: $(BUILD_DIR)/varnish $(BUILD_DIR)/varnish_overlay.so

$(BUILD_DIR)/varnish: $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
	$(STRIP) $@

$(BUILD_DIR)/varnish_overlay.so: /workspace/src/preload.c /workspace/src/varnish_shm.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
		-mcpu=cortex-a53 -mtune=cortex-a53 \
		-DPLATFORM_TG5040 \
		-fPIC -shared \
		-I/workspace/src \
		-o $@ /workspace/src/preload.c -ldl
	$(STRIP) $@
