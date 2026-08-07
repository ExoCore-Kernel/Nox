#pragma once

#include <stdbool.h>

bool ps2_keyboard_init(void);
void keyboard_irq_handler(void);
