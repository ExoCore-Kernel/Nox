#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/rootfs.h>

#define CPIO_NEWC_HEADER_SIZE 110u
#define CPIO_MODE_TYPE_MASK   0170000u
#define CPIO_MODE_DIRECTORY   0040000u
#define CPIO_MODE_SYMLINK     0120000u
#define ROOTFS_PATH_MAX       512u
#define ROOTFS_SYMLINK_MAX    12u

static const uint8_t *rootfs_archive;
static size_t rootfs_archive_size;
static size_t rootfs_entries;
static bool rootfs_ready;

static size_t align4(size_t value) {
    return (value + 3u) & ~(size_t)3u;
}

static bool magic_ok(const uint8_t *header) {
    static const char newc[] = "070701";
    static const char crc[] = "070702";
    bool first = true;
    bool second = true;
    for (size_t i = 0; i < 6u; ++i) {
        if (header[i] != (uint8_t)newc[i]) first = false;
        if (header[i] != (uint8_t)crc[i]) second = false;
    }
    return first || second;
}

static bool hex_u32(const uint8_t *text, uint32_t *out) {
    if (text == 0 || out == 0) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < 8u; ++i) {
        const uint8_t c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10u);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10u);
        else return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

static const char *canonical_path(const char *path) {
    if (path == 0) return 0;
    while (*path == '/') ++path;
    while (path[0] == '.' && path[1] == '/') path += 2;
    return path;
}

static bool path_equal(const char *wanted, const char *entry) {
    wanted = canonical_path(wanted);
    entry = canonical_path(entry);
    if (wanted == 0 || entry == 0) return false;

    /* CPIO commonly stores the archive root as '.'. Treat it as '/'. */
    if (*wanted == '\0') return entry[0] == '.' && entry[1] == '\0';
    if (wanted[0] == '.' && wanted[1] == '\0')
        return entry[0] == '.' && entry[1] == '\0';

    while (*wanted != '\0' && *entry != '\0') {
        if (*wanted++ != *entry++) return false;
    }
    return *wanted == '\0' && *entry == '\0';
}

static bool string_equal(const char *a, const char *b) {
    if (a == 0 || b == 0) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) return false;
    }
    return *a == '\0' && *b == '\0';
}

struct parsed_entry {
    const char *name;
    const uint8_t *data;
    uint32_t mode;
    uint32_t size;
    size_t next_offset;
};

static bool parse_entry(size_t offset, struct parsed_entry *out) {
    if (out == 0 || rootfs_archive == 0) return false;
    if (offset > rootfs_archive_size ||
        rootfs_archive_size - offset < CPIO_NEWC_HEADER_SIZE) return false;

    const uint8_t *header = rootfs_archive + offset;
    if (!magic_ok(header)) return false;

    uint32_t mode = 0;
    uint32_t file_size = 0;
    uint32_t name_size = 0;
    if (!hex_u32(header + 14u, &mode) ||
        !hex_u32(header + 54u, &file_size) ||
        !hex_u32(header + 94u, &name_size) ||
        name_size == 0) return false;

    const size_t name_offset = offset + CPIO_NEWC_HEADER_SIZE;
    if (name_offset > rootfs_archive_size ||
        name_size > rootfs_archive_size - name_offset) return false;

    const char *name = (const char *)(rootfs_archive + name_offset);
    if (name[name_size - 1u] != '\0') return false;

    const size_t data_offset = align4(name_offset + (size_t)name_size);
    if (data_offset > rootfs_archive_size ||
        file_size > rootfs_archive_size - data_offset) return false;

    const size_t next = align4(data_offset + (size_t)file_size);
    if (next > rootfs_archive_size || next <= offset) return false;

    out->name = name;
    out->data = rootfs_archive + data_offset;
    out->mode = mode;
    out->size = file_size;
    out->next_offset = next;
    return true;
}

bool rootfs_init(const void *archive, size_t size) {
    rootfs_archive = 0;
    rootfs_archive_size = 0;
    rootfs_entries = 0;
    rootfs_ready = false;

    if (archive == 0 || size < CPIO_NEWC_HEADER_SIZE) return false;
    rootfs_archive = (const uint8_t *)archive;
    rootfs_archive_size = size;

    size_t offset = 0;
    bool trailer_seen = false;
    while (offset < size) {
        struct parsed_entry entry;
        if (!parse_entry(offset, &entry)) {
            rootfs_archive = 0;
            rootfs_archive_size = 0;
            rootfs_entries = 0;
            return false;
        }
        if (string_equal(entry.name, "TRAILER!!!")) {
            trailer_seen = true;
            break;
        }
        ++rootfs_entries;
        offset = entry.next_offset;
    }

    if (!trailer_seen) {
        rootfs_archive = 0;
        rootfs_archive_size = 0;
        rootfs_entries = 0;
        return false;
    }

    rootfs_ready = true;
    return true;
}

