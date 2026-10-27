# Host-native tests for oralb_proto. No ESP-IDF needed.
#
#   make        build and run
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wpedantic
CPPFLAGS += -I../include

BUILD := build
BIN   := $(BUILD)/test_oralb_proto
SRCS  := ../oralb_proto.c test_oralb_proto.c

.PHONY: all test clean

all: test

$(BIN): $(SRCS) ../include/oralb_proto.h
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS)

test: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD)
