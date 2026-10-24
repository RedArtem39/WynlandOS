/*
 * WynlandOS - the example kernel module (kernel/module.c): loaded with
 * insmod, it says hello on the kernel log with the time since boot;
 * rmmod says goodbye. `make modules` builds it into build/devhost/hello.ko
 * (test-wyn 19 loads it on the laptop).
 *
 * A module calls the kernel's functions by name -- declared here, found in
 * the kernel's symbol table at load time.
 */
#include <wynland/types.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern uint64_t timer_get_ms(void);

static uint64_t g_loaded_at;

int init_module(void)
{
    char b[20];
    g_loaded_at = timer_get_ms();
    uint_to_hex(g_loaded_at, b);
    serial_write_string("hello: a module in the running kernel, ms since boot ");
    serial_write_string(b);
    serial_write_string("\r\n");
    return 0;
}

void cleanup_module(void)
{
    char b[20];
    uint_to_hex(timer_get_ms() - g_loaded_at, b);
    serial_write_string("hello: unloaded after ms ");
    serial_write_string(b);
    serial_write_string("\r\n");
}
