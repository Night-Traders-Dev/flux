CC       ?= cc
CFLAGS   ?= -Wall -Wextra -Wpedantic -std=c99 -O2 -g
AR       ?= ar
RANLIB   ?= ranlib

SRC_DIR  := src
BUILD_DIR := build
TEST_DIR := testing

SRCS     := $(SRC_DIR)/flux_arena.c $(SRC_DIR)/flux_json.c \
            $(SRC_DIR)/flux_ir.c $(SRC_DIR)/flux_asm.c \
            $(SRC_DIR)/flux_val.c $(SRC_DIR)/flux_exec.c
OBJS     := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LIB      := $(BUILD_DIR)/libflux.a
TEST_SRC := $(TEST_DIR)/test_flux.c
TEST_BIN := $(BUILD_DIR)/test_flux
MAIN_SRC := $(TEST_DIR)/main.c
MAIN_BIN := $(BUILD_DIR)/flux

.PHONY: all clean lib test check run

all: lib $(TEST_BIN) $(MAIN_BIN)

lib: $(LIB)

# Pull in dependency info from previous builds
-include $(OBJS:.o=.d)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -MMD -MP -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_SRC) $(LIB)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $< -L$(BUILD_DIR) -lflux -lm -o $@

$(MAIN_BIN): $(MAIN_SRC) $(LIB)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $< -L$(BUILD_DIR) -lflux -lm -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

run: $(MAIN_BIN)
	./$(MAIN_BIN)

check: test

clean:
	rm -rf $(BUILD_DIR)

# Individual object targets for parallel builds
$(BUILD_DIR)/flux_arena.o: $(SRC_DIR)/flux_arena.c $(SRC_DIR)/flux_internal.h $(SRC_DIR)/flux.h
$(BUILD_DIR)/flux_json.o: $(SRC_DIR)/flux_json.c $(SRC_DIR)/flux_internal.h $(SRC_DIR)/flux.h
$(BUILD_DIR)/flux_ir.o: $(SRC_DIR)/flux_ir.c $(SRC_DIR)/flux_internal.h $(SRC_DIR)/flux.h
$(BUILD_DIR)/flux_asm.o: $(SRC_DIR)/flux_asm.c $(SRC_DIR)/flux_internal.h $(SRC_DIR)/flux.h
$(BUILD_DIR)/flux_val.o: $(SRC_DIR)/flux_val.c $(SRC_DIR)/flux_internal.h $(SRC_DIR)/flux.h
$(BUILD_DIR)/flux_exec.o: $(SRC_DIR)/flux_exec.c $(SRC_DIR)/flux_internal.h $(SRC_DIR)/flux.h
