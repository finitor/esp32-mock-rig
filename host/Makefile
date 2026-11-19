# Host build of the portable modules: no ESP-IDF, no radio, no board.
#
#   make            build everything into host/build/
#   make test       run the unit tests
#   make rigctl     run real Hamlib (rigctl) against the parser over a pty

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS = -I../components/cat_civ/include -I../components/rig_ic705/include

BUILD = build
CORE  = ../components/cat_civ/cat_civ.c ../components/rig_ic705/rig_ic705.c
HDRS  = ../components/cat_civ/include/cat_civ.h ../components/rig_ic705/include/rig_ic705.h

all: $(BUILD)/civ_pty $(BUILD)/test_civ

$(BUILD)/civ_pty: civ_pty.c $(CORE) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ civ_pty.c $(CORE) $(LDLIBS_PTY)

$(BUILD)/test_civ: test_civ.c $(CORE) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_civ.c $(CORE)

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_civ
	./$(BUILD)/test_civ

rigctl: $(BUILD)/civ_pty
	./rigctl_check.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test rigctl clean
