/* Link layout for Vexa programs: loaded at 4 MiB, with the Vexa note in its
 * own PT_NOTE segment so the kernel can recognize the program. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FILEHDR PHDRS;
    rodata PT_LOAD;
    data   PT_LOAD;
    note   PT_NOTE;
}

SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;

    .note.vexa : { KEEP(*(.note.vexa)) } :text :note
    .text : { *(.text .text.*) } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data
    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note.GNU-stack)
        *(.note.gnu.*)
        *(.comment)
    }
}
