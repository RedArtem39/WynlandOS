; ============================================================
; WynlandOS - user memory copies that may fault
; ============================================================
; int uaccess_copy(void *dst, const void *src, uint64_t len)
;   0, or -EFAULT (-14) when a user page is not there and cannot be made
;   so. A page fault on the copy instruction goes to the fault handler as
;   usual (demand paging, copy on write: resolved, and the copy goes on);
;   one it cannot resolve returns here through uaccess_fixup instead of
;   being a kernel crash (kernel/idt.c). For syscalls outside the big
;   kernel lock (kernel/usercopy.c), which cannot look at page tables
;   another core may be changing.

[BITS 64]

section .text
global uaccess_copy
global uaccess_copy_insn
global uaccess_fixup

uaccess_copy:
    mov rcx, rdx
uaccess_copy_insn:
    rep movsb
    xor eax, eax
    ret

uaccess_fixup:
    mov rax, -14
    ret

; int uaccess_read32_nofault(const uint32_t *uaddr, uint32_t *out)
;   0, or -EFAULT for ANY fault -- not even a lazy page is faulted in
;   (kernel/idt.c sends it straight to the fixup): for code holding a
;   spinlock (the futex word, kernel/futex.c), which must not sleep or wait
;   for the big lock. The caller drops its lock, touches the word with
;   copy_from_user() and tries again.
global uaccess_read32_nofault
global uaccess_nofault_insn
global uaccess_nofault_fixup
uaccess_read32_nofault:
uaccess_nofault_insn:
    mov eax, [rdi]
    mov [rsi], eax
    xor eax, eax
    ret

uaccess_nofault_fixup:
    mov rax, -14
    ret
