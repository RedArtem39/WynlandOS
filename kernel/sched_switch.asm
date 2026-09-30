; ============================================================
; WynlandOS - Context Switch and Thread Trampoline
; ============================================================

[BITS 64]

section .text
global context_switch
global thread_trampoline
global fork_child_entry
extern thread_exit
extern current_fx_user

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

; Real fork() child entry point. Called via thread_trampoline exactly like
; any other thread (RDI = arg, per thread_trampoline's "mov rdi, r13; call
; r12" -- this function is passed to thread_create_ex() as the entry
; function, so the standard SysV calling convention applies: RDI holds the
; single void* argument).
;
; RDI = pointer to a heap-allocated copy of the parent's SyscallRegs (see
; kernel/syscall.c) -- the exact r15..rsp state the parent had at the
; moment of the fork() syscall. Restores all of it into the real registers,
; then replays syscall_entry.asm's own return-to-userspace tail (steps
; 4-6 there) so the child resumes at the SAME instruction the parent will
; also return to, on the SAME (now copied, per-process) user stack --
; except RAX is forced to 0, which is fork()'s whole point: the parent's
; own C return path already returns the child's real pid normally, exactly
; like any other syscall.
;
; By the time this runs, the scheduler has already switched CR3 to this
; thread's own Process (see sched_schedule()'s CR3-switch-on-proc-change
; check, keyed off Thread.proc -- set correctly by thread_create_ex()
; before this thread could ever be picked), so RCX (the parent's captured
; RIP, a virtual address) resolves through the CHILD's own page tables,
; which vmm_clone_user_pages() has already populated with a copy of every
; user page at the same virtual addresses.
;
; SyscallRegs field offsets (kernel/syscall.c): r15=0,r14=8,r13=16,r12=24,
; r11=32,r10=40,r9=48,r8=56,rdx=64,rsi=72,rdi=80,rbx=88,rbp=96,rip=104,
; rflags=112,rsp=120.
fork_child_entry:
    ; the child's user x87/SSE state (a copy of the parent's, see
    ; thread_create_ex_tls()) before going back to user mode
    mov rax, [rel current_fx_user]
    fxrstor64 [rax]
    mov rax, rdi             ; rax = base pointer to the copied SyscallRegs

    mov r15, [rax + 0]
    mov r14, [rax + 8]
    mov r13, [rax + 16]
    ; r12 (offset 24) restored last -- rax still needs to serve as the base
    ; pointer for every load up to that point.
    mov r11, [rax + 32]      ; matches syscall_entry's own redundant 2nd r11 push; overwritten below with the real saved rflags
    mov r10, [rax + 40]
    mov r9,  [rax + 48]
    mov r8,  [rax + 56]
    mov rdx, [rax + 64]
    mov rsi, [rax + 72]
    mov rdi, [rax + 80]
    mov rbx, [rax + 88]
    mov rbp, [rax + 96]
    mov rcx, [rax + 104]     ; saved RIP -> RCX, sysret's return-address source
    mov r11, [rax + 112]     ; saved RFLAGS -> R11, sysret's RFLAGS source
    mov rsp, [rax + 120]     ; saved user RSP -- child's own copy, same virtual address as the parent's
    mov r12, [rax + 24]      ; now safe: rax (the base pointer) is no longer needed after this
    xor eax, eax              ; RAX = 0: this is fork()'s return value in the child
    o64 sysret
