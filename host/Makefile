# Host build of the portable modules: no ESP-IDF, no radio, no board.
#
#   make            build everything into host/build/
#   make test       run the unit tests
#   make rigctl     run real Hamlib (rigctl) against the parser over a pty
#   make mockctl    run tools/mockctl.py against the mock's own controls over a pty

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS = -I../components/cat_civ/include -I../components/rig_ic705/include -I../components/audio_dsp/include \
           -I../components/audio_engine/include -I../components/mock_ctl/include -I../components/ui/include

BUILD = build
CORE  = ../components/cat_civ/cat_civ.c ../components/rig_ic705/rig_ic705.c
HDRS  = ../components/cat_civ/include/cat_civ.h ../components/rig_ic705/include/rig_ic705.h
DSP   = ../components/audio_dsp/audio_dsp.c
DSP_H = ../components/audio_dsp/include/audio_dsp.h
ENG   = ../components/audio_engine/audio_engine.c
ENG_H = ../components/audio_engine/include/audio_engine.h
CTL   = ../components/mock_ctl/mock_ctl.c
CTL_H = ../components/mock_ctl/include/mock_ctl.h
UI    = ../components/ui/ui_led.c
UI_H  = ../components/ui/include/ui_led.h
# everything the mock's own controls are built from
CTL_ALL   = $(CORE) $(CTL) $(ENG) $(DSP)
CTL_ALL_H = $(HDRS) $(CTL_H) $(ENG_H) $(DSP_H)

all: $(BUILD)/civ_pty $(BUILD)/test_civ $(BUILD)/test_dsp $(BUILD)/test_engine $(BUILD)/test_ctl $(BUILD)/test_ui

$(BUILD)/civ_pty: civ_pty.c $(CTL_ALL) $(CTL_ALL_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ civ_pty.c $(CTL_ALL) -lm

$(BUILD)/test_civ: test_civ.c $(CORE) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_civ.c $(CORE)

$(BUILD)/test_dsp: test_dsp.c $(DSP) $(DSP_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_dsp.c $(DSP) -lm

$(BUILD)/test_engine: test_engine.c $(ENG) $(ENG_H) $(DSP) $(DSP_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_engine.c $(ENG) $(DSP) -lm

$(BUILD)/test_ctl: test_ctl.c $(CTL_ALL) $(CTL_ALL_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_ctl.c $(CTL_ALL) -lm

$(BUILD)/test_ui: test_ui.c $(UI) $(UI_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_ui.c $(UI)

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_civ $(BUILD)/test_dsp $(BUILD)/test_engine $(BUILD)/test_ctl $(BUILD)/test_ui
	./$(BUILD)/test_civ
	./$(BUILD)/test_dsp
	./$(BUILD)/test_engine
	./$(BUILD)/test_ctl
	./$(BUILD)/test_ui

rigctl: $(BUILD)/civ_pty
	./rigctl_check.sh

mockctl: $(BUILD)/civ_pty
	./mockctl_check.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test rigctl mockctl clean
