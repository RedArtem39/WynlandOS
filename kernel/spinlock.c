/*
 * WynlandOS - Spinlocks (include/wynland/spinlock.h)
 */
#include <wynland/spinlock.h>
#include <wynland/smp.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void uint_to_hex(uint64_t val, char *buf);
extern uint64_t net_tsc_ms(void);

void spin_init(Spinlock *l, const char *name)
{
    l->locked = 0;
    l->cpu = -1;
    l->name = name;
    l->ip = 0;
}

static void spin_report(const Spinlock *l, const char *what)
{
    char b[24];
    serial_write_string("SPIN: ");
    serial_write_string(what);
    serial_write_string(" '");
    serial_write_string(l->name ? l->name : "?");
    serial_write_string("' on cpu ");
    uint_to_str(this_cpu()->id, b); serial_write_string(b);
    serial_write_string(", held by cpu ");
    uint_to_str((uint64_t)(int64_t)l->cpu, b); serial_write_string(b);
    serial_write_string(" from ");
    uint_to_hex((uint64_t)(uintptr_t)l->ip, b); serial_write_string(b);
    serial_write_string("\r\n");
}

static inline bool try_take(Spinlock *l)
{
    return !__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE);
}

static __attribute__((noinline)) void spin_slow(Spinlock *l)
{
    if (l->cpu == (int32_t)this_cpu()->id) spin_report(l, "taken again by its holder");
    uint64_t per_ms = net_tsc_ms();
    uint64_t t0 = __builtin_ia32_rdtsc();
    bool said = false;
    do {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) {
            __builtin_ia32_pause();
            smp_tlb_poll();
            if (!said && per_ms && __builtin_ia32_rdtsc() - t0 > per_ms * 1000) {
                spin_report(l, "spinning for a second on");
                said = true;
            }
        }
    } while (!try_take(l));
}

void spin_lock(Spinlock *l)
{
    if (!try_take(l)) spin_slow(l);
    l->cpu = (int32_t)this_cpu()->id;
    l->ip = __builtin_return_address(0);
}

bool spin_trylock(Spinlock *l)
{
    if (!try_take(l)) return false;
    l->cpu = (int32_t)this_cpu()->id;
    l->ip = __builtin_return_address(0);
    return true;
}

void spin_unlock(Spinlock *l)
{
    if (l->cpu != (int32_t)this_cpu()->id) spin_report(l, "released by a core that does not hold");
    l->cpu = -1;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irqsave(Spinlock *l)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    if (!try_take(l)) spin_slow(l);
    l->cpu = (int32_t)this_cpu()->id;
    l->ip = __builtin_return_address(0);
    return f;
}

void spin_unlock_irqrestore(Spinlock *l, uint64_t flags)
{
    spin_unlock(l);
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
}

bool spin_held(const Spinlock *l)
{
    return l->locked && l->cpu == (int32_t)this_cpu()->id;
}
