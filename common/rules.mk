# 各 examples/*/Makefile 复用的公共编译规则。
# 用法（在示例目录的 Makefile 中）：
#   ROOT := ../..
#   BINS := server client
#   include $(ROOT)/common/rules.mk

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -I$(ROOT)/common
LDFLAGS ?= $(shell pkg-config --libs librdmacm libibverbs)
CPPFLAGS?= $(shell pkg-config --cflags librdmacm libibverbs)

BIN_DIR := bin
TARGETS := $(addprefix $(BIN_DIR)/,$(BINS))

.PHONY: all clean
all: $(TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%: %.c $(ROOT)/common/rdma_common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)
