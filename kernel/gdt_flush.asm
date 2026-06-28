global gdt_flush

section .text
gdt_flush:
    lgdt [rdi]        ; Load the new GDT pointer passed in RDI
    
    ; Reload segment registers
    mov ax, 0x10      ; Kernel Data descriptor index (0x10)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Far return to reload CS register (0x08 for Kernel Code)
    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq             ; Perform a 64-bit far return to change CS segment
    
.reload_cs:
    ret

global thread_enter_user_mode
thread_enter_user_mode:
    ; RDI = user_entry
    ; RSI = user_stack
    cli
    
    ; Reload data segment registers with User Data selector (0x18 | RPL 3 = 0x1B)
    ; This is required to satisfy hardware virtualization checks (DPL must match CPL) on WHPX/VT-x.
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push 0x1B                ; SS (User Data 0x18 | RPL 3)
    push rsi                 ; RSP
    push 0x3202              ; RFLAGS (Interrupts enabled, IOPL = 3)
    push 0x23                ; CS (User Code 0x20 | RPL 3)
    push rdi                 ; RIP
    iretq

