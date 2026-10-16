; ============================================================
; WynlandOS - Application processor start-up code
; ============================================================
; A core woken by INIT-SIPI-SIPI starts in 16-bit real mode at the
; physical page the SIPI names (vector << 12). kernel/smp.c copies this
; code to such a page below 1 MB, fills in the data at its end and wakes
; the cores one at a time. Steps: real mode -> 32-bit protected mode (a
; small GDT of its own) -> long mode with paging (PAE, EFER.LME and NXE:
; the kernel's page tables have NX bits) -> a jump into the kernel's
; ap_main(Cpu *) on the stack prepared for this core.
;
; Everything here runs at the copy's address, not where it was linked.

[BITS 16]
section .text

global ap_trampoline_start
global ap_trampoline_end
global ap_tr_cr3
global ap_tr_stack
global ap_tr_cpu
global ap_tr_entry

; The copy's address is only known at run time, so every absolute
; reference is relative to the copy's base, which real mode finds in CS
; (CS = base >> 4, IP = 0 after a SIPI).
ap_trampoline_start:
    cli
    cld
    mov ax, cs
    mov ds, ax                      ; DS:0 = the copy
    xor ebx, ebx
    mov bx, ax
    shl ebx, 4                      ; EBX = physical base of the copy

    ; the GDT descriptor's base: absolute, patched here
    lea eax, [ebx + (ap_tr_gdt - ap_trampoline_start)]
    mov [ap_tr_gdt_desc - ap_trampoline_start + 2], eax
    ; the far jump targets below: absolute too
    lea eax, [ebx + (ap_pm32 - ap_trampoline_start)]
    mov [ap_tr_pm32_ptr - ap_trampoline_start], eax
    lea eax, [ebx + (ap_lm64 - ap_trampoline_start)]
    mov [ap_tr_lm64_ptr - ap_trampoline_start], eax

    lgdt [ap_tr_gdt_desc - ap_trampoline_start]
    mov eax, cr0
    or eax, 1                       ; PE
    mov cr0, eax
    jmp dword far [ap_tr_pm32_ptr - ap_trampoline_start]

[BITS 32]
ap_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; EBX still holds the copy's base

    mov eax, cr4
    or eax, (1 << 5)                ; PAE
    mov cr4, eax
    mov eax, [ebx + (ap_tr_cr3 - ap_trampoline_start)]
    mov cr3, eax

    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11) | 1   ; LME, NXE, SCE
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 16) | 1  ; PG, WP, PE
    mov cr0, eax
    jmp far [ebx + (ap_tr_lm64_ptr - ap_trampoline_start)]

[BITS 64]
ap_lm64:
    mov ebx, ebx                    ; upper halves are undefined after the switch
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [rbx + (ap_tr_stack - ap_trampoline_start)]
    mov rdi, [rbx + (ap_tr_cpu - ap_trampoline_start)]
    mov rax, [rbx + (ap_tr_entry - ap_trampoline_start)]
    push 0                          ; a return address slot: ap_main never returns
    jmp rax

align 8
ap_tr_gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF           ; 0x10: data
    dq 0x00AF9A000000FFFF           ; 0x18: 64-bit code
ap_tr_gdt_desc:
    dw 31
    dd 0                            ; base: patched
    dw 0
ap_tr_pm32_ptr:
    dd 0                            ; offset: patched
    dw 0x08
    dw 0
ap_tr_lm64_ptr:
    dd 0
    dw 0x18
    dw 0
align 8
ap_tr_cr3:   dq 0                   ; a PML4 below 4 GB (32-bit CR3 load)
ap_tr_stack: dq 0
ap_tr_cpu:   dq 0
ap_tr_entry: dq 0
ap_trampoline_end:
