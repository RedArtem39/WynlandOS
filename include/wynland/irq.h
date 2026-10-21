/*
 * WynlandOS - Interrupt Handlers (IRQs) Header
 */
#pragma once

#include <wynland/types.h>
#include <wynland/idt.h>

void irq_init(void);
void irq_handler(InterruptRegisters *regs);

/* Keyboard buffer interface */
bool keyboard_has_scancode(void);
uint8_t keyboard_pop_scancode(void);
/* a USB HID keyboard: scancodes go to the same queues as the PS/2 keyboard */
void keyboard_inject_scancode(uint8_t scancode);

/* Timer interface */
uint64_t timer_get_ticks(void);
