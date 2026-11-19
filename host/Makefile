# Host build of the portable modules: no ESP-IDF, no radio, no board.
#
#   make            build everything into host/build/
#   make test       run the unit tests
#   make rigctl     run real Hamlib (rigctl) against the parser over a pty

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS = -I../components/cat_civ/include -I../components/rig_ic705/include -I../components/audio_dsp/include \
           -I../components/audio_engine/include

BUILD = build
CORE  = ../components/cat_civ/cat_civ.c ../components/rig_ic705/rig_ic705.c
HDRS  = ../components/cat_civ/include/cat_civ.h ../components/rig_ic705/include/rig_ic705.h
DSP   = ../components/audio_dsp/audio_dsp.c
DSP_H = ../components/audio_dsp/include/audio_dsp.h
ENG   = ../components/audio_engine/audio_engine.c
ENG_H = ../components/audio_engine/include/audio_engine.h

all: $(BUILD)/civ_pty $(BUILD)/test_civ $(BUILD)/test_dsp $(BUILD)/test_engine

$(BUILD)/civ_pty: civ_pty.c $(CORE) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ civ_pty.c $(CORE)

$(BUILD)/test_civ: test_civ.c $(CORE) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_civ.c $(CORE)

$(BUILD)/test_dsp: test_dsp.c $(DSP) $(DSP_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_dsp.c $(DSP) -lm

$(BUILD)/test_engine: test_engine.c $(ENG) $(ENG_H) $(DSP) $(DSP_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_engine.c $(ENG) $(DSP) -lm

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_civ $(BUILD)/test_dsp $(BUILD)/test_engine
	./$(BUILD)/test_civ
	./$(BUILD)/test_dsp
	./$(BUILD)/test_engine

rigctl: $(BUILD)/civ_pty
	./rigctl_check.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test rigctl clean
