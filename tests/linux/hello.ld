ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
}

SECTIONS
{
    . = 0x0000000000400000;
    .text : ALIGN(16)
    {
        *(.text*)
        *(.rodata*)
    } :text

    /DISCARD/ :
    {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
