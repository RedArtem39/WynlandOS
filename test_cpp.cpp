typedef long unsigned int size_t;
typedef unsigned long long uint64_t;

#define SYS_yield   0
#define SYS_exit    2
#define SYS_read    12
#define SYS_write   13

/* Forward declarations */
extern "C" void _start();
void print_string(const char *msg);
long sys_write(int fd, const void *buf, int size);
long syscall_raw(long num, long a1, long a2, long a3);
void sys_exit(int code);
void print_hex(uint64_t val);

/* C++ memory operators declarations */
void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t) noexcept;
void operator delete[](void* ptr, size_t) noexcept;

class Animal {
public:
    Animal();
    virtual ~Animal();
    virtual void speak() = 0;
};

class Dog : public Animal {
private:
    const char *name;
public:
    Dog(const char *dog_name);
    virtual ~Dog() override;
    virtual void speak() override;
};

/* Program entry point MUST be at the very top for flat binary entry */
extern "C" void _start() {
    print_string("========================================\n");
    print_string("  WynlandOS userspace C++ test starting \n");
    print_string("========================================\n");

    print_string("Instantiating dynamic C++ object on heap...\n");
    Animal *pet = new Dog("Rex");

    print_string("Invoking virtual method speak()...\n");
    pet->speak();

    print_string("Deleting object...\n");
    delete pet;

    print_string("C++ validation complete! Exiting...\n");
    sys_exit(0);
}

/* Helper implementations */
long syscall_raw(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "syscall\n"
        : "=a"(ret)
        : "g"(num), "g"(a1), "g"(a2), "g"(a3)
        : "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

void sys_exit(int code) {
    syscall_raw(SYS_exit, code, 0, 0);
    while (1) {
        __asm__ volatile("hlt");
    }
}

long sys_write(int fd, const void *buf, int size) {
    return syscall_raw(SYS_write, fd, (long)buf, size);
}

void print_string(const char *msg) {
    int len = 0;
    while (msg[len]) len++;
    sys_write(1, msg, len);
}

/* C++ memory operators implementation */
void* operator new(size_t size) {
    return (void*)syscall_raw(15, 0, size, 0);
}

void* operator new[](size_t size) {
    return (void*)syscall_raw(15, 0, size, 0);
}

void operator delete(void* ptr) noexcept {
    syscall_raw(4, (long)ptr, 0, 0);
}

void operator delete[](void* ptr) noexcept {
    syscall_raw(4, (long)ptr, 0, 0);
}

void operator delete(void* ptr, size_t) noexcept {
    syscall_raw(4, (long)ptr, 0, 0);
}

void operator delete[](void* ptr, size_t) noexcept {
    syscall_raw(4, (long)ptr, 0, 0);
}

/* Pure virtual function handler */
extern "C" void __cxa_pure_virtual() {
    print_string("CRITICAL: Pure virtual function call!\n");
    sys_exit(1);
}

void print_hex(uint64_t val) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        int nibble = (val >> (60 - i * 4)) & 0xF;
        buf[2 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buf[18] = '\0';
    print_string(buf);
}

/* C++ Test Classes implementation */
Animal::Animal() {
    print_string("[Animal] Constructor called\n");
}

Animal::~Animal() {
    print_string("[Animal] Destructor called\n");
}

Dog::Dog(const char *dog_name) : name(dog_name) {
    print_string("[Dog] Constructor called for ");
    print_string(name);
    print_string("\n");
}

Dog::~Dog() {
    print_string("[Dog] Destructor called for ");
    print_string(name);
    print_string("\n");
}

void Dog::speak() {
    print_string("[Dog] ");
    print_string(name);
    print_string(" says: Woof! Woof!\n");
}
