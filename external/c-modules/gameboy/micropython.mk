#
# Game Boy emulator (Peanut-GB core); Python side is shared/gameboy.py
#
GB_DIR := $(USERMOD_DIR)

ifndef BOARD

# desktop simulator
SRC_USERMOD += $(GB_DIR)/modgameboy.c $(GB_DIR)/gb_core.c $(GB_DIR)/gb_video.c
SRC_USERMOD += $(GB_DIR)/hal_unix.c
CFLAGS_USERMOD += -DGB_HAL_UNIX

endif

ifeq ($(BOARD),COLDCARD_Q1)

# COLDCARD Q only; the Mk4 shares this c-modules directory but has no LCD
# or keyboard for it
SRC_USERMOD += $(GB_DIR)/modgameboy.c $(GB_DIR)/gb_core.c $(GB_DIR)/gb_video.c
SRC_USERMOD += $(GB_DIR)/hal_q1.c

# the emulator and scaler are the hot loop; the rest of the firmware is -Os
$(BUILD)/gameboy/gb_core.o $(BUILD)/gameboy/gb_video.o: COPT += -O2

endif
