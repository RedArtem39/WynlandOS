#include <wynland/types.h>

/* Inline assembly helpers for port I/O */
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

int g_sys_volume = 80;

/* Play sound with frequency */
void play_sound(uint32_t nFrequence)
{
    if (nFrequence == 0 || g_sys_volume == 0) {
        /* Stop sound / Mute */
        uint8_t tmp = inb(0x61) & 0xFC;
        outb(0x61, tmp);
        return;
    }

    /* Set the PIT Channel 2 divisor to the desired frequency */
    uint32_t Div = 1193180 / nFrequence;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(Div));
    outb(0x42, (uint8_t)(Div >> 8));

    /* Enable speaker (connect PIT channel 2 output to speaker) */
    uint8_t tmp = inb(0x61);
    if ((tmp & 3) != 3) {
        outb(0x61, tmp | 3);
    }
}

/* Stop sound */
void nosound(void)
{
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

/* Make a beep for a duration in milliseconds */
void beep(uint32_t freq, uint32_t duration_ms)
{
    if (g_sys_volume == 0) {
        return;
    }
    
    play_sound(freq);
    
    /* Active wait using timer ticks */
    extern uint64_t timer_get_ticks(void);
    uint64_t start = timer_get_ticks();
    /* 1 tick = 10ms (100 Hz timer) */
    uint64_t ticks_to_wait = duration_ms / 10;
    if (ticks_to_wait == 0) ticks_to_wait = 1;
    while (timer_get_ticks() - start < ticks_to_wait) {
        __asm__ volatile("hlt");
    }
    
    nosound();
}

/* Play a beautiful C Major arpeggio startup chime */
void play_startup_chime(void)
{
    if (g_sys_volume == 0) return;
    beep(261, 80);  /* C4 */
    beep(329, 80);  /* E4 */
    beep(392, 80);  /* G4 */
    beep(523, 250); /* C5 */
}
