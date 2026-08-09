#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct limine_framebuffer;

bool framebuffer_init(struct limine_framebuffer *framebuffer);
void framebuffer_clear(uint8_t red, uint8_t green, uint8_t blue);
void framebuffer_put_pixel(size_t x, size_t y, uint8_t red, uint8_t green, uint8_t blue);
void framebuffer_fill_rect(size_t x, size_t y, size_t width, size_t height,
                           uint8_t red, uint8_t green, uint8_t blue);
void framebuffer_scroll_region_up(size_t top, size_t bottom, size_t pixels,
                                  uint8_t red, uint8_t green, uint8_t blue);
size_t framebuffer_width(void);
size_t framebuffer_height(void);
size_t framebuffer_pitch(void);
size_t framebuffer_size(void);
unsigned framebuffer_bpp(void);
uint64_t framebuffer_physical_address(void);
uint8_t framebuffer_red_mask_size(void);
uint8_t framebuffer_red_mask_shift(void);
uint8_t framebuffer_green_mask_size(void);
uint8_t framebuffer_green_mask_shift(void);
uint8_t framebuffer_blue_mask_size(void);
uint8_t framebuffer_blue_mask_shift(void);
