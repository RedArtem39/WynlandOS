/* The vDSO's layout (Linux's arch/x86/entry/vdso/vdso-layout.lds.S, cut
   down): one PT_LOAD from address 0 with the dynamic tables glibc reads,
   the data page the page before it, nothing writable, no relocations. */

SECTIONS
{
    vvar_page = . - 4096;

    . = SIZEOF_HEADERS;

    .hash           : { *(.hash) }                  :text
    .gnu.hash       : { *(.gnu.hash) }
    .dynsym         : { *(.dynsym) }
    .dynstr         : { *(.dynstr) }
    .gnu.version    : { *(.gnu.version) }
    .gnu.version_d  : { *(.gnu.version_d) }
    .gnu.version_r  : { *(.gnu.version_r) }

    .dynamic        : { *(.dynamic) }               :text :dynamic

    .rodata         : { *(.rodata*) }               :text
    .note           : { *(.note.*) }                :text :note

    .text           : { *(.text*) }                 :text

    /DISCARD/ : {
        *(.data*) *(.bss*) *(.got*) *(.plt*) *(.comment) *(.note.GNU-stack) *(.eh_frame*)
    }
}

PHDRS
{
    text         PT_LOAD FLAGS(5) FILEHDR PHDRS;   /* R E */
    dynamic      PT_DYNAMIC FLAGS(4);              /* R */
    note         PT_NOTE FLAGS(4);
}

VERSION
{
    LINUX_2.6 {
    global:
        clock_gettime; __vdso_clock_gettime;
        gettimeofday;  __vdso_gettimeofday;
        time;          __vdso_time;
    local: *;
    };
}
