#pragma once

#include <linux/types.h>

static inline u32 ether_crc(int length, unsigned char *data) {
    u32 crc = 0xffffffffu;
    for (int i = 0; i < length; ++i) {
        u8 current = data[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            const u32 mix = (crc ^ current) & 1u;
            crc >>= 1;
            if (mix) crc ^= 0xedb88320u;
            current >>= 1;
        }
    }
    return crc;
}
