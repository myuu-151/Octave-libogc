/* GameCube apploader link script.
 * The IPL loads the apploader body to 0x81200000 and calls its entry function,
 * which must sit at the very start -- so .text.entry (app_entry) is placed first.
 */
OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc");
OUTPUT_ARCH(powerpc:common);
ENTRY(app_entry);

SECTIONS
{
    . = 0x81200000;

    .text : {
        KEEP(*(.text.entry))        /* app_entry -- must be at 0x81200000 */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.sdata2 .sdata2.*)
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(32);
    }

    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(32);
    }
    __bss_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
        *(.gnu.attributes)
    }
}
