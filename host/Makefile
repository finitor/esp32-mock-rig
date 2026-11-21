# Host build of the portable modules: no ESP-IDF, no radio, no board.
#
#   make            build everything into host/build/
#   make test       run the unit tests
#   make rigctl     run real Hamlib (rigctl) against each personality over a pty
#   make mockctl    run tools/mockctl.py against the mock's own controls over a pty
#   make console    run the web console's API through tools/console_sim.py and check it
#   make console-sim  serve the web console on http://localhost:8080 with a simulated radio
#   make radio-sim    the same with no pretend transmissions, to drive the radio from Hamlib:
#                     rigctl -m 3085 -r host/build/cat-a

CC      ?= cc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS = -I../components/cat_civ/include -I../components/rig_ic705/include -I../components/rig_g90/include -I../components/audio_dsp/include \
           -I../components/audio_engine/include -I../components/mock_ctl/include -I../components/ui/include \
           -I../components/web_console/include -I../components/link/include \
           -I../components/clips/include -I../components/coord/include

BUILD = build
CORE  = ../components/cat_civ/cat_civ.c ../components/rig_ic705/rig_ic705.c ../components/rig_g90/rig_g90.c
HDRS  = ../components/cat_civ/include/cat_civ.h ../components/rig_ic705/include/rig_ic705.h \
        ../components/rig_g90/include/rig_g90.h
DSP   = ../components/audio_dsp/audio_dsp.c
DSP_H = ../components/audio_dsp/include/audio_dsp.h
ENG   = ../components/audio_engine/audio_engine.c
ENG_H = ../components/audio_engine/include/audio_engine.h
CTL   = ../components/mock_ctl/mock_ctl.c
CTL_H = ../components/mock_ctl/include/mock_ctl.h
UI    = ../components/ui/ui_led.c
UI_H  = ../components/ui/include/ui_led.h
CON   = ../components/web_console/console_api.c
CON_H = ../components/web_console/include/console_api.h
LINK   = ../components/link/link.c
LINK_H = ../components/link/include/link.h
CLIPS   = ../components/clips/clip_store.c
CLIPS_H = ../components/clips/include/clip_store.h
COORD   = ../components/coord/coord.c
COORD_H = ../components/coord/include/coord.h
# everything the mock's own controls are built from
CTL_ALL   = $(CORE) $(CTL) $(ENG) $(DSP)
CTL_ALL_H = $(HDRS) $(CTL_H) $(ENG_H) $(DSP_H)

all: $(BUILD)/civ_pty $(BUILD)/test_civ $(BUILD)/test_dsp $(BUILD)/test_engine $(BUILD)/test_ctl $(BUILD)/test_ui \
     $(BUILD)/test_console $(BUILD)/test_link $(BUILD)/test_clips $(BUILD)/test_coord \
     $(BUILD)/console_sim

$(BUILD)/civ_pty: civ_pty.c $(CTL_ALL) $(CTL_ALL_H) $(CLIPS) $(CLIPS_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ civ_pty.c $(CTL_ALL) $(CLIPS) -lm

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

$(BUILD)/test_console: test_console.c $(CON) $(CON_H) $(LINK_H) $(CTL_ALL) $(CTL_ALL_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_console.c $(CON) $(CTL_ALL) -lm

$(BUILD)/test_link: test_link.c $(LINK) $(LINK_H) $(DSP) $(DSP_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_link.c $(LINK) $(DSP) -lm

$(BUILD)/test_clips: test_clips.c $(CLIPS) $(CLIPS_H) $(ENG) $(ENG_H) $(DSP) $(DSP_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_clips.c $(CLIPS) $(ENG) $(DSP) -lm

$(BUILD)/test_coord: test_coord.c $(COORD) $(COORD_H) $(LINK) $(LINK_H) $(CTL_ALL) $(CTL_ALL_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_coord.c $(COORD) $(LINK) $(CTL_ALL) -lm

$(BUILD)/console_sim: console_sim.c $(CON) $(CON_H) $(LINK) $(LINK_H) $(CLIPS) $(CLIPS_H) $(COORD) $(COORD_H) $(CTL_ALL) $(CTL_ALL_H) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ console_sim.c $(CON) $(LINK) $(CLIPS) $(COORD) $(CTL_ALL) -lm

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test_civ $(BUILD)/test_dsp $(BUILD)/test_engine $(BUILD)/test_ctl $(BUILD)/test_ui \
      $(BUILD)/test_console $(BUILD)/test_link $(BUILD)/test_clips $(BUILD)/test_coord
	./$(BUILD)/test_civ
	./$(BUILD)/test_dsp
	./$(BUILD)/test_engine
	./$(BUILD)/test_ctl
	./$(BUILD)/test_ui
	./$(BUILD)/test_console
	./$(BUILD)/test_link
	./$(BUILD)/test_clips
	./$(BUILD)/test_coord

rigctl: $(BUILD)/civ_pty
	./rigctl_check.sh
	MODEL=3088 ./rigctl_check.sh

mockctl: $(BUILD)/civ_pty
	./mockctl_check.sh

console: $(BUILD)/console_sim
	python3 tools/console_sim.py --check

console-sim: $(BUILD)/console_sim
	python3 tools/console_sim.py

radio-sim: $(BUILD)/console_sim
	python3 tools/console_sim.py --quiet

clean:
	rm -rf $(BUILD)

.PHONY: all test rigctl mockctl console console-sim radio-sim clean
