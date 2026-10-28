# Headless test runner for the Game Boy core (host build).
#
#   make            build build/gbtest
#   make test       fetch test ROMs, run cpu_instrs, instr_timing, dmg-acid2
#
CORE    = ../../external/c-modules/gameboy
ROMS    = ../roms
BUILD   = build

CFLAGS  += -O2 -g -Wall -Wextra -I$(CORE) -I. $(shell pkg-config --cflags zlib)
LDLIBS  += $(shell pkg-config --libs zlib)

SRCS    = gbtest.c hal_test.c $(CORE)/gb_core.c $(CORE)/gb_video.c
HDRS    = $(wildcard $(CORE)/*.h) hal_test.h

all: $(BUILD)/gbtest

$(BUILD)/gbtest: $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

roms:
	../scripts/fetch-test-roms.sh

test: $(BUILD)/gbtest roms
	$(BUILD)/gbtest serial $(ROMS)/cpu_instrs.gb 10000
	$(BUILD)/gbtest -s 1 serial $(ROMS)/cpu_instrs.gb 10000
	$(BUILD)/gbtest serial $(ROMS)/instr_timing.gb 1000
	$(BUILD)/gbtest shot $(ROMS)/dmg-acid2.gb 120 1x $(BUILD)/acid2-1x.png
	python3 -I compare_png.py $(BUILD)/acid2-1x.png $(ROMS)/dmg-acid2-reference.png 80 48
	@echo "ALL TESTS PASSED"

clean:
	rm -rf $(BUILD)

.PHONY: all roms test clean
