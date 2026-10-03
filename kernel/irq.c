/*
 * WynlandOS - Interrupt Handlers (IRQs) Implementation
 */

#include <wynland/irq.h>
#include <wynland/process.h>
#include <wynland/sched.h>


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

/* Separate, always-on raw scancode queue for Ring-3 (/dev/input/kbd),
   independent of kbd_buffer above -- mirrors drivers/input/mouse.c's
   mouse_queue_push/mouse_read_queue exactly. kbd_buffer above is still
   destructively drained by /dev/tty, SYS_read stdin, and the old Ring-0
   wm_handle_key polling loop; this is a SEPARATE feed so a Ring-3 reader
   doesn't steal scancodes from (or get stolen from by) those. */
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

int kbd_raw_read_queue(uint8_t *buf, int size)
{
    int read_bytes = 0;
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

void irq_handler(InterruptRegisters *regs)
{
    uint64_t irq = regs->int_no - 32;

    if (irq == 0) {
        /* Timer interrupt (1 kHz) */
        timer_ms++;
        {   /* time accounting: whose tick, user or kernel */
            Thread *at = sched_current();
            if (at && at->proc && at->proc->pid != 0) {
                if ((regs->cs & 3) == 3) at->proc->st_user_ticks++;
                else at->proc->st_kernel_ticks++;
            }
        }
        /* Send End of Interrupt (EOI) to PIC before yielding */
        outb(PIC1_COMMAND, PIC_EOI);

        /* Signals are otherwise delivered only on the way out of a
           syscall, so a thread busy in user code would outlive a kill -9
           or its process's exit_group(). Interrupted user code of a
           thread whose process is dead, or that has SIGKILL pending,
           dies right here (the interrupted context is simply dropped). */
        if ((regs->cs & 3) == 3) {
            Thread *t = sched_current();
            if (t && t->proc && t->proc->pid != 0 &&
                (t->proc->exited || (t->sig_pending & (1ULL << 9)))) {
                extern void process_mark_exited(struct Process *p, int wait_status);
                extern void thread_exit(void);
                process_mark_exited(t->proc, 9); /* no-op if already dead */
                thread_exit();                    /* never returns */
            }
        }

        /* virtio-gpu housekeeping: pending cursor update, TSC
           calibration, re-sending frame areas a stalled host missed. */
        extern void virtio_gpu_tick(void);
        virtio_gpu_tick();

        /* the network every millisecond: on the 10 ms tick alone, every
           exchange (a TLS handshake step, an ACK, a DNS answer) waited up
           to 10 ms for its packet; an empty RX ring is one index compare */
        extern void net_poll(void);
        net_poll();

        if ((timer_ms % TIMER_MS_PER_TICK) != 0) {
            /* 1ms preemption: sched_schedule()'s deadline pass is also
               what wakes millisecond sleepers (sched_sleep_ms()). */
            sched_preempt_tick();
            return;
        }
        timer_ticks++;
        /* Phase 22d: virtio-net has no RX interrupt of its own (confirmed:
           no IRQ/MSI-X registration anywhere in drivers/net/) -- net_poll()
           draining the RX virtqueue used to happen only as a side effect of
           tcp_recv/tcp_send/udp_recv's own busy-spin loops. Now that those
           block instead of spinning, something has to keep driving RX
           regardless of whether any thread is actively waiting on a
           socket, or a blocked reader would never wake up. Runs BEFORE
           sched_preempt_tick() so it fires on literally every tick
           unconditionally, not only on ticks where this particular thread
           happens to get rescheduled back after a preemption. */
        extern void hda_tick(void);
        hda_tick();                  /* silence what the sound card just played */
        {   /* Latency report: timer ticks more than 100 ms apart mean the
               kernel ran that long with interrupts off. The first tick
               after it lands on the culprit: its syscall and process. */
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
        sched_preempt_tick();
        return;
    } else if (irq == 1) {
        /* Keyboard interrupt */
        uint8_t scancode = inb(0x60);
        keyboard_push_scancode(scancode);
        kbd_raw_queue_push(scancode);
    } else if (irq == 12) {
        /* Mouse interrupt */
        uint8_t data = inb(0x60);
        extern void mouse_handle_interrupt(uint8_t data);
        mouse_handle_interrupt(data);
    }

    /* Send End of Interrupt (EOI) to PIC */
    if (regs->int_no >= 40) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}
