#include <stddef.h>
#include <stdint.h>

#include <twilight/sha256.h>

static const uint32_t round_constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotate_right(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32u - amount));
}

static uint32_t read_be32(const uint8_t *input) {
    return ((uint32_t)input[0] << 24) |
           ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) |
           (uint32_t)input[3];
}

static void write_be32(uint8_t *output, uint32_t value) {
    output[0] = (uint8_t)(value >> 24);
    output[1] = (uint8_t)(value >> 16);
    output[2] = (uint8_t)(value >> 8);
    output[3] = (uint8_t)value;
}

static void transform(struct sha256_context *context, const uint8_t block[64]) {
    uint32_t words[64];
    for (size_t i = 0; i < 16; ++i) words[i] = read_be32(block + i * 4u);

    for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotate_right(words[i - 15], 7) ^
                            rotate_right(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
        const uint32_t s1 = rotate_right(words[i - 2], 17) ^
                            rotate_right(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];

    for (size_t i = 0; i < 64; ++i) {
        const uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + sum1 + choose + round_constants[i] + words[i];
        const uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void sha256_init(struct sha256_context *context) {
    context->state[0] = 0x6a09e667u;
    context->state[1] = 0xbb67ae85u;
    context->state[2] = 0x3c6ef372u;
    context->state[3] = 0xa54ff53au;
    context->state[4] = 0x510e527fu;
    context->state[5] = 0x9b05688cu;
    context->state[6] = 0x1f83d9abu;
    context->state[7] = 0x5be0cd19u;
    context->total_bytes = 0;
    context->buffer_size = 0;
}

void sha256_update(struct sha256_context *context, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    context->total_bytes += size;

    while (size != 0) {
        const size_t available = 64u - context->buffer_size;
        const size_t take = size < available ? size : available;
        for (size_t i = 0; i < take; ++i) {
            context->buffer[context->buffer_size + i] = bytes[i];
        }
        context->buffer_size += take;
        bytes += take;
        size -= take;

        if (context->buffer_size == 64u) {
            transform(context, context->buffer);
            context->buffer_size = 0;
        }
    }
}

void sha256_final(struct sha256_context *context, uint8_t digest[SHA256_DIGEST_SIZE]) {
    const uint64_t bit_length = context->total_bytes * 8ull;

    context->buffer[context->buffer_size++] = 0x80u;
    if (context->buffer_size > 56u) {
        while (context->buffer_size < 64u) context->buffer[context->buffer_size++] = 0;
        transform(context, context->buffer);
        context->buffer_size = 0;
    }

    while (context->buffer_size < 56u) context->buffer[context->buffer_size++] = 0;
    for (unsigned i = 0; i < 8; ++i) {
        context->buffer[56u + i] = (uint8_t)(bit_length >> (56u - i * 8u));
    }
    transform(context, context->buffer);

    for (size_t i = 0; i < 8; ++i) write_be32(digest + i * 4u, context->state[i]);

    for (size_t i = 0; i < sizeof(*context); ++i) ((volatile uint8_t *)context)[i] = 0;
}

void sha256(const void *data, size_t size, uint8_t digest[SHA256_DIGEST_SIZE]) {
    struct sha256_context context;
    sha256_init(&context);
    sha256_update(&context, data, size);
    sha256_final(&context, digest);
}
