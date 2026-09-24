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
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note.GNU-stack)
        *(.note.gnu.*)
        *(.comment)
    }
}
