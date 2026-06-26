; ============================================================
; WynlandOS - Context Switch and Thread Trampoline
; ============================================================

[BITS 64]

section .text
global context_switch
global thread_trampoline
extern thread_exit

; void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
; System V ABI: RDI = old_rsp, RSI = new_rsp
context_switch:
    ; Push callee-saved registers
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save old RSP to the address in RDI
    mov [rdi], rsp

    ; Load new RSP from RSI
    mov rsp, rsi

    ; Pop callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ret

; Trampoline to start a new thread
; R12 = thread entry point function
; R13 = argument to pass to thread entry point
thread_trampoline:
    ; Align stack and call entry function (arg in RDI)
    mov rdi, r13
    call r12

    ; If the entry function returns, exit the thread
    call thread_exit
    
    ; Should never reach here
.hang:
    hlt
    jmp .hang
