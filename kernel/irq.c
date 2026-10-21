/*
 * WynlandOS - Interrupt Handlers (IRQs) Implementation
 */

#include <wynland/irq.h>
#include <wynland/process.h>
#include <wynland/sched.h>
#include <wynland/smp.h>


#define PIC1          0x20
#define PIC2          0xA0
#define PIC1_COMMAND  PIC1
#define PIC1_DATA     (PIC1+1)
#define PIC2_COMMAND  PIC2
#define PIC2_DATA     (PIC2+1)
#define PIC_EOI       0x20

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43

extern void serial_write_string(const char *str);

static inline uint8_t inb(uint16_t port)
{
    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static inline void outb(uint16_t port, uint8_t data)
{
    __asm__ volatile("outb %0, %1" :: "a"(data), "Nd"(port));
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* The PIT runs at TIMER_HW_HZ (1 kHz) so sleeps, frame pacing and
   preemption get millisecond resolution -- 100 Hz capped every
   nanosleep()-paced loop (the compositor included) at <=100 FPS with
   10ms jitter. timer_ticks deliberately KEEPS its historical 100 Hz unit
   (every 10th hardware tick): futex/poll/pipe/TCP deadlines, the RTC
   and clock code all count in it. timer_ms is the new 1 kHz clock. */
#define TIMER_HW_HZ        1000
#define TIMER_MS_PER_TICK  10   /* TIMER_HW_HZ / 100 */

/* Timer global tick count (100 Hz) and millisecond clock (1 kHz) */
static volatile uint64_t timer_ticks = 0;
static volatile uint64_t timer_ms = 0;

/* Keyboard ring buffer */
#define KEYBOARD_BUFFER_SIZE 256
static volatile uint8_t kbd_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

bool keyboard_has_scancode(void)
{
    return kbd_head != kbd_tail;
}

uint8_t keyboard_pop_scancode(void)
{
    if (kbd_head == kbd_tail) {
        return 0;
    }
    uint8_t scancode = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KEYBOARD_BUFFER_SIZE;
    return scancode;
}

static void keyboard_push_scancode(uint8_t scancode)
{
    uint32_t next = (kbd_head + 1) % KEYBOARD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = scancode;
        kbd_head = next;
    }
}

/* US layout, unshifted: the console's line input (/dev/tty, a stdin
   that was never opened) */
const char scancode_to_ascii_lower[59] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

/* The display server's keyboard (/dev/input/kbd), apart from kbd_buffer
   above (the console's). While the display server reads it, keys are its
   alone: the console queue gets nothing, so what is typed in a window
   never reaches a program reading the console. */
static volatile uint64_t g_kbd_raw_read_ms;   /* last read of /dev/input/kbd */
#define KBD_OWNER_MS 1000
#define KBD_RAW_QUEUE_SIZE 256
static uint8_t kbd_raw_queue[KBD_RAW_QUEUE_SIZE];
static uint32_t kbd_raw_queue_head = 0;
static uint32_t kbd_raw_queue_tail = 0;

void kbd_raw_queue_push(uint8_t scancode)
{
    uint32_t next = (kbd_raw_queue_head + 1) % KBD_RAW_QUEUE_SIZE;
    if (next != kbd_raw_queue_tail) {
        kbd_raw_queue[kbd_raw_queue_head] = scancode;
        kbd_raw_queue_head = next;
    }
}

/* A USB HID keyboard's scancodes enter the same queues as the PS/2 one. */
void keyboard_inject_scancode(uint8_t scancode)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    if (!g_kbd_raw_read_ms || timer_ms - g_kbd_raw_read_ms > KBD_OWNER_MS)
        keyboard_push_scancode(scancode);
    kbd_raw_queue_push(scancode);
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

int kbd_raw_read_queue(uint8_t *buf, int size)
{
    int read_bytes = 0;
    g_kbd_raw_read_ms = timer_ms;
    while (read_bytes < size) {
        if (kbd_raw_queue_head == kbd_raw_queue_tail) {
            break;
        }
        buf[read_bytes++] = kbd_raw_queue[kbd_raw_queue_tail];
        kbd_raw_queue_tail = (kbd_raw_queue_tail + 1) % KBD_RAW_QUEUE_SIZE;
    }
    return read_bytes;
}

uint64_t timer_get_ticks(void)
{
    return timer_ticks;
}

uint64_t timer_get_ms(void)
{
    return timer_ms;
}

static void pic_remap(int offset1, int offset2)
{
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    outb(PIC1_DATA, offset1);
    io_wait();
    outb(PIC2_DATA, offset2);
    io_wait();

    outb(PIC1_DATA, 4);
    io_wait();
    outb(PIC2_DATA, 2);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* Unmask Timer (IRQ0), Keyboard (IRQ1), and Cascade (IRQ2). Mask others for now. */
    outb(PIC1_DATA, 0xF8); /* 1111 1000 -> unmask IRQ0, IRQ1, and IRQ2 */
    outb(PIC2_DATA, 0xFF);
}

void irq_init(void)
{
    serial_write_string("IRQ: Remapping PIC...\r\n");
    pic_remap(0x20, 0x28);

    /* Initialize PIT (frequency 1000 Hz, see TIMER_HW_HZ) */
    serial_write_string("IRQ: Initializing PIT (1000 Hz)...\r\n");
    uint32_t divisor = 1193182 / TIMER_HW_HZ;
    outb(PIT_COMMAND, 0x36);
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    io_wait();

    /* Enable interrupts on CPU */
    __asm__ volatile("sti");
    serial_write_string("IRQ: Interrupts enabled on CPU.\r\n");
}

/* PS/2 bytes as they come, until the lock holder hands them on (the
   console's and the display server's keyboard queues, the mouse driver
   -- which moves the cursor through the GPU queue). The interrupt
   handler only appends; whoever holds the kernel lock drains. */
#define PS2_RING 512
static volatile uint8_t ps2_byte[PS2_RING];
static volatile uint8_t ps2_mouse[PS2_RING];   /* 1: from the mouse (IRQ 12) */
static volatile uint32_t ps2_head, ps2_tail;

static void ps2_push(uint8_t b, uint8_t mouse)
{
    uint32_t next = (ps2_head + 1) % PS2_RING;
    if (next == ps2_tail) return;           /* full: drop */
    ps2_byte[ps2_head] = b;
    ps2_mouse[ps2_head] = mouse;
    __atomic_store_n(&ps2_head, next, __ATOMIC_RELEASE);
}

static void ps2_drain(void)
{
    extern void mouse_handle_interrupt(uint8_t data);
    while (ps2_tail != __atomic_load_n(&ps2_head, __ATOMIC_ACQUIRE)) {
        uint8_t b = ps2_byte[ps2_tail];
        uint8_t m = ps2_mouse[ps2_tail];
        ps2_tail = (ps2_tail + 1) % PS2_RING;
        if (m) {
            mouse_handle_interrupt(b);
        } else {
            if (!g_kbd_raw_read_ms || timer_ms - g_kbd_raw_read_ms > KBD_OWNER_MS)
                keyboard_push_scancode(b);
            kbd_raw_queue_push(b);
        }
    }
}

/* The timer's device work, under the kernel lock: virtio-gpu
   housekeeping (pending cursor update, TSC calibration, re-sending frame
   areas a stalled host missed), the network every millisecond (on the
   10 ms tick alone, every exchange -- a TLS handshake step, an ACK, a DNS
   answer -- waited up to 10 ms for its packet; an empty RX ring is one
   index compare; virtio-net has no RX interrupt of its own), the sound
   card every 10 ms, and the PS/2 bytes. */
static uint64_t g_hda_last_tick;
uint64_t g_dump_every_ms;          /* boot.cfg "dumpevery=N" (kernel/main.c) */
static void timer_device_work(void)
{
    if (g_dump_every_ms) {
        static uint64_t next_dump;
        if (!next_dump) next_dump = timer_ms + g_dump_every_ms;
        if (timer_ms >= next_dump) {
            next_dump = timer_ms + g_dump_every_ms;
            sched_dump_user_threads();
        }
    }
    ps2_drain();
    extern void virtio_gpu_tick(void);
    virtio_gpu_tick();
    extern void net_poll(void);
    net_poll();
    if (timer_ticks != g_hda_last_tick) {
        g_hda_last_tick = timer_ticks;
        extern void hda_tick(void);
        hda_tick();                  /* silence what the sound card just played */
    }
}

/* What a handler left because another core held the lock
   (smp_run_deferred(), run by the holder). */
void irq_deferred_work(void)
{
    timer_device_work();
}

/* An interrupt from user mode on a thread whose process is dead, or that
   has SIGKILL pending: the thread dies right here (the interrupted
   context is simply dropped). Signals are otherwise delivered only on the
   way out of a syscall, so a thread busy in user code would outlive a
   kill -9 or its process's exit_group(). */
static void kill_check(InterruptRegisters *regs)
{
    if ((regs->cs & 3) != 3) return;
    Thread *t = sched_current();
    if (t && t->proc && t->proc->pid != 0 &&
        (t->proc->exited || (t->sig_pending & (1ULL << 9)))) {
        extern void process_mark_exited(struct Process *p, int wait_status);
        extern void thread_exit(void);
        process_mark_exited(t->proc, 9); /* no-op if already dead */
        thread_exit();                    /* never returns */
    }
}

/* time accounting: whose tick, user or kernel (no lock: statistics) */
static void tick_account(InterruptRegisters *regs)
{
    Thread *at = sched_current();
    if (at && at->proc && at->proc->pid != 0) {
        /* atomic: threads of one process tick on several cores at once */
        if ((regs->cs & 3) == 3) __atomic_add_fetch(&at->proc->st_user_ticks, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&at->proc->st_kernel_ticks, 1, __ATOMIC_RELAXED);
    }
}

/* The kernel lock in an interrupt handler (include/wynland/smp.h): taken
   only when free. `took` from kernel mode means the core was idle (in its
   hlt, without the lock): do the work, give it back, switch nothing --
   the idle loop schedules right after. */
static void irq_leave(int took, InterruptRegisters *regs)
{
    if (took == 1 && (regs->cs & 3) != 3) bkl_irq_leave();
}

/* Preemption needs no big lock (the scheduler has its own): from user
   code always; in the kernel only a thread inside the big lock that this
   core holds (took 0) -- kernel code outside it runs with interrupts off,
   or is this core waiting for the lock. */
static inline bool may_preempt(int took, InterruptRegisters *regs)
{
    return (regs->cs & 3) == 3 || took == 0;
}

static void lapic_irq(InterruptRegisters *regs)
{
    if (regs->int_no == VEC_IPI_TLB) {
        smp_tlb_poll();
        lapic_eoi();
        return;
    }
    lapic_eoi();
    if (regs->int_no != VEC_LAPIC_TIMER) return;      /* a wake-up: the hlt is over */

    /* an AP's 1 kHz tick: preemption */
    tick_account(regs);
    int took = bkl_irq_enter();
    if (took >= 0) kill_check(regs);
    if (may_preempt(took, regs)) sched_preempt_tick();
    irq_leave(took, regs);
}

void irq_handler(InterruptRegisters *regs)
{
    if ((regs->cs & 3) == 3) {   /* the frame must be on the running thread's kernel stack */
        Thread *ct = sched_current();
        uint64_t a = (uint64_t)(uintptr_t)regs;
        if (ct && ct->stack_orig && (a < (uint64_t)(uintptr_t)ct->stack_orig ||
                                     a >= (uint64_t)(uintptr_t)ct->stack_orig + THREAD_STACK_SIZE)) {
            extern void uint_to_str(uint64_t v, char *b);
            char b[24];
            serial_write_string("SMP: interrupt frame off its thread's kernel stack: cpu ");
            uint_to_str(this_cpu()->id, b); serial_write_string(b);
            serial_write_string(" tid "); uint_to_str(ct->id, b); serial_write_string(b);
            serial_write_string("\r\n");
        }
    }
    if (regs->int_no >= 0x40) {
        lapic_irq(regs);
        return;
    }
    uint64_t irq = regs->int_no - 32;

    if (irq == 0) {
        /* Timer interrupt (1 kHz) on the boot core */
        timer_ms++;
        bool ten = (timer_ms % TIMER_MS_PER_TICK) == 0;
        if (ten) timer_ticks++;
        tick_account(regs);
        /* Send End of Interrupt (EOI) to PIC before yielding */
        outb(PIC1_COMMAND, PIC_EOI);

        int took = bkl_irq_enter();
        if (took < 0) {
            /* the device work to the lock holder; the thread here may
               still be preempted */
            __atomic_store_n(&g_deferred_due, 1, __ATOMIC_RELEASE);
            if ((regs->cs & 3) == 3) sched_preempt_tick();
            return;
        }
        kill_check(regs);
        __atomic_store_n(&g_deferred_due, 0, __ATOMIC_RELAXED);
        timer_device_work();

        if (ten) {   /* Latency report: timer ticks more than 100 ms apart mean the
               kernel ran that long with interrupts off (or held the kernel
               lock). The first tick after it lands on the culprit: its
               syscall and process. */
            static uint64_t last_tsc;
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t now = ((uint64_t)hi << 32) | lo;
            extern uint64_t net_tsc_ms(void);
            uint64_t per = net_tsc_ms();
            if (last_tsc && per && (now - last_tsc) / per > 100) {
                extern volatile uint64_t g_last_syscall;
                extern void uint_to_str(uint64_t v, char *b);
                char b[32];
                serial_write_string("LATENCY: ");
                uint_to_str((now - last_tsc) / per, b); serial_write_string(b);
                serial_write_string(" ms, syscall ");
                uint_to_str(g_last_syscall, b); serial_write_string(b);
                Thread *lt = sched_current();
                if (lt && lt->proc) { serial_write_string(" in "); serial_write_string(lt->proc->exe_path); }
                serial_write_string("\r\n");
            }
            last_tsc = now;
        }
        /* 1ms preemption: sched_schedule()'s deadline pass is also
           what wakes millisecond sleepers (sched_sleep_ms()). */
        if (may_preempt(took, regs)) sched_preempt_tick();
        irq_leave(took, regs);
        return;
    } else if (irq == 1 || irq == 12) {
        /* Keyboard / mouse: the byte now (the controller holds one),
           the rest under the lock */
        ps2_push(inb(0x60), irq == 12);
        if (regs->int_no >= 40) outb(PIC2_COMMAND, PIC_EOI);
        outb(PIC1_COMMAND, PIC_EOI);
        int took = bkl_irq_enter();
        if (took < 0) {
            __atomic_store_n(&g_deferred_due, 1, __ATOMIC_RELEASE);
            return;
        }
        ps2_drain();
        irq_leave(took, regs);
        return;
    }

    /* Send End of Interrupt (EOI) to PIC */
    if (regs->int_no >= 40) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}
