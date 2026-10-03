/*
 * WynlandOS - memfd_create() and timerfd objects.
 *
 * memfd: an anonymous, resizable file made of individually allocated
 * physical pages (not one contiguous block like SYS_shm_create's
 * segments, so a large memfd never needs a large contiguous run). The
 * memfd holds one pmm reference per page; every MAP_SHARED mapping takes
 * its own reference per page (kernel/syscall.c's mmap branch) and maps
 * them PAGE_SHARED_MAP | PAGE_SHARED_REF, so pages outlive a ftruncate()
 * shrink or the last close for as long as some process still maps them.
 * munmap()/process teardown drop a PAGE_SHARED_REF mapping's reference and
 * fork() takes one for the child, so the frames are freed with the last
 * of (memfd, mappings).
 * Growing after mapping is fine (existing mappings keep their pages);
 * shrinking doesn't revoke existing mappings of the cut-off pages (Linux
 * would SIGBUS on access there) -- documented gap, nobody relies on it.
 *
 * timerfd: expirations are computed lazily from the 1 kHz clock whenever
 * the timer is read or polled; a blocked read() sleeps until the next
 * expiry, and do_poll() clamps its own sleep to that expiry (it can't be
 * woken by the timer itself -- nothing fires an interrupt for it).
 */
#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/pmm.h>
#include <wynland/sched.h>
#include <wynland/unix_socket.h>

extern uint64_t timer_get_ms(void);

#define EAGAIN 11
#define ENOMEM 12
#define EINVAL 22
#define EFBIG  27
#define ENFILE 23

#define MEMFD_SLOTS     256
#define MEMFD_MAX_PAGES (1u << 18)   /* 1 GiB per memfd */

typedef struct {
    int       refs;
    uint64_t  size;
    uint32_t  npages;     /* pages currently owned (== ceil(size / 4K)) */
    uint32_t  cap;        /* capacity of pages[] */
    uint64_t *pages;      /* physical addresses */
    char      name[64];
} MemFd;

static MemFd *g_memfd[MEMFD_SLOTS];

static MemFd *mslot(int idx) {
    if (idx < 0 || idx >= MEMFD_SLOTS) return NULL;
    return g_memfd[idx];
}

int memfd_new(const char *name) {
    for (int i = 0; i < MEMFD_SLOTS; i++) {
        if (g_memfd[i]) continue;
        MemFd *m = (MemFd *)kmalloc(sizeof(MemFd));
        if (!m) return -ENOMEM;
        memset(m, 0, sizeof(MemFd));
        m->refs = 1;
        int k = 0;
        if (name) while (name[k] && k < (int)sizeof(m->name) - 1) { m->name[k] = name[k]; k++; }
        m->name[k] = '\0';
        g_memfd[i] = m;
        return i;
    }
    return -ENFILE;
}

void memfd_ref(int idx) {
    MemFd *m = mslot(idx);
    if (m) m->refs++;
}

void memfd_unref(int idx) {
    MemFd *m = mslot(idx);
    if (!m) return;
    if (--m->refs > 0) return;
    for (uint32_t i = 0; i < m->npages; i++) pmm_free_page((void *)(uintptr_t)m->pages[i]);
    if (m->pages) kfree(m->pages);
    kfree(m);
    g_memfd[idx] = NULL;
}

int64_t memfd_truncate(int idx, uint64_t size) {
    MemFd *m = mslot(idx);
    if (!m) return -EINVAL;
    uint64_t want = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (want > MEMFD_MAX_PAGES) return -EFBIG;

    if (want > m->cap) {
        uint32_t ncap = m->cap ? m->cap : 16;
        while (ncap < want) ncap *= 2;
        uint64_t *np = (uint64_t *)kmalloc((size_t)ncap * sizeof(uint64_t));
        if (!np) return -ENOMEM;
        for (uint32_t i = 0; i < m->npages; i++) np[i] = m->pages[i];
        if (m->pages) kfree(m->pages);
        m->pages = np;
        m->cap = ncap;
    }
    while (m->npages < want) {
        void *p = pmm_alloc_page();
        if (!p) return -ENOMEM; /* size stays at what we could back */
        memset(p, 0, PAGE_SIZE); /* physical RAM is identity-mapped in every PML4 */
        m->pages[m->npages++] = (uint64_t)(uintptr_t)p;
    }
    while (m->npages > want) {
        pmm_free_page((void *)(uintptr_t)m->pages[--m->npages]);
    }
    /* zero the tail of a partial last page so a later grow reads zeros */
    if (size < m->size && (size % PAGE_SIZE) && m->npages > 0) {
        uint8_t *last = (uint8_t *)(uintptr_t)m->pages[m->npages - 1];
        memset(last + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
    }
    m->size = size;
    return 0;
}

uint64_t memfd_size(int idx) {
    MemFd *m = mslot(idx);
    return m ? m->size : 0;
}

uint64_t memfd_page_phys(int idx, uint64_t page_index) {
    MemFd *m = mslot(idx);
    if (!m || page_index >= m->npages) return 0;
    return m->pages[page_index];
}

int64_t memfd_pread(int idx, uint64_t off, void *buf, uint64_t len) {
    MemFd *m = mslot(idx);
    if (!m) return -EINVAL;
    if (off >= m->size) return 0;
    if (len > m->size - off) len = m->size - off;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t in = pos % PAGE_SIZE;
        uint64_t take = PAGE_SIZE - in;
        if (take > len - done) take = len - done;
        memcpy((uint8_t *)buf + done, (uint8_t *)(uintptr_t)m->pages[pos / PAGE_SIZE] + in, (size_t)take);
        done += take;
    }
    return (int64_t)done;
}

