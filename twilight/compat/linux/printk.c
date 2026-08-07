#include <stdarg.h>
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

static void append_string(struct output_buffer *out, const char *string) {
    if (string == 0) string = "(null)";
    while (*string != '\0') append_char(out, *string++);
}

static size_t number_digits(uint64_t value, unsigned base) {
    size_t digits = 1;
    while (value >= base) {
        value /= base;
        ++digits;
    }
    return digits;
}

static void append_unsigned(struct output_buffer *out,
                            uint64_t value,
                            unsigned base,
                            bool uppercase,
                            unsigned width,
                            char padding) {
    const char *digits = uppercase
        ? "0123456789ABCDEF"
        : "0123456789abcdef";
    char reverse[32];
    size_t count = 0;

    do {
        reverse[count++] = digits[value % base];
        value /= base;
    } while (value != 0 && count < sizeof(reverse));

    while (width > count) {
        append_char(out, padding);
        --width;
    }

    while (count != 0) append_char(out, reverse[--count]);
}

static void append_signed(struct output_buffer *out,
                          int64_t value,
                          unsigned width,
                          char padding) {
    if (value < 0) {
        append_char(out, '-');
        if (width != 0) --width;
        const uint64_t magnitude = (uint64_t)(-(value + 1)) + 1ull;
        append_unsigned(out, magnitude, 10, false, width, padding);
        return;
    }
    append_unsigned(out, (uint64_t)value, 10, false, width, padding);
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

        char padding = ' ';
        if (format[i] == '0') {
            padding = '0';
            ++i;
        }

        unsigned width = 0;
        while (format[i] >= '0' && format[i] <= '9') {
            width = width * 10u + (unsigned)(format[i] - '0');
            ++i;
        }

        enum { LEN_DEFAULT, LEN_LONG, LEN_LONGLONG, LEN_SIZE } length = LEN_DEFAULT;
        if (format[i] == 'z') {
            length = LEN_SIZE;
            ++i;
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
            case 'c':
                append_char(&out, (char)va_arg(arguments, int));
                break;
            case 's':
                append_string(&out, va_arg(arguments, const char *));
                break;
            case 'd':
            case 'i': {
                int64_t value;
                if (length == LEN_LONGLONG) value = va_arg(arguments, long long);
                else if (length == LEN_LONG) value = va_arg(arguments, long);
                else if (length == LEN_SIZE) value = (int64_t)va_arg(arguments, size_t);
                else value = va_arg(arguments, int);
                append_signed(&out, value, width, padding);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                uint64_t value;
                if (length == LEN_LONGLONG) value = va_arg(arguments, unsigned long long);
                else if (length == LEN_LONG) value = va_arg(arguments, unsigned long);
                else if (length == LEN_SIZE) value = va_arg(arguments, size_t);
                else value = va_arg(arguments, unsigned int);
                const unsigned base = specifier == 'u' ? 10u : 16u;
                append_unsigned(&out, value, base, specifier == 'X', width, padding);
                break;
            }
            case 'p': {
                const uintptr_t pointer = (uintptr_t)va_arg(arguments, void *);
                append_string(&out, "0x");
                append_unsigned(&out,
                                (uint64_t)pointer,
                                16,
                                false,
                                width != 0 ? width : (unsigned)(sizeof(uintptr_t) * 2u),
                                '0');
                break;
            }
            default:
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
