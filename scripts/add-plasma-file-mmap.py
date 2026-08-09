#!/usr/bin/env python3
"""Add rootfs-backed mmap() semantics to the generated Plasma Linux ABI.

The Plasma rootfs is an immutable CPIO filesystem.  This transform implements
real mmap semantics for the backing types Twilight currently exposes:

* MAP_PRIVATE anonymous mappings;
* MAP_PRIVATE rootfs file mappings, including writable private copies;
* read-only MAP_SHARED rootfs mappings.  Because the CPIO backing is immutable,
  independently-backed read-only pages are observationally equivalent to shared
  pages: neither the file nor any mapping can modify the contents;
* MAP_FIXED replacement and MAP_FIXED_NOREPLACE collision handling.

Writable MAP_SHARED rootfs mappings are rejected with EACCES because Twilight's
CPIO filesystem is read-only and has no writeback path.  Shared anonymous mmap
is still reported as unsupported rather than being faked: implementing that
correctly requires shared VM objects that survive fork.
"""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected mmap source fragment not found: {old[:120]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Extend the base mmap flags with the Linux modes used by Qt/QFile and the
    # dynamic loader.  MAP_FIXED_NOREPLACE is implemented below rather than
    # silently behaving like MAP_FIXED.
    text = replace_once(
        text,
        "#define MAP_PRIVATE   0x02ull\n#define MAP_FIXED     0x10ull\n#define MAP_ANONYMOUS 0x20ull\n",
        "#define MAP_SHARED    0x01ull\n"
        "#define MAP_PRIVATE   0x02ull\n"
        "#define MAP_TYPE      0x0full\n"
        "#define MAP_FIXED     0x10ull\n"
        "#define MAP_ANONYMOUS 0x20ull\n"
        "#define MAP_DENYWRITE 0x0800ull\n"
        "#define MAP_EXECUTABLE 0x1000ull\n"
        "#define MAP_NORESERVE 0x4000ull\n"
        "#define MAP_POPULATE  0x8000ull\n"
        "#define MAP_NONBLOCK  0x10000ull\n"
        "#define MAP_STACK     0x20000ull\n"
        "#define MAP_FIXED_NOREPLACE 0x100000ull\n",
    )

    old = r'''static int64_t sys_mmap(uint64_t address, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
    (void)offset;
    if (length == 0) return -LINUX_EINVAL;
    if ((flags & MAP_PRIVATE) == 0 || (flags & MAP_ANONYMOUS) == 0 || fd != UINT64_MAX)
        return -LINUX_ENOSYS;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return -LINUX_EACCES;
    uint64_t map_length = 0;
    if (!align_up(length, &map_length)) return -LINUX_ENOMEM;
    uint64_t base = address;
    if ((flags & MAP_FIXED) == 0) {
        base = image.mmap_next;
        if (!align_up(base, &base)) return -LINUX_ENOMEM;
    } else if ((base & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) {
        return -LINUX_EINVAL;
    }
    if (base >= SHELL_MMAP_LIMIT || map_length > SHELL_MMAP_LIMIT - base)
        return -LINUX_ENOMEM;
    uint64_t page_flags = VMM_FLAG_USER;
    if ((prot & PROT_WRITE) != 0) page_flags |= VMM_FLAG_WRITE;
    if ((prot & PROT_EXEC) == 0 && vmm_nx_supported()) page_flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = base; va < base + map_length; va += TWILIGHT_PAGE_SIZE)
        if (find_page(va) != 0 || !map_runtime_page(va, page_flags)) return -LINUX_ENOMEM;
    if ((flags & MAP_FIXED) == 0) image.mmap_next = base + map_length + TWILIGHT_PAGE_SIZE;
    return (int64_t)base;
}
'''

    new = r'''static bool plasma_remove_user_page(uint64_t va) {
    for (size_t i = 0; i < image.page_count; ++i) {
        if (image.pages[i].va != va) continue;
        uint64_t old_phys = 0;
        if (!vmm_unmap_page(image.space, va, &old_phys)) return false;
        if (old_phys != 0) (void)pmm_free_page(old_phys);
        image.pages[i] = image.pages[image.page_count - 1u];
        --image.page_count;
        return true;
    }
    return true;
}

static bool plasma_mmap_range_free(uint64_t base, uint64_t map_length) {
    if (base >= SHELL_MMAP_LIMIT || map_length > SHELL_MMAP_LIMIT - base) return false;
    for (uint64_t va = base; va < base + map_length; va += TWILIGHT_PAGE_SIZE)
        if (find_page(va) != 0) return false;
    return true;
}

static bool plasma_mmap_choose_base(uint64_t hint, uint64_t map_length,
                                    bool fixed, uint64_t *base_out) {
    if (base_out == 0 || map_length == 0) return false;
    if (fixed) {
        if ((hint & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return false;
        if (hint >= SHELL_MMAP_LIMIT || map_length > SHELL_MMAP_LIMIT - hint) return false;
        *base_out = hint;
        return true;
    }

    /* Linux treats a non-fixed address as a hint.  Try its page-aligned value
     * first, then fall back to Twilight's monotonically advancing mmap arena. */
    if (hint != 0) {
        const uint64_t candidate = align_down(hint);
        if (candidate != 0 && plasma_mmap_range_free(candidate, map_length)) {
            *base_out = candidate;
            return true;
        }
    }

    uint64_t candidate = image.mmap_next;
    if (!align_up(candidate, &candidate)) return false;
    while (candidate < SHELL_MMAP_LIMIT && map_length <= SHELL_MMAP_LIMIT - candidate) {
        if (plasma_mmap_range_free(candidate, map_length)) {
            *base_out = candidate;
            return true;
        }
        if (candidate > SHELL_MMAP_LIMIT - TWILIGHT_PAGE_SIZE) break;
        candidate += TWILIGHT_PAGE_SIZE;
    }
    return false;
}

static int64_t sys_mmap(uint64_t address, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
    if (length == 0) return -LINUX_EINVAL;
    if ((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) return -LINUX_EINVAL;

    const uint64_t map_type = flags & MAP_TYPE;
    if (map_type != MAP_PRIVATE && map_type != MAP_SHARED) return -LINUX_EINVAL;

    const uint64_t known_flags = MAP_TYPE | MAP_FIXED | MAP_ANONYMOUS |
                                 MAP_DENYWRITE | MAP_EXECUTABLE | MAP_NORESERVE |
                                 MAP_POPULATE | MAP_NONBLOCK | MAP_STACK |
                                 MAP_FIXED_NOREPLACE;
    if ((flags & ~known_flags) != 0) return -LINUX_EINVAL;

    const bool anonymous = (flags & MAP_ANONYMOUS) != 0;
    const bool shared = map_type == MAP_SHARED;
    const bool fixed_replace = (flags & MAP_FIXED) != 0;
    const bool fixed_noreplace = (flags & MAP_FIXED_NOREPLACE) != 0;
    const bool fixed = fixed_replace || fixed_noreplace;

    if (fixed_replace && fixed_noreplace) return -LINUX_EINVAL;
    if (anonymous && shared) {
        /* Correct MAP_SHARED|MAP_ANONYMOUS requires a VM object shared across
         * fork/clone.  Twilight does not have that object type yet. */
        return -LINUX_ENOSYS;
    }

    struct rootfs_open_file *mapped_file = 0;
    if (!anonymous) {
        mapped_file = rootfs_file_for_fd((int)fd);
        if (mapped_file == 0) return -LINUX_EBADF;
        if ((mapped_file->node.mode & 0170000u) == S_IFDIR) return -LINUX_ENODEV;
        if ((offset & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return -LINUX_EINVAL;
        /* A MAP_SHARED writable mapping would promise writeback/coherency with
         * the mapped file. CPIO is immutable, so Linux-style EACCES is the only
         * correct result until a writable backing object exists. */
        if (shared && (prot & PROT_WRITE) != 0) return -LINUX_EACCES;
    }

    uint64_t map_length = 0;
    if (!align_up(length, &map_length)) return -LINUX_ENOMEM;

    uint64_t base = 0;
    if (!plasma_mmap_choose_base(address, map_length, fixed, &base))
        return fixed ? -LINUX_EINVAL : -LINUX_ENOMEM;

    if (fixed_noreplace && !plasma_mmap_range_free(base, map_length))
        return -LINUX_EEXIST;
    if (!fixed && !plasma_mmap_range_free(base, map_length))
        return -LINUX_ENOMEM;

    uint64_t page_flags = VMM_FLAG_USER;
    if ((prot & PROT_WRITE) != 0) page_flags |= VMM_FLAG_WRITE;
    if ((prot & PROT_EXEC) == 0 && vmm_nx_supported()) page_flags |= VMM_FLAG_NO_EXECUTE;

    uint64_t mapped_pages = 0;
    for (uint64_t va = base; va < base + map_length; va += TWILIGHT_PAGE_SIZE) {
        if (fixed_replace && find_page(va) != 0) {
            if (!plasma_remove_user_page(va)) return -LINUX_ENOMEM;
        } else if (find_page(va) != 0) {
            return fixed_noreplace ? -LINUX_EEXIST : -LINUX_ENOMEM;
        }
        if (!map_runtime_page(va, page_flags)) {
            /* Roll back pages created by this call.  MAP_FIXED replacement may
             * have removed older mappings, matching Linux's destructive fixed
             * placement semantics, but it must not leave newly-created partial
             * mappings behind after an allocation failure. */
            for (uint64_t rollback = base; rollback < base + mapped_pages;
                 rollback += TWILIGHT_PAGE_SIZE)
                (void)plasma_remove_user_page(rollback);
            return -LINUX_ENOMEM;
        }
        mapped_pages += TWILIGHT_PAGE_SIZE;
    }

    if (mapped_file != 0 && offset < mapped_file->node.size) {
        uint64_t available = mapped_file->node.size - offset;
        uint64_t copy_length = length < available ? length : available;
        if (copy_length != 0 &&
            !copy_to_process(base, mapped_file->node.data + offset, copy_length)) {
            for (uint64_t rollback = base; rollback < base + mapped_pages;
                 rollback += TWILIGHT_PAGE_SIZE)
                (void)plasma_remove_user_page(rollback);
            return -LINUX_EFAULT;
        }
        /* map_runtime_page() zeroes every page first, so bytes after EOF in the
         * final partial page have Linux's required zero-fill behavior. */
    }

    if (!fixed) {
        const uint64_t next = base + map_length;
        image.mmap_next = next <= SHELL_MMAP_LIMIT - TWILIGHT_PAGE_SIZE
                        ? next + TWILIGHT_PAGE_SIZE : next;
    }
    return (int64_t)base;
}
'''

    text = replace_once(text, old, new)
    path.write_text(text, encoding="utf-8")
    print(f"Added real Plasma rootfs mmap shared/private semantics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
