#!/usr/bin/env python3
"""Expose Twilight's Limine framebuffer as a minimal Linux /dev/fb0 device.

The mapping uses the real framebuffer physical pages directly.  Those pages are
not added to shell_image.pages, so process teardown never hands boot framebuffer
memory back to the PMM as ordinary userspace RAM.
"""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected fbdev source fragment not found: {old[:140]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#include <twilight/rootfs.h>\n",
        "#include <twilight/rootfs.h>\n#include <twilight/framebuffer.h>\n",
    )
    text = replace_once(
        text,
        "#define LINUX_EEXIST     17\n",
        "#define LINUX_EEXIST     17\n#define LINUX_ENODEV     19\n",
    )
    text = replace_once(
        text,
        "#define MAP_ANONYMOUS 0x20ull\n",
        "#define MAP_ANONYMOUS 0x20ull\n#define PLASMA_FB_FD 5\n",
    )

    fb_defs = r'''
#define FBIOGET_VSCREENINFO 0x4600ull
#define FBIOPUT_VSCREENINFO 0x4601ull
#define FBIOGET_FSCREENINFO 0x4602ull
#define FBIOPAN_DISPLAY     0x4606ull
#define FBIOBLANK           0x4611ull
#define FB_TYPE_PACKED_PIXELS 0u
#define FB_VISUAL_TRUECOLOR    2u

struct plasma_fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct plasma_fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct plasma_fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};

struct plasma_fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint16_t padding0;
    uint32_t line_length;
    uint32_t padding1;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
    uint16_t padding2;
};

static void plasma_fb_fill_var(struct plasma_fb_var_screeninfo *var) {
    bytes_zero(var, sizeof(*var));
    var->xres = (uint32_t)framebuffer_width();
    var->yres = (uint32_t)framebuffer_height();
    var->xres_virtual = var->xres;
    var->yres_virtual = var->yres;
    var->bits_per_pixel = (uint32_t)framebuffer_bpp();
    var->red.offset = framebuffer_red_mask_shift();
    var->red.length = framebuffer_red_mask_size();
    var->green.offset = framebuffer_green_mask_shift();
    var->green.length = framebuffer_green_mask_size();
    var->blue.offset = framebuffer_blue_mask_shift();
    var->blue.length = framebuffer_blue_mask_size();
    if (var->bits_per_pixel == 32u) {
        var->transp.offset = 24u;
        var->transp.length = 8u;
    }
    var->height = 0xffffffffu;
    var->width = 0xffffffffu;
}

static void plasma_fb_fill_fix(struct plasma_fb_fix_screeninfo *fix) {
    bytes_zero(fix, sizeof(*fix));
    const char id[] = "Twilight fb0";
    bytes_copy(fix->id, id, sizeof(id));
    fix->smem_start = framebuffer_physical_address();
    fix->smem_len = (uint32_t)framebuffer_size();
    fix->type = FB_TYPE_PACKED_PIXELS;
    fix->visual = FB_VISUAL_TRUECOLOR;
    fix->line_length = (uint32_t)framebuffer_pitch();
}

static int64_t plasma_fb_ioctl(uint64_t request, uint64_t argument) {
    if (framebuffer_width() == 0 || framebuffer_height() == 0 ||
        framebuffer_bpp() != 32u)
        return -LINUX_ENODEV;
    switch (request) {
    case FBIOGET_VSCREENINFO: {
        struct plasma_fb_var_screeninfo var;
        plasma_fb_fill_var(&var);
        return user_copy_out(argument, &var, sizeof(var)) ? 0 : -LINUX_EFAULT;
    }
    case FBIOGET_FSCREENINFO: {
        struct plasma_fb_fix_screeninfo fix;
        plasma_fb_fill_fix(&fix);
        return user_copy_out(argument, &fix, sizeof(fix)) ? 0 : -LINUX_EFAULT;
    }
    case FBIOPUT_VSCREENINFO: {
        struct plasma_fb_var_screeninfo requested;
        if (!user_copy_in(&requested, argument, sizeof(requested))) return -LINUX_EFAULT;
        if (requested.xres != framebuffer_width() ||
            requested.yres != framebuffer_height() ||
            requested.bits_per_pixel != framebuffer_bpp())
            return -LINUX_EINVAL;
        plasma_fb_fill_var(&requested);
        return user_copy_out(argument, &requested, sizeof(requested)) ? 0 : -LINUX_EFAULT;
    }
    case FBIOPAN_DISPLAY:
    case FBIOBLANK:
        return 0;
    default:
        return -LINUX_ENOTTY;
    }
}

static int64_t plasma_fb_mmap(uint64_t address, uint64_t length,
                              uint64_t prot, uint64_t flags, uint64_t offset) {
    if (offset != 0 || length == 0) return -LINUX_EINVAL;
    const uint64_t physical = framebuffer_physical_address();
    const uint64_t fb_size = framebuffer_size();
    if (physical == 0 || fb_size == 0 ||
        (physical & (TWILIGHT_PAGE_SIZE - 1ull)) != 0)
        return -LINUX_ENODEV;
    if (length > fb_size) return -LINUX_EINVAL;
    if ((prot & PROT_EXEC) != 0) return -LINUX_EACCES;

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

    uint64_t page_flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) page_flags |= VMM_FLAG_NO_EXECUTE;
    uint64_t mapped = 0;
    for (; mapped < map_length; mapped += TWILIGHT_PAGE_SIZE) {
        uint64_t ignored_phys = 0, ignored_flags = 0;
        if (vmm_translate(image.space, base + mapped, &ignored_phys, &ignored_flags) ||
            !vmm_map_page(image.space, base + mapped, physical + mapped, page_flags)) {
            while (mapped != 0) {
                mapped -= TWILIGHT_PAGE_SIZE;
                (void)vmm_unmap_page(image.space, base + mapped, 0);
            }
            return -LINUX_ENOMEM;
        }
    }
    if ((flags & MAP_FIXED) == 0)
        image.mmap_next = base + map_length + TWILIGHT_PAGE_SIZE;
    serial_write("[linux:fbdev] mapped /dev/fb0 into userspace\n");
    return (int64_t)base;
}

'''

    # sys_mmap appears before sys_ioctl, where the full fbdev helpers are
    # injected. Give it a forward declaration before adding the dispatch hook.
    mmap_anchor = "static int64_t sys_mmap(uint64_t address, uint64_t length, uint64_t prot,\n                        uint64_t flags, uint64_t fd, uint64_t offset) {\n"
    text = replace_once(
        text,
        mmap_anchor,
        "static int64_t plasma_fb_mmap(uint64_t address, uint64_t length,\n"
        "                              uint64_t prot, uint64_t flags, uint64_t offset);\n\n"
        + mmap_anchor
        + "    if ((int)fd == PLASMA_FB_FD && (flags & MAP_ANONYMOUS) == 0)\n"
        + "        return plasma_fb_mmap(address, length, prot, flags, offset);\n",
    )

    text = replace_once(
        text,
        "static int64_t sys_ioctl(uint64_t fd_value, uint64_t request, uint64_t argument) {\n",
        fb_defs + "static int64_t sys_ioctl(uint64_t fd_value, uint64_t request, uint64_t argument) {\n",
    )
    text = replace_once(
        text,
        "    const int fd = (int)fd_value;\n    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n",
        "    const int fd = (int)fd_value;\n"
        "    if (fd == PLASMA_FB_FD) return plasma_fb_ioctl(request, argument);\n"
        "    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n",
    )

    text = replace_once(
        text,
        "static bool fd_is_tty(int fd) {\n    return fd >= 0 && fd <= 9;\n}\n",
        "static bool fd_is_tty(int fd) {\n"
        "    return fd >= 0 && fd <= 9 && fd != PLASMA_FB_FD;\n"
        "}\n",
    )

    text = replace_once(
        text,
        "        string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\") ||\n",
        "        string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\") ||\n"
        "        string_equal(path, \"/dev/fb0\") ||\n",
    )
    text = replace_once(
        text,
        "    if (string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\"))\n"
        "        return fill_stat(stat_address, S_IFCHR | 0666u);\n",
        "    if (string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\") ||\n"
        "        string_equal(path, \"/dev/fb0\"))\n"
        "        return fill_stat(stat_address, S_IFCHR | 0666u);\n",
    )
    text = replace_once(
        text,
        "    if (string_equal(path, \"/dev/null\")) return 4;\n",
        "    if (string_equal(path, \"/dev/null\")) return 4;\n"
        "    if (string_equal(path, \"/dev/fb0\")) return PLASMA_FB_FD;\n",
    )

    text = replace_once(
        text,
        "        if (!fd_is_tty((int)a1) && (int)a1 != 4) return -LINUX_EBADF;\n"
        "        return fill_stat(a2, S_IFCHR | 0666u);\n",
        "        if ((int)a1 == PLASMA_FB_FD) return fill_stat(a2, S_IFCHR | 0666u);\n"
        "        if (!fd_is_tty((int)a1) && (int)a1 != 4) return -LINUX_EBADF;\n"
        "        return fill_stat(a2, S_IFCHR | 0666u);\n",
    )

    text = replace_once(
        text,
        "static int64_t sys_fcntl(int fd, uint64_t command, uint64_t argument) {\n"
        "    (void)argument;\n"
        "    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n",
        "static int64_t sys_fcntl(int fd, uint64_t command, uint64_t argument) {\n"
        "    (void)argument;\n"
        "    if (fd == PLASMA_FB_FD) {\n"
        "        if (command == 1 || command == 2 || command == 4) return 0;\n"
        "        if (command == 3) return 2;\n"
        "        return 0;\n"
        "    }\n"
        "    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Added Twilight /dev/fb0 Linux fbdev ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
