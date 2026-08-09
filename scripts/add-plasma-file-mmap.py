#!/usr/bin/env python3
"""Add rootfs-backed mmap() semantics to the generated Plasma Linux ABI.

Musl maps shared objects with MAP_PRIVATE file mappings and then replaces
individual pages/segments using MAP_FIXED.  Twilight's earlier shell ABI only
supported anonymous mappings.  This transform keeps normal builds untouched
while giving the Plasma bring-up image the DSO mapping behavior it needs.
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

static int64_t sys_mmap(uint64_t address, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
    if (length == 0) return -LINUX_EINVAL;
    if ((flags & MAP_PRIVATE) == 0) return -LINUX_ENOSYS;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return -LINUX_EACCES;

    const bool anonymous = (flags & MAP_ANONYMOUS) != 0;
    struct rootfs_open_file *mapped_file = 0;
    if (anonymous) {
        if (fd != UINT64_MAX) return -LINUX_EBADF;
    } else {
        mapped_file = rootfs_file_for_fd((int)fd);
        if (mapped_file == 0) return -LINUX_EBADF;
        if ((offset & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return -LINUX_EINVAL;
    }

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

    for (uint64_t va = base; va < base + map_length; va += TWILIGHT_PAGE_SIZE) {
        if ((flags & MAP_FIXED) != 0 && find_page(va) != 0) {
            if (!plasma_remove_user_page(va)) return -LINUX_ENOMEM;
        } else if (find_page(va) != 0) {
            return -LINUX_ENOMEM;
        }
        if (!map_runtime_page(va, page_flags)) return -LINUX_ENOMEM;
    }

    if (mapped_file != 0 && offset < mapped_file->node.size) {
        uint64_t available = mapped_file->node.size - offset;
        uint64_t copy_length = length < available ? length : available;
        if (copy_length != 0 &&
            !copy_to_process(base, mapped_file->node.data + offset, copy_length))
            return -LINUX_EFAULT;
    }

    if ((flags & MAP_FIXED) == 0)
        image.mmap_next = base + map_length + TWILIGHT_PAGE_SIZE;
    return (int64_t)base;
}
'''

    text = replace_once(text, old, new)
    path.write_text(text, encoding="utf-8")
    print(f"Added Plasma rootfs-backed mmap/MAP_FIXED support: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
