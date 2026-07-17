global syscall_entry
extern syscall_dispatcher
extern current_kernel_stack

section .data
user_stack_temp: dq 0

section .text
syscall_entry:
    ; 1. Save user stack and switch to kernel stack
    mov [rel user_stack_temp], rsp
    mov rsp, [rel current_kernel_stack]
    
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
    
    ; 5. Restore user execution context
    pop rcx                          ; Restore User RIP
    pop r11                          ; Restore User RFLAGS
    pop rsp                          ; Restore User RSP
    
    ; 6. Return to User Mode (Ring 3)
    o64 sysret
