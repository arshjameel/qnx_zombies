# qnx_client -- native QNX Screen/EGL/GLES2 client.
#
# QNX-ONLY. This does not compile on Linux (no libscreen/libEGL/
# libGLESv2 there)
#
# Reuses ../src/net.c, ../src/map.c, ../src/net.h, ../src/map.h
# via -I and by compiling them into this binary.
#
# Targets:
#   make                       Stage 1 spike (window/input/net proof)
#   make deploy                cross-compile + scp to the Pi
#   make run                   deploy + run over ssh
#   make clean / rebuild

BUILD_PROFILE ?= debug
TOP_SRC := ../src
BUILD_DIR := build/$(BUILD_PROFILE)

QCC        := qcc
QCC_TARGET := -Vgcc_ntoaarch64le
QNX_DEFS   := -D__EXT_POSIX1_199309 -D__EXT_XOPEN_EX -D__EXT

CFLAGS  := -std=c99 -Wall -Wextra -Isrc -I$(TOP_SRC) -MMD $(QNX_DEFS) $(QCC_TARGET)
LDFLAGS := -lscreen -lEGL -lGLESv2 -lsocket -lm $(QCC_TARGET)

ifeq ($(BUILD_PROFILE),release)
    CFLAGS += -O2 -DNDEBUG
else
    CFLAGS += -g -O0 -fno-builtin
endif

SRCS := src/main.c src/level_geo.c src/hud_render.c src/text_render.c $(TOP_SRC)/net.c $(TOP_SRC)/map.c
OBJS := $(BUILD_DIR)/main.o $(BUILD_DIR)/level_geo.o $(BUILD_DIR)/hud_render.o $(BUILD_DIR)/text_render.o \
        $(BUILD_DIR)/net.o $(BUILD_DIR)/map.o

.PHONY: all clean rebuild deploy run

all: $(BUILD_DIR)/qnx_client

$(BUILD_DIR)/qnx_client: $(OBJS)
	@mkdir -p $(dir $@)
	$(QCC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/main.o: src/main.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/level_geo.o: src/level_geo.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hud_render.o: src/hud_render.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/text_render.o: src/text_render.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/net.o: $(TOP_SRC)/net.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/map.o: $(TOP_SRC)/map.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

# ---- Pi deployment ----
PI_USER := qnxuser
PI_HOST := qnxpi
PI_DIR  := /data/home/qnxuser/game

deploy: all
	ssh $(PI_USER)@$(PI_HOST) "mkdir -p $(PI_DIR)"
	scp $(BUILD_DIR)/qnx_client $(PI_USER)@$(PI_HOST):$(PI_DIR)/qnx_client

run: deploy
	ssh $(PI_USER)@$(PI_HOST) "cd $(PI_DIR) && ./qnx_client"

clean:
	rm -rf build

rebuild: clean all

-include $(OBJS:.o=.d)
