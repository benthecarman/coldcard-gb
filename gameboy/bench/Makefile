# Instruction counts for the Game Boy core on a Cortex-M4 (QEMU mps2-an386).
#
#   make run ROM=../roms/tobu.gb           # firmware flags (-O2)
#   make run ROM=../roms/tobu.gb OPT=-Os
#   make profile                           # where the instructions go
#
CORE    = ../../external/c-modules/gameboy
ROM    ?= ../roms/tobu.gb
OPT    ?= -O2
FRAMES ?= 1800
BUILD   = build$(OPT)

CC      = arm-none-eabi-gcc
# same CPU flags as the stm32 port's l4 build
CFLAGS  = -mthumb -mcpu=cortex-m4 -mtune=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
          $(OPT) -g -Wall -I$(CORE) -ffunction-sections -fdata-sections \
          -DBENCH_FRAMES=$(FRAMES)
LDFLAGS = -nostartfiles -T link.ld -Wl,--gc-sections --specs=nano.specs

SRCS    = bench.c $(CORE)/gb_core.c $(CORE)/gb_video.c $(CORE)/gb_oled.c

# the firmware's memcpy/memset/memcmp (MicroPython's, built -O2), not newlib's
STRING0 = ../../external/micropython/lib/libc/string0.c

$(BUILD)/bench.elf: $(SRCS) rom.S link.ld $(wildcard $(CORE)/*.h) FORCE
	@mkdir -p $(BUILD)
	$(CC) $(filter-out $(OPT),$(CFLAGS)) -O2 -ffreestanding -fno-tree-loop-distribute-patterns \
	    -c -o $(BUILD)/string0.o $(STRING0)
	$(CC) $(CFLAGS) $(EXTRA) $(LDFLAGS) -DROM_PATH='"$(abspath $(ROM))"' -o $@ \
	    $(SRCS) rom.S $(BUILD)/string0.o

# sampling profile (see profile.py)
profile:
	$(MAKE) BUILD=build-prof EXTRA=-DPROFILE build-prof/bench.elf
	$(QEMU) -kernel build-prof/bench.elf 2>&1 | ./profile.py build-prof/bench.elf

# -icount shift=0: virtual time advances 1 ns per instruction
QEMU    = qemu-system-arm -M mps2-an386 -nographic -monitor none -serial none \
	    -icount shift=0,align=off,sleep=off -semihosting-config enable=on,target=native

run: $(BUILD)/bench.elf
	$(QEMU) -kernel $< 2>&1

clean:
	rm -rf build-*

FORCE:
.PHONY: run profile clean FORCE
