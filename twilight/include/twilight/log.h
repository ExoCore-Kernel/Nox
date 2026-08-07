#pragma once

#include <stddef.h>
#include <stdint.h>

void klog_init(void);
void klog_enable_uptime(void);
void klog(const char *message);
void klog_color(const char *message, uint8_t red, uint8_t green, uint8_t blue);
void klog_parts(const char *const *parts, size_t count);
