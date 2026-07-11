# qnx_client -- native QNX Screen/EGL/GLES2 client.
#
# QNX-ONLY. This does not compile on Linux (no libscreen/libEGL/
# libGLESv2 there) -- that's expected and accepted per the redesign
# brief, not a bug to fix.
#
# Reuses ../src/net.c, ../src/map.c, ../src/net.h, ../src/map.h
# DIRECTLY via -I and by compiling them into this binary. Since this
# client is C -- like the server -- the compiler enforces the wire
# protocol structs match byte-for-byte. There is no hand-mirrored
# GDScript-style protocol here, and no risk of the two silently
# drifting apart the way net.h/Network.gd can.
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

# main.c and level_geo.c are qnx_client's own; net.c/map.c are
# compiled straight out of the top-level src/ tree so there's exactly
# one copy of the protocol and map logic in the whole project.
SRCS := src/main.c src/level_geo.c $(TOP_SRC)/net.c $(TOP_SRC)/map.c
OBJS := $(BUILD_DIR)/main.o $(BUILD_DIR)/level_geo.o $(BUILD_DIR)/net.o $(BUILD_DIR)/map.o

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

$(BUILD_DIR)/net.o: $(TOP_SRC)/net.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/map.o: $(TOP_SRC)/map.c
	@mkdir -p $(dir $@)
	$(QCC) $(CFLAGS) -c $< -o $@

# ---- Pi deployment -- uses the "qnxpi" ssh alias, same as the
# top-level Makefile. Update ~/.ssh/config when the Pi's IP changes,
# not here. ----
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
