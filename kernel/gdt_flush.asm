global gdt_flush
extern current_fx_user
%include "fpu.inc"

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
    mov rcx, [rel current_fx_user]   ; fresh program: default user FPU state
    FPU_RESTORE rcx
    
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

global thread_enter_user_mode_clone
thread_enter_user_mode_clone:
    ; RDI = pointer to SyscallRegs structure
    cli
    mov rcx, [rel current_fx_user]   ; the new thread's user FPU state (creator's copy)
    FPU_RESTORE rcx
    mov rbx, rdi ; Save pointer to regs in rbx (which is callee-saved, so wrmsr won't touch it!)

    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; 1. Restore TLS base from regs->r8 (offset 56 in SyscallRegs)
    mov rdx, [rbx + 56] ; rdx = regs->r8 (tls)
    mov rcx, 0xC0000100 ; IA32_FS_BASE MSR address
    mov rax, rdx
    shr rdx, 32         ; EDX = high 32 bits of tls
    wrmsr               ; writes EDX:EAX to MSR in RCX

    ; 2. Build iretq stack frame from regs->rsp, regs->rflags, regs->rip
    mov rax, [rbx + 120] ; regs->rsp
    push qword 0x1B      ; SS
    push rax             ; RSP
    mov rax, [rbx + 112] ; regs->rflags
    or rax, 0x200        ; Ensure interrupts are enabled (IF = 1)
    push rax             ; RFLAGS
    push qword 0x23      ; CS
    mov rax, [rbx + 104] ; regs->rip
    push rax             ; RIP

    ; 3. Restore all general-purpose registers from SyscallRegs
    mov r15, [rbx + 0]
    mov r14, [rbx + 8]
    mov r13, [rbx + 16]
    mov r12, [rbx + 24]
    mov r11, [rbx + 32]
    mov r10, [rbx + 40]
    mov r9,  [rbx + 48]
    mov r8,  [rbx + 56]
    mov rdx, [rbx + 64]
    mov rsi, [rbx + 72]
    mov rdi, [rbx + 80]
    ; Restore rbp and rbx at the end
    mov rbp, [rbx + 96]
    mov rbx, [rbx + 88]

    ; Clear RAX so clone returns 0 to the child thread
    xor rax, rax

    iretq

