; WynlandOS - per-CPU data offsets (struct Cpu, include/wynland/smp.h)
; ============================================================
; In the kernel GS points at the running core's struct Cpu; the entry
; stubs swapgs on every transition from and to user mode.

%define CPU_USER_RSP  8
%define CPU_KSTACK    16
%define CPU_FX_USER   24
