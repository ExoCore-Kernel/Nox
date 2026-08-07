#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool font_init(const void *data, size_t size);
void font_draw_string(const char *text, size_t x, size_t y,
                      uint8_t red, uint8_t green, uint8_t blue);
