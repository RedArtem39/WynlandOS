/*
 * WynlandOS - real POSIX-style signals (Phase 22b)
 *
 * Delivery model: signals become PENDING on a thread; the single delivery
 * point is the tail of every syscall (kernel/syscall_entry.asm calls
 * signal_deliver_check() right after syscall_dispatcher returns, before the
 * saved user context is popped back). Delivery patches that saved context
 * in place -- user RIP becomes the handler, user RSP gets a pretcode slot,
 * RDI carries the signal number -- so `sysret` lands directly in the
 * handler; its RET goes to sa_restorer, which issues SYS_rt_sigreturn, and
 * the kernel restores the interrupted context from a KERNEL-SIDE copy of
 * the frame (never re-parsed from user memory -- simpler and abuse-proof,
 * documented as a deliberate divergence from Linux's user-side frame).
 *
 * Threads that never issue another syscall never observe pending signals;
 * acceptable v1 boundary (every real program syscalls constantly).
 *
 * Scope notes, honestly bounded: handlers are per-process, masks/pending
 * per-thread; sa_mask is parsed-but-not-applied (no nested-signal masking
 * yet); stop/continue signals are ignored; default actions are the classic
 * terminate set plus ignore-for-{PIPE,CHLD}; hardware-origin signals (a
 * page fault raising SIGSEGV) are NOT wired yet -- only syscall-raised ones
 * (tgkill/tkill, i.e. raise()/abort()) reach the pipeline this phase.
 *
 * SyscallRegs is kernel/syscall.c's private typedef (anonymous struct);
 * this header deliberately speaks in void* so it stays includable
 * everywhere without moving that definition.
 */
#pragma once

#include <wynland/types.h>

void signal_init(void);

/* Called from the syscall-return tail. Returns 1 when it patched the
   saved-context block (*regs, a SyscallRegs* in disguise) for immediate
   handler entry -- caller must then load live RAX from offset 0 of the
   block's rax slot (pre-signal user value) instead of the syscall result.
   Returns 0 normally. */
int signal_deliver_check(void *regs);

/* SYS_rt_sigaction(13): act_ptr/oldact_ptr are kernel_sigaction
   {handler,flags,restorer,mask}; sigsetsize must be 8. */
uint64_t signal_do_sigaction(int sig, uint64_t act_ptr, uint64_t oldact_ptr);

/* SYS_rt_sigprocmask(14): how = {0 BLOCK, 1 UNBLOCK, 2 SET}. */
uint64_t signal_do_procmask(int how, uint64_t set_ptr, uint64_t oldset_ptr);

/* SYS_rt_sigreturn(15): restores the interrupted context from the thread's
   kernel-side frame copy; returns the saved user RAX (the dispatcher's
   return register IS the syscall-result channel on this kernel). */
uint64_t signal_rt_return(void *regs);

/* Mark a signal pending on the CALLING thread (raise()/tgkill-on-self). */
void signal_raise_current(int sig);

/* Mark a signal pending on the thread with id `tid`, wherever it is in the
   system -- real SYS_tkill/SYS_tgkill target a specific thread by id, not
   necessarily the caller (kernel/syscall.c's case 200/234 previously always
   called signal_raise_current() instead, silently ignoring the tid/tgid
   arguments whenever they named anyone but the caller). If `tgid` is
   nonzero, delivery is refused (false, no-op) unless the target thread's
   owning process's pid matches it, matching real tgkill(2)'s "signal only
   this thread if it's actually in thread group tgid" semantics; pass 0 for
   plain tkill(2), which has no thread-group argument to check against.
   Returns false if no thread with that id currently exists. */
bool signal_raise_thread(uint64_t tid, uint64_t tgid, int sig);
