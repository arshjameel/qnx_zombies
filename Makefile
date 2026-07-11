# qnx-game -- server-only Makefile
#
# Targets:
#   make server              Linux headless server (local testing)
#   make qnx-server          QNX AArch64 server (cross-compile)
#   make deploy-server       cross-compile + scp to Pi
#   make run-server          deploy + start server over ssh
#   make clean
#   make rebuild
#
# Build profiles:
#   make server BUILD_PROFILE=release   -O2 -DNDEBUG
#   make server BUILD_PROFILE=debug     -g -O0 -fno-builtin (default)

BUILD_PROFILE ?= debug

SRC_DIR   := src
BUILD_DIR := build/$(BUILD_PROFILE)

SERVER_SRCS := $(SRC_DIR)/server.c $(SRC_DIR)/map.c $(SRC_DIR)/net.c
SERVER_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/linux/%.o,$(SERVER_SRCS))
SERVER_QNX_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/qnx/%.o,$(SERVER_SRCS))

CC       := gcc
CFLAGS   := -std=c99 -Wall -Wextra -Isrc -MMD
LDFLAGS  := -lm

QCC      := qcc
QCC_TARGET := -Vgcc_ntoaarch64le
QNX_DEFS := -D__EXT_POSIX1_199309 -D__EXT_XOPEN_EX -D__EXT
QNX_CFLAGS := -std=c99 -Wall -Wextra -Isrc -MMD $(QNX_DEFS) $(QCC_TARGET)
QNX_LDFLAGS := -lm -lsocket $(QCC_TARGET)

ifeq ($(BUILD_PROFILE),release)
    CFLAGS     += -O2 -DNDEBUG
    QNX_CFLAGS += -O2 -DNDEBUG
else
    CFLAGS     += -g -O0 -fno-builtin
    QNX_CFLAGS += -g -O0 -fno-builtin
endif

# ---- Pi deployment settings ----
# Uses the "qnxpi" alias from ~/.ssh/config
PI_USER := qnxuser
PI_HOST := qnxpi
PI_DIR  := /data/home/qnxuser/game

.PHONY: all server qnx-server deploy-server run-server clean rebuild

all: server

# ---- Linux server (local testing) ----
server: $(BUILD_DIR)/server_linux

$(BUILD_DIR)/server_linux: $(SERVER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(SERVER_OBJS) -o $@ $(LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/linux/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- QNX AArch64 server (cross-compile, requires sourced qnxsdp-env.sh) ----
qnx-server: $(BUILD_DIR)/server_qnx

$(BUILD_DIR)/server_qnx: $(SERVER_QNX_OBJS)
	@mkdir -p $(dir $@)
	$(QCC) $(SERVER_QNX_OBJS) -o $@ $(QNX_LDFLAGS)
	@echo "Built $@"

$(BUILD_DIR)/qnx/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(QCC) $(QNX_CFLAGS) -c $< -o $@

deploy-server: qnx-server
	ssh $(PI_USER)@$(PI_HOST) "mkdir -p $(PI_DIR)"
	scp $(BUILD_DIR)/server_qnx $(PI_USER)@$(PI_HOST):$(PI_DIR)/server_qnx

run-server: deploy-server
	ssh $(PI_USER)@$(PI_HOST) "cd $(PI_DIR) && ./server_qnx"

clean:
	rm -rf build

rebuild: clean all

-include $(SERVER_OBJS:.o=.d)
-include $(SERVER_QNX_OBJS:.o=.d)
