/*
 * WynlandOS - the boot log that outlives a crash.
 *
 * The UEFI loader (boot/boot.c) reserves BOOTLOG_BYTES of RAM at one of
 * BOOTLOG_ADDRS and hands it over (BootInfo.log_addr); the kernel
 * (kernel/bootlog.c) copies everything it prints to the serial line into
 * it, a ring. A kernel panic marks it and resets the machine warm (the CPU
 * only: RAM keeps its contents); the loader of the next boot finds the
 * log and writes it to \logs on the boot stick with the firmware's own
 * file system driver -- nothing of ours that may be what crashed. A real
 * machine without a serial port then still has its exact log.
 */
#pragma once

#include <wynland/types.h>

#define BOOTLOG_MAGIC   0x474F4C544F4F4257ULL     /* "WBOOTLOG" */
#define BOOTLOG_BYTES   (2u * 1024u * 1024u)      /* header + ring: a whole boot, up to a desktop */

/* where it may live: the first one free in the firmware's memory map */
#define BOOTLOG_NADDRS  6
static const uint64_t BOOTLOG_ADDRS[BOOTLOG_NADDRS] = {
    0x0F000000ULL, 0x1F000000ULL, 0x2F000000ULL, 0x3F000000ULL, 0x4F000000ULL, 0x5F000000ULL };

#define BOOTLOG_RUNNING 0     /* still up -- or reset without a word (a hang, a triple fault) */
#define BOOTLOG_PANIC   1     /* a kernel panic, then a warm reset */
#define BOOTLOG_CLEAN   2     /* powered off or rebooted on purpose */

typedef struct PACKED {
    uint64_t magic;
    uint64_t self;            /* its own physical address */
    uint32_t size;            /* bytes of the ring after the header */
    uint32_t state;           /* BOOTLOG_* */
    uint64_t head;            /* bytes ever written: the ring's next is head % size */
    uint32_t boot_no;         /* counts the boots that had a log */
    uint32_t check;           /* magic, self and size folded: a stale header fails */
    uint8_t  data[];
} BootLog;

static inline uint32_t bootlog_check(const BootLog *l)
{
    return (uint32_t)(l->magic ^ (l->magic >> 32) ^ l->self ^ (l->self >> 32) ^ l->size) ^ 0x5A17C0DEu;
}

/* the log at `addr`, left by a previous boot, is whole and believable */
static inline bool bootlog_valid(const BootLog *l, uint64_t addr)
{
    return l->magic == BOOTLOG_MAGIC && l->self == addr &&
           l->size == BOOTLOG_BYTES - sizeof(BootLog) && l->check == bootlog_check(l) &&
           l->state <= BOOTLOG_CLEAN;
}
