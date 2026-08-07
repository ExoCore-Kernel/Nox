OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS {
    limine_requests PT_LOAD;
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0xffffffff80000000;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __twilight_text_start = .;
    .text : { *(.text .text.*) } :text
    __twilight_text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __twilight_rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(8);
        __twilight_pci_drivers_start = .;
        KEEP(*(.twilight_pci_drivers))
        __twilight_pci_drivers_end = .;
    } :rodata
    __twilight_rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) }
}
