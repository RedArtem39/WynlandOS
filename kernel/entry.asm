; ============================================================
; WynlandOS - Kernel Entry Point
; ============================================================
; Phase 1: Minimal kernel bootstrap
;
; This is the first code that runs after the UEFI bootloader
; transfers control to the kernel. It bridges the calling
; convention gap between the UEFI bootloader (Microsoft x64 ABI,
; compiled with mingw) and the kernel C code (System V ABI,
; compiled with system gcc).
;
; The bootloader passes BootInfo* in RCX (MS ABI).
; The kernel expects it in RDI (System V ABI).
; ============================================================

[BITS 64]

; Kernel entry point - called from UEFI bootloader
; The bootloader passes BootInfo* in RCX (Microsoft x64 ABI)
; We need to bridge to System V ABI (RDI) for kernel_main

section .text.entry
global _kernel_start
global kernel_stack_top
global kernel_stack_bottom
extern kernel_main
extern __bss_start
extern __bss_end

_kernel_start:
    ; Save BootInfo pointer (passed in RCX from MS ABI bootloader)
    mov r15, rcx
    
    ; Disable interrupts until we set up IDT (Phase 2)
    cli
    
    ; Load GDT
    lgdt [rel gdt_descriptor]
    
    ; Reload data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Reload code segment (CS)
    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    
    ; Set up kernel stack
    lea rsp, [rel kernel_stack_top]
    
    ; Align stack to 16 bytes (ABI requirement)
    and rsp, ~0xF
    
    ; Zero the BSS section
    lea rdi, [rel __bss_start]
    lea rcx, [rel __bss_end]
    sub rcx, rdi
    test rcx, rcx
    jz .bss_done
    mov r8, rcx             ; Save total size
    shr rcx, 3              ; Size in qwords
    xor rax, rax
    rep stosq               ; Zero qwords, advances rdi
    mov rcx, r8
    and rcx, 7              ; Remaining bytes (0-7)
    rep stosb               ; Zero remaining bytes from current rdi
.bss_done:
    
    ; Call kernel_main(boot_info)
    ; System V ABI: first argument goes in RDI
    mov rdi, r15
    
    call kernel_main
    
    ; If kernel_main returns, halt the CPU
.halt:
    cli
    hlt
    jmp .halt

; ============================================================
; Global Descriptor Table (GDT)
; ============================================================
section .rodata
align 8
gdt_start:
    ; Null descriptor
    dq 0x0000000000000000
    
    ; Kernel Code (Selector 0x08)
    dq 0x002F9A000000FFFF
    
    ; Kernel Data (Selector 0x10)
    dq 0x000F92000000FFFF
    
    ; User Data (Selector 0x18)
    dq 0x000FF2000000FFFF
    
    ; User Code (Selector 0x20)
    dq 0x002FFA000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dq gdt_start

; ============================================================
; Kernel Stack (16 KB)
; ============================================================
section .bss
align 16
kernel_stack_bottom:
    resb 16384          ; 16 KB stack
kernel_stack_top:

; ============================================================
; Interrupt Service Routines (ISRs)
; ============================================================
section .text

extern exception_handler

isr_common_stub:
    ; Save all general purpose registers (r15-r8, rbp, rdi, rsi, rdx, rcx, rbx, rax)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; First argument to System V ABI is RDI (pointer to registers on stack)
    mov rdi, rsp

    ; Call the C exception handler
    call exception_handler

    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Pop exception number and error code
    add rsp, 16

    ; Return from interrupt
    iretq

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0         ; Dummy error code
    push qword %1        ; Exception number
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    ; Error code is already pushed by CPU
    push qword %1        ; Exception number
    jmp isr_common_stub
%endmacro

; Generate all 32 exception handlers
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_ERRCODE   29
ISR_ERRCODE   30
ISR_NOERRCODE 31

; ============================================================
; Hardware Interrupt Service Routines (IRQs)
; ============================================================

extern irq_handler

irq_common_stub:
    ; Save all general purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; First argument to System V ABI is RDI (pointer to registers on stack)
    mov rdi, rsp

    ; Call the C IRQ handler
    call irq_handler

    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Pop interrupt vector and dummy error code
    add rsp, 16

    ; Return from interrupt
    iretq

%macro IRQ_STUB 2
global irq%1
irq%1:
    push qword 0         ; Dummy error code
    push qword %2        ; Interrupt vector (0x20 + IRQ)
    jmp irq_common_stub
%endmacro

; Generate IRQ handlers 0..15 mapped to vectors 32..47
IRQ_STUB 0,  32
IRQ_STUB 1,  33
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47
