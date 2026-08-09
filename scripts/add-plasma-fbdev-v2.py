#!/usr/bin/env python3
"""Expose Twilight's Limine framebuffer as Linux /dev/fb0 after runtime-FD injection."""
from __future__ import annotations
import pathlib, sys

def rep(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected fbdev-v2 fragment not found: {old[:140]!r}")
    return text.replace(old, new, 1)

def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr); return 2
    p = pathlib.Path(sys.argv[1]); text = p.read_text(encoding="utf-8")
    text = rep(text, "#include <twilight/rootfs.h>\n", "#include <twilight/rootfs.h>\n#include <twilight/framebuffer.h>\n")
    text = rep(text, "#define MAP_ANONYMOUS 0x20ull\n", "#define MAP_ANONYMOUS 0x20ull\n#define PLASMA_FB_FD 5\n")

    mmap_anchor = "static int64_t sys_mmap(uint64_t address, uint64_t length, uint64_t prot,\n                        uint64_t flags, uint64_t fd, uint64_t offset) {\n"
    text = rep(text, mmap_anchor,
        "static int64_t plasma_fb_mmap(uint64_t address, uint64_t length, uint64_t prot,\n"
        "                              uint64_t flags, uint64_t offset);\n\n" + mmap_anchor +
        "    if ((int)fd == PLASMA_FB_FD && (flags & MAP_ANONYMOUS) == 0)\n"
        "        return plasma_fb_mmap(address, length, prot, flags, offset);\n")

    text = rep(text, "static bool fd_is_tty(int fd) {\n    return fd >= 0 && fd <= 9;\n}\n",
               "static bool fd_is_tty(int fd) {\n    return fd >= 0 && fd <= 9 && fd != PLASMA_FB_FD;\n}\n")

    defs = r'''
#define FBIOGET_VSCREENINFO 0x4600ull
#define FBIOPUT_VSCREENINFO 0x4601ull
#define FBIOGET_FSCREENINFO 0x4602ull
#define FBIOPAN_DISPLAY 0x4606ull
#define FBIOBLANK 0x4611ull
struct plasma_fb_bitfield { uint32_t offset, length, msb_right; };
struct plasma_fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct plasma_fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock;
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};
struct plasma_fb_fix_screeninfo {
    char id[16]; uint64_t smem_start; uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep, pad0; uint32_t line_length, pad1;
    uint64_t mmio_start; uint32_t mmio_len, accel; uint16_t capabilities, reserved[2], pad2;
};
static void plasma_fb_var(struct plasma_fb_var_screeninfo *v) {
    bytes_zero(v, sizeof(*v)); v->xres=(uint32_t)framebuffer_width(); v->yres=(uint32_t)framebuffer_height();
    v->xres_virtual=v->xres; v->yres_virtual=v->yres; v->bits_per_pixel=framebuffer_bpp();
    v->red.offset=framebuffer_red_mask_shift(); v->red.length=framebuffer_red_mask_size();
    v->green.offset=framebuffer_green_mask_shift(); v->green.length=framebuffer_green_mask_size();
    v->blue.offset=framebuffer_blue_mask_shift(); v->blue.length=framebuffer_blue_mask_size();
    if (v->bits_per_pixel==32u) { v->transp.offset=24; v->transp.length=8; }
    v->height=0xffffffffu; v->width=0xffffffffu;
}
static void plasma_fb_fix(struct plasma_fb_fix_screeninfo *f) {
    bytes_zero(f,sizeof(*f)); const char id[]="Twilight fb0"; bytes_copy(f->id,id,sizeof(id));
    f->smem_start=framebuffer_physical_address(); f->smem_len=(uint32_t)framebuffer_size();
    f->type=0; f->visual=2; f->line_length=(uint32_t)framebuffer_pitch();
}
static int64_t plasma_fb_ioctl(uint64_t request, uint64_t arg) {
    if (!framebuffer_width() || !framebuffer_height() || framebuffer_bpp()!=32u) return -LINUX_ENODEV;
    if (request==FBIOGET_VSCREENINFO) { struct plasma_fb_var_screeninfo v; plasma_fb_var(&v); return user_copy_out(arg,&v,sizeof(v))?0:-LINUX_EFAULT; }
    if (request==FBIOGET_FSCREENINFO) { struct plasma_fb_fix_screeninfo f; plasma_fb_fix(&f); return user_copy_out(arg,&f,sizeof(f))?0:-LINUX_EFAULT; }
    if (request==FBIOPUT_VSCREENINFO) { struct plasma_fb_var_screeninfo v; if(!user_copy_in(&v,arg,sizeof(v)))return -LINUX_EFAULT;
        if(v.xres!=framebuffer_width()||v.yres!=framebuffer_height()||v.bits_per_pixel!=framebuffer_bpp())return -LINUX_EINVAL;
        plasma_fb_var(&v); return user_copy_out(arg,&v,sizeof(v))?0:-LINUX_EFAULT; }
    if (request==FBIOPAN_DISPLAY || request==FBIOBLANK) return 0;
    return -LINUX_ENOTTY;
}
static int64_t plasma_fb_mmap(uint64_t address,uint64_t length,uint64_t prot,uint64_t flags,uint64_t offset) {
    if(offset||!length||(prot&PROT_EXEC)) return -LINUX_EINVAL;
    uint64_t phys=framebuffer_physical_address(), size=framebuffer_size();
    if(!phys||!size||(phys&(TWILIGHT_PAGE_SIZE-1ull))) return -LINUX_ENODEV;
    if(length>size) return -LINUX_EINVAL;
    uint64_t ml=0; if(!align_up(length,&ml))return -LINUX_ENOMEM; uint64_t base=address;
    if((flags&MAP_FIXED)==0){base=image.mmap_next;if(!align_up(base,&base))return -LINUX_ENOMEM;}
    else if(base&(TWILIGHT_PAGE_SIZE-1ull)) return -LINUX_EINVAL;
    if(base>=SHELL_MMAP_LIMIT||ml>SHELL_MMAP_LIMIT-base)return -LINUX_ENOMEM;
    uint64_t pf=VMM_FLAG_USER|VMM_FLAG_WRITE; if(vmm_nx_supported())pf|=VMM_FLAG_NO_EXECUTE;
    uint64_t done=0; for(;done<ml;done+=TWILIGHT_PAGE_SIZE){uint64_t ip=0,ifl=0;
        if(vmm_translate(image.space,base+done,&ip,&ifl)||!vmm_map_page(image.space,base+done,phys+done,pf)){
            while(done){done-=TWILIGHT_PAGE_SIZE;(void)vmm_unmap_page(image.space,base+done,0);}return -LINUX_ENOMEM;}}
    if((flags&MAP_FIXED)==0)image.mmap_next=base+ml+TWILIGHT_PAGE_SIZE;
    serial_write("[linux:fbdev] mapped /dev/fb0 into userspace\n"); return (int64_t)base;
}
'''
    text = rep(text, "static int64_t sys_ioctl(uint64_t fd_value, uint64_t request, uint64_t argument) {\n",
               defs + "static int64_t sys_ioctl(uint64_t fd_value, uint64_t request, uint64_t argument) {\n")
    text = rep(text, "    const int fd = (int)fd_value;\n    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n",
               "    const int fd = (int)fd_value;\n    if (fd == PLASMA_FB_FD) return plasma_fb_ioctl(request, argument);\n    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n")
    text = rep(text, "        string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\") ||\n",
               "        string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\") ||\n        string_equal(path, \"/dev/fb0\") ||\n")
    text = rep(text, "    if (string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\"))\n        return fill_stat(stat_address, S_IFCHR | 0666u);\n",
               "    if (string_equal(path, \"/dev/tty\") || string_equal(path, \"/dev/null\") || string_equal(path, \"/dev/fb0\"))\n        return fill_stat(stat_address, S_IFCHR | 0666u);\n")
    text = rep(text, "    if (string_equal(path, \"/dev/null\")) return 4;\n",
               "    if (string_equal(path, \"/dev/null\")) return 4;\n    if (string_equal(path, \"/dev/fb0\")) return PLASMA_FB_FD;\n")
    text = rep(text, "        if (!fd_is_tty((int)a1) && (int)a1 != 4) return -LINUX_EBADF;\n        return fill_stat(a2, S_IFCHR | 0666u);\n",
               "        if ((int)a1 == PLASMA_FB_FD) return fill_stat(a2, S_IFCHR | 0666u);\n        if (!fd_is_tty((int)a1) && (int)a1 != 4) return -LINUX_EBADF;\n        return fill_stat(a2, S_IFCHR | 0666u);\n")
    text = rep(text, "    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n    switch (command) {\n",
               "    if (fd == PLASMA_FB_FD) { if (command==1||command==2||command==4) return 0; if(command==3)return 2; return 0; }\n    if (!fd_is_tty(fd)) return -LINUX_EBADF;\n    switch (command) {\n")
    p.write_text(text,encoding="utf-8"); print(f"Added runtime-compatible Twilight /dev/fb0 ABI: {p}"); return 0
if __name__ == "__main__":
    try: raise SystemExit(main())
    except Exception as e: print(f"ERROR: {e}",file=sys.stderr); raise SystemExit(1)
