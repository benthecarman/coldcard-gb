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

# Q1 hardware build: added with hal_q1.c
