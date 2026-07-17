#pragma once
// Stub for udis86.h
typedef struct ud ud_t;
static inline void ud_init(ud_t*) {}
static inline void ud_set_mode(ud_t*, int) {}
static inline void ud_set_pc(ud_t*, int) {}
static inline void ud_set_input_buffer(ud_t*, const uint8_t*, size_t) {}
static inline int ud_disassemble(ud_t*) { return 0; }
static inline int ud_insn_len(const ud_t*) { return 0; }
