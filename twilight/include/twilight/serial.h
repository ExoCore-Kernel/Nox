#pragma once

#include <stdbool.h>

bool serial_init(void);
void serial_write_char(char c);
void serial_write(const char *text);
