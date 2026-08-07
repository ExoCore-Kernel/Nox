#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/font.h>
#include <twilight/framebuffer.h>

#define PSF1_MAGIC0 0x36u
#define PSF1_MAGIC1 0x04u
#define PSF2_MAGIC 0x864ab572u

struct psf2_header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t glyph_count;
    uint32_t glyph_size;
    uint32_t height;
    uint32_t width;
};

static const uint8_t *glyph_data;
static size_t glyph_count;
static size_t glyph_size;
static size_t glyph_width;
static size_t glyph_height;
static size_t bytes_per_row;
static bool ready;

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

bool font_init(const void *data, size_t size) {
    ready = false;
    if (data == NULL || size < 4) {
        return false;
    }

    const uint8_t *bytes = data;

    if (bytes[0] == PSF1_MAGIC0 && bytes[1] == PSF1_MAGIC1) {
        const uint8_t mode = bytes[2];
        const uint8_t char_size = bytes[3];
        glyph_count = (mode & 0x01u) ? 512u : 256u;
        glyph_size = char_size;
        glyph_width = 8u;
        glyph_height = char_size;
        bytes_per_row = 1u;
        if (size < 4u + glyph_count * glyph_size) {
            return false;
        }
        glyph_data = bytes + 4u;
        ready = true;
        return true;
    }

    if (size >= sizeof(struct psf2_header) && read_u32_le(bytes) == PSF2_MAGIC) {
        const uint32_t header_size = read_u32_le(bytes + 8);
        const uint32_t count = read_u32_le(bytes + 16);
        const uint32_t char_size = read_u32_le(bytes + 20);
        const uint32_t height = read_u32_le(bytes + 24);
        const uint32_t width = read_u32_le(bytes + 28);

        if (header_size < sizeof(struct psf2_header) || count == 0 || char_size == 0 || width == 0 || height == 0) {
            return false;
        }
        if ((size_t)header_size + (size_t)count * char_size > size) {
            return false;
        }

        glyph_count = count;
        glyph_size = char_size;
        glyph_width = width;
        glyph_height = height;
        bytes_per_row = (glyph_width + 7u) / 8u;
        if (bytes_per_row * glyph_height > glyph_size) {
            return false;
        }
        glyph_data = bytes + header_size;
        ready = true;
        return true;
    }

    return false;
}

static void draw_glyph(uint32_t codepoint, size_t x, size_t y,
                       uint8_t red, uint8_t green, uint8_t blue) {
    if (!ready) {
        return;
    }

    size_t index = codepoint;
    if (index >= glyph_count) {
        index = (uint32_t)'?';
        if (index >= glyph_count) {
            index = 0;
        }
    }

    const uint8_t *glyph = glyph_data + index * glyph_size;
    for (size_t gy = 0; gy < glyph_height; ++gy) {
        const uint8_t *row = glyph + gy * bytes_per_row;
        for (size_t gx = 0; gx < glyph_width; ++gx) {
            const uint8_t mask = (uint8_t)(0x80u >> (gx & 7u));
            if ((row[gx >> 3u] & mask) != 0) {
                framebuffer_put_pixel(x + gx, y + gy, red, green, blue);
            }
        }
    }
}

void font_draw_string(const char *text, size_t x, size_t y,
                      uint8_t red, uint8_t green, uint8_t blue) {
    if (!ready || text == NULL) {
        return;
    }

    size_t cursor_x = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const uint8_t c = (uint8_t)text[i];
        draw_glyph(c, cursor_x, y, red, green, blue);
        cursor_x += glyph_width;
    }
}
