#include <limine.h>
#include <twilight/framebuffer.h>

static struct limine_framebuffer *fb;

static uint32_t scale_component(uint8_t value, uint8_t bits) {
    if (bits == 0) {
        return 0;
    }
    if (bits >= 8) {
        return (uint32_t)value << (bits - 8);
    }
    return (uint32_t)value >> (8 - bits);
}

static uint32_t pack_rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return (scale_component(red, fb->red_mask_size) << fb->red_mask_shift)
         | (scale_component(green, fb->green_mask_size) << fb->green_mask_shift)
         | (scale_component(blue, fb->blue_mask_size) << fb->blue_mask_shift);
}

bool framebuffer_init(struct limine_framebuffer *framebuffer) {
    if (framebuffer == NULL || framebuffer->address == NULL || framebuffer->bpp != 32) {
        return false;
    }
    fb = framebuffer;
    return true;
}

void framebuffer_put_pixel(size_t x, size_t y, uint8_t red, uint8_t green, uint8_t blue) {
    if (fb == NULL || x >= fb->width || y >= fb->height) {
        return;
    }

    volatile uint32_t *row = (volatile uint32_t *)((uintptr_t)fb->address + y * fb->pitch);
    row[x] = pack_rgb(red, green, blue);
}

void framebuffer_fill_rect(size_t x, size_t y, size_t width, size_t height,
                           uint8_t red, uint8_t green, uint8_t blue) {
    if (fb == NULL || x >= fb->width || y >= fb->height) {
        return;
    }

    size_t end_x = x + width;
    size_t end_y = y + height;
    if (end_x > fb->width) end_x = fb->width;
    if (end_y > fb->height) end_y = fb->height;

    const uint32_t colour = pack_rgb(red, green, blue);
    for (size_t py = y; py < end_y; ++py) {
        volatile uint32_t *row = (volatile uint32_t *)((uintptr_t)fb->address + py * fb->pitch);
        for (size_t px = x; px < end_x; ++px) {
            row[px] = colour;
        }
    }
}

void framebuffer_clear(uint8_t red, uint8_t green, uint8_t blue) {
    if (fb == NULL) {
        return;
    }
    framebuffer_fill_rect(0, 0, fb->width, fb->height, red, green, blue);
}

size_t framebuffer_width(void) {
    return fb == NULL ? 0 : fb->width;
}

size_t framebuffer_height(void) {
    return fb == NULL ? 0 : fb->height;
}
