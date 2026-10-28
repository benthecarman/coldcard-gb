#
# Game Boy emulator (Peanut-GB core); Python side is shared/gameboy.py
#
GB_DIR := $(USERMOD_DIR)

GB_CORE_SRC = $(GB_DIR)/modgameboy.c $(GB_DIR)/gb_core.c $(GB_DIR)/gb_video.c $(GB_DIR)/gb_oled.c

ifndef BOARD

# desktop simulator
SRC_USERMOD += $(GB_CORE_SRC)
SRC_USERMOD += $(GB_DIR)/hal_unix.c
CFLAGS_USERMOD += -DGB_HAL_UNIX

endif

ifeq ($(BOARD),COLDCARD_Q1)

SRC_USERMOD += $(GB_CORE_SRC)
SRC_USERMOD += $(GB_DIR)/hal_q1.c $(GB_DIR)/hal_stm32.c

endif

ifeq ($(BOARD),COLDCARD_MK4)

# The Mk4 firmware also runs on the Mk5, but Python only offers the game
# on the Mk4 (see shared/flow.py)
SRC_USERMOD += $(GB_CORE_SRC)
SRC_USERMOD += $(GB_DIR)/hal_mk4.c $(GB_DIR)/hal_stm32.c

endif

ifdef BOARD

# the emulator and scalers are the hot loop; the rest of the firmware is -Os
$(BUILD)/gameboy/gb_core.o $(BUILD)/gameboy/gb_video.o $(BUILD)/gameboy/gb_oled.o: COPT += -O2

endif
