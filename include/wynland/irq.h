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

/* Timer interface */
uint64_t timer_get_ticks(void);
