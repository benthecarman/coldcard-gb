//
// hal_test.h - test runner side of hal_test.c
//
#pragma once

#include <stdint.h>
#include "gb_hal.h"

extern uint16_t test_screen[HAL_LCD_H][HAL_LCD_W];
extern uint64_t test_keys;
extern bool test_keys_at_start;     // keys held as a run starts count
extern int test_lcd_writes;

void test_fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
