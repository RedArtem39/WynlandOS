/*
 * WynlandOS - Interrupt Handlers (IRQs) Implementation
 */

#include <wynland/irq.h>

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

/* Timer global tick count */
static volatile uint64_t timer_ticks = 0;

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

uint64_t timer_get_ticks(void)
{
    return timer_ticks;
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

    /* Unmask Timer (IRQ0) and Keyboard (IRQ1). Mask others for now. */
    outb(PIC1_DATA, 0xFC); /* 1111 1100 -> unmask IRQ0 and IRQ1 */
    outb(PIC2_DATA, 0xFF);
}

void irq_init(void)
{
    serial_write_string("IRQ: Remapping PIC...\r\n");
    pic_remap(0x20, 0x28);

    /* Initialize PIT (frequency 100 Hz) */
    serial_write_string("IRQ: Initializing PIT (100 Hz)...\r\n");
    uint32_t divisor = 1193182 / 100;
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
        /* Timer interrupt */
        timer_ticks++;
    } else if (irq == 1) {
        /* Keyboard interrupt */
        uint8_t scancode = inb(0x60);
        keyboard_push_scancode(scancode);
    }

    /* Send End of Interrupt (EOI) to PIC */
    if (regs->int_no >= 40) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}
