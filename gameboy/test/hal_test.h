//
// hal_test.h - test runner side of hal_test.c
//
#pragma once

#include <stdint.h>
#include "gb_hal.h"

extern uint16_t test_screen[HAL_LCD_H][HAL_LCD_W];
extern uint64_t test_keys;
// if set, hal_keys() returns these in turn (the last one from then on)
extern const uint64_t *test_key_seq;
extern int test_key_len;
extern int test_lcd_writes;

void test_fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
