/* Lays the program out like a 3DS title's code sets: text, rodata and data, each on its own page from 0x00100000. */
ENTRY(guest_main)
SECTIONS {
    . = 0x00100000;
    .text : { __text_start = .; *(.text.start) *(.text*) __text_end = .; }
    . = ALIGN(0x1000);
    .rodata : { __rodata_start = .; *(.rodata*) *(.ARM.extab*) *(.ARM.exidx*) __rodata_end = .; }
    . = ALIGN(0x1000);
    .data : { __data_start = .; *(.data*) __data_end = .; }
    .bss : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; }
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) }
}
