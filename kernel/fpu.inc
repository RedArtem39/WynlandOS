; WynlandOS - saving and restoring a thread's FPU/SSE/AVX registers
; ============================================================
; XSAVE/XRSTOR (x87 + SSE + AVX: whatever XCR0 enables) when the CPU has
; it, FXSAVE/FXRSTOR (x87 + SSE only) otherwise; kernel/sched.c's
; fpu_init() decides at boot (g_fpu_xsave). The area is FPU_AREA_MAX
; bytes, 64-byte aligned (include/wynland/sched.h).
;
; FPU_SAVE reg / FPU_RESTORE reg: reg holds the area's address.
; Clobbers RAX, RDX and the flags.

extern g_fpu_xsave

%macro FPU_SAVE 1
    cmp byte [rel g_fpu_xsave], 0
    je %%fx
    mov eax, -1                 ; every component XCR0 enables
    mov edx, -1
    xsave64 [%1]
    jmp %%done
%%fx:
    fxsave64 [%1]
%%done:
%endmacro

%macro FPU_RESTORE 1
    cmp byte [rel g_fpu_xsave], 0
    je %%fx
    mov eax, -1
    mov edx, -1
    xrstor64 [%1]
    jmp %%done
%%fx:
    fxrstor64 [%1]
%%done:
%endmacro