bool rootfs_available(void) {
    return rootfs_ready;
}

size_t rootfs_entry_count(void) {
    return rootfs_ready ? rootfs_entries : 0u;
}

bool rootfs_lookup(const char *path, struct rootfs_node *out) {
    if (!rootfs_ready || path == 0 || out == 0) return false;

    size_t offset = 0;
    while (offset < rootfs_archive_size) {
        struct parsed_entry entry;
        if (!parse_entry(offset, &entry)) return false;
        if (string_equal(entry.name, "TRAILER!!!")) return false;
        if (path_equal(path, entry.name)) {
            out->name = entry.name;
            out->data = entry.data;
            out->size = entry.size;
            out->mode = entry.mode;
            return true;
        }
        offset = entry.next_offset;
    }
    return false;
}

static bool normalize_path(const char *input, char out[ROOTFS_PATH_MAX]) {
    if (input == 0 || out == 0) return false;

    size_t out_len = 0;
    size_t component_start[64];
    size_t depth = 0;
    const char *p = input;

    while (*p == '/') ++p;
    while (*p != '\0') {
        while (*p == '/') ++p;
        if (*p == '\0') break;

        const char *start = p;
        while (*p != '\0' && *p != '/') ++p;
        const size_t len = (size_t)(p - start);

        if (len == 1u && start[0] == '.') continue;
        if (len == 2u && start[0] == '.' && start[1] == '.') {
            if (depth != 0) {
                out_len = component_start[--depth];
                if (out_len != 0 && out[out_len - 1u] == '/') --out_len;
            }
            continue;
        }

        if (depth >= sizeof(component_start) / sizeof(component_start[0])) return false;
        if (out_len != 0) {
            if (out_len + 1u >= ROOTFS_PATH_MAX) return false;
            out[out_len++] = '/';
        }
        component_start[depth++] = out_len;
        if (len >= ROOTFS_PATH_MAX - out_len) return false;
        for (size_t i = 0; i < len; ++i) out[out_len++] = start[i];
    }

    if (out_len == 0) {
        out[0] = '.';
        out[1] = '\0';
    } else {
        out[out_len] = '\0';
    }
    return true;
}

bool rootfs_lookup_follow(const char *path, struct rootfs_node *out) {
    if (!rootfs_ready || path == 0 || out == 0) return false;

    char current[ROOTFS_PATH_MAX];
    if (!normalize_path(path, current)) return false;

    for (unsigned depth = 0; depth < ROOTFS_SYMLINK_MAX; ++depth) {
        struct rootfs_node node;
        if (!rootfs_lookup(current, &node)) return false;
        if ((node.mode & CPIO_MODE_TYPE_MASK) != CPIO_MODE_SYMLINK) {
            *out = node;
            return true;
        }
        if (node.size == 0 || node.size >= ROOTFS_PATH_MAX) return false;

        char target[ROOTFS_PATH_MAX];
        for (size_t i = 0; i < node.size; ++i) target[i] = (char)node.data[i];
        target[node.size] = '\0';

        char combined[ROOTFS_PATH_MAX];
        if (target[0] == '/') {
            if (!normalize_path(target, combined)) return false;
        } else {
            size_t slash = 0;
            size_t current_len = 0;
            while (current[current_len] != '\0') {
                if (current[current_len] == '/') slash = current_len + 1u;
                ++current_len;
            }
            if (slash + node.size + 1u >= ROOTFS_PATH_MAX) return false;
            size_t n = 0;
            for (; n < slash; ++n) combined[n] = current[n];
            for (size_t i = 0; i < node.size; ++i) combined[n++] = target[i];
            combined[n] = '\0';
            char normalized[ROOTFS_PATH_MAX];
            if (!normalize_path(combined, normalized)) return false;
            n = 0;
            do {
                current[n] = normalized[n];
            } while (normalized[n++] != '\0');
            continue;
        }

        size_t n = 0;
        do {
            current[n] = combined[n];
        } while (combined[n++] != '\0');
    }
    return false;
}

size_t rootfs_read(const struct rootfs_node *node, size_t offset,
                   void *buffer, size_t size) {
    if (!rootfs_ready || node == 0 || buffer == 0 || size == 0 ||
        (node->mode & CPIO_MODE_TYPE_MASK) == CPIO_MODE_DIRECTORY ||
        offset >= node->size) return 0;

    size_t remaining = node->size - offset;
    if (size > remaining) size = remaining;

    uint8_t *out = (uint8_t *)buffer;
    const uint8_t *in = node->data + offset;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
    return size;
}