int64_t memfd_pwrite(int idx, uint64_t off, const void *buf, uint64_t len) {
    MemFd *m = mslot(idx);
    if (!m) return -EINVAL;
    if (len == 0) return 0;
    if (off + len > m->size) {
        int64_t r = memfd_truncate(idx, off + len);
        if (r < 0) return r;
    }
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t in = pos % PAGE_SIZE;
        uint64_t take = PAGE_SIZE - in;
        if (take > len - done) take = len - done;
        memcpy((uint8_t *)(uintptr_t)m->pages[pos / PAGE_SIZE] + in, (const uint8_t *)buf + done, (size_t)take);
        done += take;
    }
    return (int64_t)done;
}

/* ---------------------------------------------------------------- */

#define TIMERFD_SLOTS 64

typedef struct {
    int       refs;
    int       clockid;
    uint64_t  next_ms;      /* absolute monotonic ms of the next expiry; 0 = disarmed */
    uint64_t  interval_ms;
    uint64_t  pending;      /* expirations not yet read */
    WaitQueue wq;
} TimerFd;

static TimerFd *g_timerfd[TIMERFD_SLOTS];

static TimerFd *tslot(int idx) {
    if (idx < 0 || idx >= TIMERFD_SLOTS) return NULL;
    return g_timerfd[idx];
}

static void timer_update(TimerFd *t) {
    if (!t->next_ms) return;
    uint64_t now = timer_get_ms();
    if (now < t->next_ms) return;
    if (t->interval_ms) {
        uint64_t n = 1 + (now - t->next_ms) / t->interval_ms;
        t->pending += n;
        t->next_ms += n * t->interval_ms;
    } else {
        t->pending++;
        t->next_ms = 0;
    }
}

int timerfd_new(int clockid) {
    for (int i = 0; i < TIMERFD_SLOTS; i++) {
        if (g_timerfd[i]) continue;
        TimerFd *t = (TimerFd *)kmalloc(sizeof(TimerFd));
        if (!t) return -ENOMEM;
        memset(t, 0, sizeof(TimerFd));
        t->refs = 1;
        t->clockid = clockid;
        g_timerfd[i] = t;
        return i;
    }
    return -ENFILE;
}

void timerfd_ref(int idx) {
    TimerFd *t = tslot(idx);
    if (t) t->refs++;
}

void timerfd_unref(int idx) {
    TimerFd *t = tslot(idx);
    if (!t) return;
    if (--t->refs > 0) return;
    kfree(t);
    g_timerfd[idx] = NULL;
}

int timerfd_clock(int idx) {
    TimerFd *t = tslot(idx);
    return t ? t->clockid : -1;
}

void timerfd_gettime(int idx, uint64_t *value_ms, uint64_t *interval_ms) {
    TimerFd *t = tslot(idx);
    *value_ms = 0; *interval_ms = 0;
    if (!t) return;
    timer_update(t);
    uint64_t now = timer_get_ms();
    if (t->next_ms) *value_ms = t->next_ms > now ? t->next_ms - now : 1;
    *interval_ms = t->interval_ms;
}

int64_t timerfd_settime(int idx, uint64_t value_ms, uint64_t interval_ms, bool abs,
                        uint64_t *old_value_ms, uint64_t *old_interval_ms) {
    TimerFd *t = tslot(idx);
    if (!t) return -EINVAL;
    if (old_value_ms) timerfd_gettime(idx, old_value_ms, old_interval_ms);
    t->pending = 0;
    t->interval_ms = interval_ms;
    if (value_ms == 0) {
        t->next_ms = 0;
    } else {
        uint64_t now = timer_get_ms();
        t->next_ms = abs ? value_ms : now + value_ms;
        if (t->next_ms == 0) t->next_ms = 1; /* 0 means disarmed */
    }
    waitqueue_wake_all(&t->wq);
    waitqueue_wake_all(&g_poll_any_wq);
    return 0;
}

int64_t timerfd_read(int idx, uint64_t *count, bool nonblock) {
    TimerFd *t = tslot(idx);
    if (!t) return -EINVAL;
    t->refs++;
    int64_t ret;
    for (;;) {
        timer_update(t);
        if (t->pending) {
            *count = t->pending;
            t->pending = 0;
            ret = 8;
            break;
        }
        if (nonblock) { ret = -EAGAIN; break; }
        if (sched_dying()) { ret = -4; break; }   /* -EINTR: dying */
        /* next_ms 0 == SCHED_NO_DEADLINE: sleep until a settime() wakes us */
        waitqueue_wait_ms(&t->wq, t->next_ms);
    }
    timerfd_unref(idx);
    return ret;
}

uint32_t timerfd_poll(int idx, WaitQueue **wq, uint64_t *wake_ms) {
    TimerFd *t = tslot(idx);
    if (!t) return UPOLLERR;
    timer_update(t);
    *wq = &t->wq;
    *wake_ms = t->next_ms;
    return t->pending ? UPOLLIN : 0;
}
