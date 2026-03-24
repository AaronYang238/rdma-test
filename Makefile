CC := gcc
CFLAGS := -O2 -Wall -Wextra -std=c11
LDFLAGS := $(shell pkg-config --libs librdmacm libibverbs)
CPPFLAGS := $(shell pkg-config --cflags librdmacm libibverbs)

SRC_DIR := src
BIN_DIR := bin

TARGETS := $(BIN_DIR)/rdma_server $(BIN_DIR)/rdma_client

.PHONY: all clean run-local

all: $(TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/rdma_server: $(SRC_DIR)/server.c $(SRC_DIR)/common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

$(BIN_DIR)/rdma_client: $(SRC_DIR)/client.c $(SRC_DIR)/common.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)

run-local: all
	@echo "Terminal #1: ./bin/rdma_server 127.0.0.1 7471"
	@echo "Terminal #2: ./bin/rdma_client 127.0.0.1 7471"
