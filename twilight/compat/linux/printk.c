#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/printk.h>
#include <twilight/log.h>

#define PRINTK_BUFFER_SIZE 768u

struct output_buffer {
    char data[PRINTK_BUFFER_SIZE];
    size_t length;
};

static void append_char(struct output_buffer *out, char value) {
    if (out->length + 1u < PRINTK_BUFFER_SIZE) out->data[out->length] = value;
    ++out->length;
}

static void append_repeat(struct output_buffer *out, char value, unsigned count) {
    while (count-- != 0u) append_char(out, value);
}

static size_t string_length_limited(const char *string, int precision) {
    if (string == 0) string = "(null)";
    size_t length = 0;
    while (string[length] != '\0' && (precision < 0 || length < (size_t)precision)) ++length;
    return length;
}

static void append_string_formatted(struct output_buffer *out,
                                    const char *string,
                                    unsigned width,
                                    int precision,
                                    bool left_align) {
    if (string == 0) string = "(null)";
    const size_t length = string_length_limited(string, precision);
    if (!left_align && width > length) append_repeat(out, ' ', width - (unsigned)length);
    for (size_t i = 0; i < length; ++i) append_char(out, string[i]);
    if (left_align && width > length) append_repeat(out, ' ', width - (unsigned)length);
}

static size_t encode_unsigned_reverse(uint64_t value,
                                      unsigned base,
                                      bool uppercase,
                                      char reverse[32]) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t count = 0;
    do {
        reverse[count++] = digits[value % base];
        value /= base;
    } while (value != 0 && count < 32u);
    return count;
}

static void append_unsigned_formatted(struct output_buffer *out,
                                      uint64_t value,
                                      unsigned base,
                                      bool uppercase,
                                      unsigned width,
                                      int precision,
                                      bool alternate,
                                      bool left_align,
                                      bool zero_pad) {
    char reverse[32];
    size_t digits = encode_unsigned_reverse(value, base, uppercase, reverse);

    /* C integer precision zero with a zero value emits no digits. Kernel logs
     * almost never use that form, but honoring it keeps the parser sane. */
    if (precision == 0 && value == 0) digits = 0;

    unsigned prefix = 0;
    char prefix_a = 0;
    char prefix_b = 0;
    if (alternate && base == 16u && value != 0) {
        prefix = 2u;
        prefix_a = '0';
        prefix_b = uppercase ? 'X' : 'x';
    } else if (alternate && base == 8u && (digits == 0 || reverse[digits - 1u] != '0')) {
        prefix = 1u;
        prefix_a = '0';
    }

    unsigned leading_zeroes = 0;
    if (precision > 0 && (unsigned)precision > digits)
        leading_zeroes = (unsigned)precision - (unsigned)digits;

    unsigned content = prefix + leading_zeroes + (unsigned)digits;
    unsigned spaces = width > content ? width - content : 0u;

    if (zero_pad && precision < 0 && !left_align) {
        leading_zeroes += spaces;
        spaces = 0u;
    }

    if (!left_align) append_repeat(out, ' ', spaces);
    if (prefix >= 1u) append_char(out, prefix_a);
    if (prefix == 2u) append_char(out, prefix_b);
    append_repeat(out, '0', leading_zeroes);
    while (digits != 0) append_char(out, reverse[--digits]);
    if (left_align) append_repeat(out, ' ', spaces);
}

static void append_signed_formatted(struct output_buffer *out,
                                    int64_t value,
                                    unsigned width,
                                    int precision,
                                    bool left_align,
                                    bool zero_pad,
                                    bool plus_sign,
                                    bool space_sign) {
    char sign = 0;
    uint64_t magnitude;
    if (value < 0) {
        sign = '-';
        magnitude = (uint64_t)(-(value + 1)) + 1ull;
    } else {
        if (plus_sign) sign = '+';
        else if (space_sign) sign = ' ';
        magnitude = (uint64_t)value;
    }

    char reverse[32];
    size_t digits = encode_unsigned_reverse(magnitude, 10u, false, reverse);
    if (precision == 0 && magnitude == 0) digits = 0;

    unsigned leading_zeroes = 0;
    if (precision > 0 && (unsigned)precision > digits)
        leading_zeroes = (unsigned)precision - (unsigned)digits;

    const unsigned sign_width = sign != 0 ? 1u : 0u;
    unsigned content = sign_width + leading_zeroes + (unsigned)digits;
    unsigned spaces = width > content ? width - content : 0u;

    if (zero_pad && precision < 0 && !left_align) {
        leading_zeroes += spaces;
        spaces = 0u;
    }

    if (!left_align) append_repeat(out, ' ', spaces);
    if (sign != 0) append_char(out, sign);
    append_repeat(out, '0', leading_zeroes);
    while (digits != 0) append_char(out, reverse[--digits]);
    if (left_align) append_repeat(out, ' ', spaces);
}

