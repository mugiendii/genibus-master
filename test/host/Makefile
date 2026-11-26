# Host unit tests for lib/genibus (portable C99).
#   make            build + run
#   make asan       build + run with AddressSanitizer / UBSan
#   make clean

CC      ?= cc
LIB     := ../../lib/genibus
SRCS    := test_genibus.c $(LIB)/genibus.c $(LIB)/geni_units.c $(LIB)/geni_master.c $(LIB)/mp204.c
CFLAGS  ?= -O1 -g
CFLAGS  += -std=c99 -Wall -Wextra -Werror -pedantic -I$(LIB)
LDLIBS  := -lm
BUILD   := build

.PHONY: all test asan clean
all: test

$(BUILD)/test_genibus: $(SRCS) $(wildcard $(LIB)/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(BUILD)/test_genibus_asan: $(SRCS) $(wildcard $(LIB)/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
	    -o $@ $(SRCS) $(LDLIBS)

test: $(BUILD)/test_genibus
	./$(BUILD)/test_genibus

asan: $(BUILD)/test_genibus_asan
	./$(BUILD)/test_genibus_asan

clean:
	rm -rf $(BUILD)
