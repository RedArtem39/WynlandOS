#include <wynland/types.h>

/* C++ runtime function symbols needed for freestanding execution */

extern "C" void* kmalloc(size_t size);
extern "C" void kfree(void* ptr);

/* Global new / delete operators mapped to kernel heap allocation */

void* operator new(unsigned long size)
{
    return kmalloc(size);
}

void* operator new[](unsigned long size)
{
    return kmalloc(size);
}

void operator delete(void* ptr) noexcept
{
    kfree(ptr);
}

void operator delete[](void* ptr) noexcept
{
    kfree(ptr);
}

void operator delete(void* ptr, unsigned long) noexcept
{
    kfree(ptr);
}

void operator delete[](void* ptr, unsigned long) noexcept
{
    kfree(ptr);
}

/* Placement new operators */
void* operator new(unsigned long, void* p) noexcept
{
    return p;
}

void* operator new[](unsigned long, void* p) noexcept
{
    return p;
}

/* Exception personality stub (since we compile with -fno-exceptions) */
extern "C" void __cxa_pure_virtual()
{
    extern void serial_write_string(const char *str);
    serial_write_string("KERNEL PANIC: Pure virtual function call!\r\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
