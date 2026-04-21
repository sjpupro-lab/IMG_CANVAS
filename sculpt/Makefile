# Sculpt Engine — Phase 3 Makefile
#
# No external C deps per DESIGN.md §10. Pure C99 + libc.

CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter -Iinclude
LDFLAGS  ?=

BUILD    := build
SRC_DIR  := src
TEST_DIR := tests
TOOL_DIR := tools

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))

TESTS    := $(patsubst $(TEST_DIR)/%.c,$(BUILD)/%,$(wildcard $(TEST_DIR)/*.c))
TOOLS    := $(patsubst $(TOOL_DIR)/%.c,$(BUILD)/%,$(wildcard $(TOOL_DIR)/*.c))

.PHONY: all test train clean help

all: $(OBJS) $(TOOLS) $(TESTS)

help:
	@echo "targets:"
	@echo "  all     - build objects, tools, tests (default)"
	@echo "  test    - build + run all tests"
	@echo "  train   - build train_sculpt tool"
	@echo "  clean   - remove build/"

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Test binaries link against all objects (small codebase; fine for Phase 3).
$(BUILD)/test_%: $(TEST_DIR)/test_%.c $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/train_sculpt: $(TOOL_DIR)/train_sculpt.c $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

train: $(BUILD)/train_sculpt

test: $(TESTS)
	@set -e; for t in $(TESTS); do \
		echo "=== $$t ==="; \
		$$t; \
	done
	@echo "all tests passed"

clean:
	rm -rf $(BUILD)
