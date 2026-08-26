global syscall_entry
extern syscall_dispatcher
extern current_kernel_stack
extern signal_deliver_check

section .data
user_stack_temp: dq 0

section .text
syscall_entry:
    ; 1. Save user stack and switch to kernel stack
    mov [rel user_stack_temp], rsp
    mov rsp, [rel current_kernel_stack]

    ; ABI-alignment pad: 16 GPR pushes + 1 "push rsp" (7th arg) below is an
    ; ODD number of qword pushes, which leaves RSP at (16-aligned - 8) right
    ; before `call syscall_dispatcher` -- violating the SysV requirement that
    ; RSP be 16-aligned immediately before a CALL. This single extra qword
    ; restores that, so callees relying on ABI-guaranteed alignment (e.g.
    ; GCC-emitted `movaps` stack spills) don't fault. Popped back out below,
    ; symmetrically, before restoring user RIP/RFLAGS/RSP.
    sub rsp, 8

    ; 2. Push user state (RIP, RFLAGS, RSP) onto the kernel stack
    push qword [rel user_stack_temp] ; User RSP
    push r11                         ; User RFLAGS
    push rcx                         ; User RIP
    
    ; Save all caller/callee-saved registers
    push rbp
    push rbx
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; 3. Setup arguments for syscall_dispatcher(num, a1, a2, a3, a4, a5)
    mov r9, r8         ; a5 (User R8 -> R9)
    mov r8, r10        ; a4 (User R10 -> R8)
    mov rcx, rdx       ; a3 (User RDX -> RCX)
    mov rdx, rsi       ; a2 (User RSI -> RDX)
    mov rsi, rdi       ; a1 (User RDI -> RSI)
    mov rdi, rax       ; num (User RAX -> RDI)
    
    push rsp           ; 7th argument: SyscallRegs* (on stack)
    call syscall_dispatcher
    add rsp, 8         ; Clean up 7th argument

    ; 3b. Phase 22b signal delivery point. Runs after the dispatcher, before
    ; the saved user context is popped back -- so it can patch that context
    ; in place and sysret lands straight in a signal handler. Only RDI/RBX/
    ; RAX/EFLAGS are touched here; every other register is dead anyway
    ; (popped back from the struct below). On delivery, live RAX keeps the
    ; dispatcher's return value -- meaningless inside a handler (Linux also
    ; enters handlers with syscall-result garbage in RAX), documented.
    mov rbx, rax              ; stash syscall return value
    mov rdi, rsp              ; SyscallRegs*
    call signal_deliver_check
    mov rax, rbx              ; syscall result back in RAX either way

    ; 4. Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi
    pop rbx
    pop rbp

    ; Note: no compensating `add rsp,8` is needed for the alignment pad
    ; pushed above -- the kernel stack pointer is unconditionally
    ; re-initialized from current_kernel_stack on the next syscall entry
    ; (see top of this function), and `pop rsp` below immediately switches
    ; RSP back to the user stack anyway, so the pad qword is simply
    ; abandoned along with the rest of this frame.

    ; 5. Restore user execution context
    pop rcx                          ; Restore User RIP
    pop r11                          ; Restore User RFLAGS
    pop rsp                          ; Restore User RSP
    
    ; 6. Return to User Mode (Ring 3)
    o64 sysret
