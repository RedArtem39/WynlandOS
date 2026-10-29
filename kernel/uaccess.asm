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

; int uaccess_cmpxchg32_nofault(uint32_t *uaddr, uint32_t old, uint32_t new, uint32_t *cur)
;   the user word compare-exchanged (locked); *cur gets what it held. 0, or
;   -EFAULT for any fault (as uaccess_read32_nofault: for a PI futex word,
;   kernel/futex.c, under its spinlock)
global uaccess_cmpxchg32_nofault
global uaccess_cmpxchg_insn
uaccess_cmpxchg32_nofault:
    mov eax, esi
uaccess_cmpxchg_insn:
    lock cmpxchg [rdi], edx
    mov [rcx], eax
    xor eax, eax
    ret

; int uaccess_touch_write32(uint32_t *uaddr)
;   the user word made writable, its value unchanged (a locked OR with 0):
;   a lazy or copy-on-write page is resolved as for a copy. 0, or -EFAULT.
global uaccess_touch_write32
global uaccess_touch_insn
uaccess_touch_write32:
uaccess_touch_insn:
    lock or dword [rdi], 0
    xor eax, eax
    ret