int vprintk(const char *format, va_list arguments) {
    if (format == 0) return 0;

    struct output_buffer out = { .data = {0}, .length = 0 };

    for (size_t i = 0; format[i] != '\0'; ++i) {
        if (format[i] != '%') {
            append_char(&out, format[i]);
            continue;
        }

        ++i;
        if (format[i] == '%') {
            append_char(&out, '%');
            continue;
        }
        if (format[i] == '\0') break;

        bool alternate = false;
        bool left_align = false;
        bool zero_pad = false;
        bool plus_sign = false;
        bool space_sign = false;

        bool parsing_flags = true;
        while (parsing_flags) {
            switch (format[i]) {
                case '#': alternate = true; ++i; break;
                case '-': left_align = true; ++i; break;
                case '0': zero_pad = true; ++i; break;
                case '+': plus_sign = true; ++i; break;
                case ' ': space_sign = true; ++i; break;
                default: parsing_flags = false; break;
            }
        }

        unsigned width = 0;
        if (format[i] == '*') {
            const int supplied = va_arg(arguments, int);
            if (supplied < 0) {
                left_align = true;
                width = (unsigned)(-supplied);
            } else {
                width = (unsigned)supplied;
            }
            ++i;
        } else {
            while (format[i] >= '0' && format[i] <= '9') {
                width = width * 10u + (unsigned)(format[i] - '0');
                ++i;
            }
        }

        int precision = -1;
        if (format[i] == '.') {
            ++i;
            precision = 0;
            if (format[i] == '*') {
                precision = va_arg(arguments, int);
                if (precision < 0) precision = -1;
                ++i;
            } else {
                while (format[i] >= '0' && format[i] <= '9') {
                    precision = precision * 10 + (int)(format[i] - '0');
                    ++i;
                }
            }
        }

        enum { LEN_DEFAULT, LEN_CHAR, LEN_SHORT, LEN_LONG, LEN_LONGLONG, LEN_SIZE } length = LEN_DEFAULT;
        if (format[i] == 'z') {
            length = LEN_SIZE;
            ++i;
        } else if (format[i] == 'h') {
            length = LEN_SHORT;
            ++i;
            if (format[i] == 'h') {
                length = LEN_CHAR;
                ++i;
            }
        } else if (format[i] == 'l') {
            length = LEN_LONG;
            ++i;
            if (format[i] == 'l') {
                length = LEN_LONGLONG;
                ++i;
            }
        }

        const char specifier = format[i];
        switch (specifier) {
            case 'c': {
                const char value = (char)va_arg(arguments, int);
                if (!left_align && width > 1u) append_repeat(&out, ' ', width - 1u);
                append_char(&out, value);
                if (left_align && width > 1u) append_repeat(&out, ' ', width - 1u);
                break;
            }
            case 's':
                append_string_formatted(&out, va_arg(arguments, const char *), width, precision, left_align);
                break;
            case 'd':
            case 'i': {
                int64_t value;
                if (length == LEN_LONGLONG) value = va_arg(arguments, long long);
                else if (length == LEN_LONG) value = va_arg(arguments, long);
                else if (length == LEN_SIZE) value = (int64_t)va_arg(arguments, intptr_t);
                else value = va_arg(arguments, int);
                append_signed_formatted(&out, value, width, precision, left_align, zero_pad,
                                        plus_sign, space_sign);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                uint64_t value;
                if (length == LEN_LONGLONG) value = va_arg(arguments, unsigned long long);
                else if (length == LEN_LONG) value = va_arg(arguments, unsigned long);
                else if (length == LEN_SIZE) value = va_arg(arguments, size_t);
                else value = va_arg(arguments, unsigned int);
                const unsigned base = specifier == 'u' ? 10u : (specifier == 'o' ? 8u : 16u);
                append_unsigned_formatted(&out, value, base, specifier == 'X', width, precision,
                                          alternate, left_align, zero_pad);
                break;
            }
            case 'p': {
                const uintptr_t pointer = (uintptr_t)va_arg(arguments, void *);
                append_string_formatted(&out, "0x", 0u, -1, false);
                append_unsigned_formatted(&out,
                                          (uint64_t)pointer,
                                          16u,
                                          false,
                                          width != 0 ? width : (unsigned)(sizeof(uintptr_t) * 2u),
                                          precision,
                                          false,
                                          false,
                                          true);
                break;
            }
            default:
                /* Unknown conversion: preserve it textually. Crucially, all
                 * flags/width/precision have already been parsed so supported
                 * Linux forms such as %4.4x cannot desynchronise va_list. */
                append_char(&out, '%');
                append_char(&out, specifier);
                break;
        }
    }

    size_t stored = out.length;
    if (stored >= PRINTK_BUFFER_SIZE) stored = PRINTK_BUFFER_SIZE - 1u;
    out.data[stored] = '\0';

    while (stored != 0 && (out.data[stored - 1u] == '\n' || out.data[stored - 1u] == '\r')) {
        out.data[--stored] = '\0';
    }

    klog(out.data);
    return (int)out.length;
}

int printk(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = vprintk(format, arguments);
    va_end(arguments);
    return result;
}
