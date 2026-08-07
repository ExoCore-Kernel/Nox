#pragma once

#include <stddef.h>
#include <stdint.h>

void font_draw_string(const char *text, size_t x, size_t y,
                      uint8_t red, uint8_t green, uint8_t blue);
