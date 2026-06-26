/*
 * WynlandOS - Custom Low-Level Virtual Machine (WynVM) & Assembler (WynASM)
 * ============================================================================
 */

#include <wynland/types.h>
#include <wynland/vfs.h>
#include <wynland/heap.h>
#include <wynland/sched.h>
#include <wynland/font.h>
#include "../gui/window.h"
#include "../gui/theme.h"

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void uint_to_hex(uint64_t val, char *buf);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);
extern uint32_t mouse_get_x(void);
extern uint32_t mouse_get_y(void);
extern uint8_t mouse_get_buttons(void);
extern void wm_register_window(Window *win);
extern void wm_raise_window(Window *win);
extern void comp_mark_dirty(void);
extern uint32_t comp_get_width(void);
extern uint32_t *comp_get_backbuffer(void);

#define VM_MAGIC 0x57594E56 /* "WYNV" */

/* VM Bytecode instruction format (fixed-size 8 bytes) */
typedef struct {
    uint8_t  opcode;
    uint8_t  dest;
    uint8_t  src;
    uint8_t  unused;
    uint32_t imm;
} __attribute__((packed)) VMInstruction;

typedef struct {
    uint32_t magic;
    uint32_t code_size; /* Bytes of instructions */
    uint32_t data_size; /* Bytes of raw string constants */
} __attribute__((packed)) VMHeader;

/* Opcodes */
#define OP_HALT         0
#define OP_MOV_REG_REG  1
#define OP_MOV_REG_IMM  2
#define OP_ADD_REG_REG  3
#define OP_ADD_REG_IMM  4
#define OP_SUB_REG_REG  5
#define OP_SUB_REG_IMM  6
#define OP_CMP_REG_REG  7
#define OP_CMP_REG_IMM  8
#define OP_JMP          9
#define OP_JE           10
#define OP_JNE          11
#define OP_JL           12
#define OP_JG           13
#define OP_PUSH         14
#define OP_POP          15
#define OP_CALL         16
#define OP_RET          17
#define OP_SYS          18

/* Symbol Table for Assembler */
#define MAX_SYMBOLS 128
typedef struct {
    char name[32];
    uint32_t offset;
    bool is_string;
} Symbol;

static Symbol symbol_table[MAX_SYMBOLS];
static uint32_t symbol_count = 0;

static char data_section[2048];
static uint32_t data_offset = 0;

/* Helper functions for string parsing */
static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_spaces(const char *p) {
    while (*p && is_space(*p)) p++;
    return p;
}

static int str_compare_local(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int __attribute__((unused)) str_n_compare(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int parse_reg(const char *p) {
    p = skip_spaces(p);
    if (p[0] == 'r' && p[1] >= '0' && p[1] <= '7') {
        return p[1] - '0';
    }
    return -1;
}

static uint32_t parse_num(const char *p) {
    p = skip_spaces(p);
    uint32_t val = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (*p) {
            char c = *p;
            if (c >= '0' && c <= '9') val = val * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
            else break;
            p++;
        }
    } else {
        while (*p && *p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            p++;
        }
    }
    return val;
}

static void add_symbol(const char *name, uint32_t offset, bool is_string) {
    if (symbol_count >= MAX_SYMBOLS) return;
    Symbol *s = &symbol_table[symbol_count++];
    uint32_t i = 0;
    while (name[i] && name[i] != ':' && i < 31) {
        s->name[i] = name[i];
        i++;
    }
    s->name[i] = '\0';
    s->offset = offset;
    s->is_string = is_string;
}

static int find_symbol(const char *name) {
    char clean_name[32];
    uint32_t i = 0;
    while (name[i] && !is_space(name[i]) && name[i] != ',' && name[i] != ';' && i < 31) {
        clean_name[i] = name[i];
        i++;
    }
    clean_name[i] = '\0';

    for (uint32_t s = 0; s < symbol_count; s++) {
        if (str_compare_local(symbol_table[s].name, clean_name) == 0) {
            return s;
        }
    }
    return -1;
}

/* ============================================================
 * TWO-PASS ASSEMBLER (WynASM)
 * ============================================================ */

bool wynvm_assemble(const char *src_path, const char *dest_path)
{
    VfsFile *sf = vfs_open(src_path);
    if (!sf) {
        serial_write_string("WynASM: ERROR - Could not open source file!\r\n");
        return false;
    }

    symbol_count = 0;
    data_offset = 0;

    /* Read entire source into memory buffer */
    static char src_buf[16384];
    int bytes = vfs_read(sf, src_buf, sizeof(src_buf) - 1);
    vfs_close(sf);
    if (bytes <= 0) return false;
    src_buf[bytes] = '\0';

    /* ---- PASS 1: Find Labels and Strings ---- */
    const char *line = src_buf;
    uint32_t code_offset = 0;

    while (*line) {
        line = skip_spaces(line);
        if (*line == '\0') break;

        /* Skip comments */
        if (*line == ';') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        /* Detect definitions: labels or string data */
        const char *colon = line;
        while (*colon && *colon != '\n' && *colon != ':') colon++;

        if (*colon == ':') {
            /* We found a colon, label definition! */
            const char *def_val = skip_spaces(colon + 1);
            if (*def_val == '"') {
                /* String Constant Definition: label: "text" */
                def_val++; /* skip first quote */
                uint32_t start_data = data_offset;
                while (*def_val && *def_val != '"' && *def_val != '\n') {
                    data_section[data_offset++] = *def_val++;
                }
                data_section[data_offset++] = '\0'; /* null-terminator */
                
                add_symbol(line, start_data, true);
            } else {
                /* Code Label Definition: label: */
                add_symbol(line, code_offset, false);
            }
            line = colon + 1;
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        /* It's a normal instruction, increment PC offset */
        code_offset += sizeof(VMInstruction);
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    /* ---- PASS 2: Compile instructions into binary ---- */
    static VMInstruction instructions[512];
    uint32_t inst_count = 0;

    line = src_buf;
    code_offset = 0;

    while (*line) {
        line = skip_spaces(line);
        if (*line == '\0') break;

        if (*line == ';') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        const char *colon = line;
        while (*colon && *colon != '\n' && *colon != ':') colon++;
        if (*colon == ':') {
            line = colon + 1;
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        /* Parse Instruction */
        char op[16];
        uint32_t opi = 0;
        while (*line && !is_space(*line) && *line != ';' && opi < 15) {
            op[opi++] = *line++;
        }
        op[opi] = '\0';

        VMInstruction *inst = &instructions[inst_count++];
        memset(inst, 0, sizeof(VMInstruction));

        if (str_compare_local(op, "halt") == 0) {
            inst->opcode = OP_HALT;
        } else if (str_compare_local(op, "ret") == 0) {
            inst->opcode = OP_RET;
        } else if (str_compare_local(op, "mov") == 0) {
            /* mov dest, src */
            line = skip_spaces(line);
            int dest = parse_reg(line);
            while (*line && *line != ',') line++;
            if (*line == ',') line++;
            line = skip_spaces(line);
            int src = parse_reg(line);
            if (src != -1) {
                inst->opcode = OP_MOV_REG_REG;
                inst->dest = dest;
                inst->src = src;
            } else {
                inst->opcode = OP_MOV_REG_IMM;
                inst->dest = dest;
                /* Immediate or Label symbol */
                if (line[0] == 'r' || (line[0] >= '0' && line[0] <= '9') || line[0] == '-') {
                    inst->imm = parse_num(line);
                } else {
                    int sym = find_symbol(line);
                    if (sym != -1) inst->imm = symbol_table[sym].offset;
                }
            }
        } else if (str_compare_local(op, "add") == 0) {
            line = skip_spaces(line);
            int dest = parse_reg(line);
            while (*line && *line != ',') line++;
            if (*line == ',') line++;
            line = skip_spaces(line);
            int src = parse_reg(line);
            if (src != -1) {
                inst->opcode = OP_ADD_REG_REG;
                inst->dest = dest;
                inst->src = src;
            } else {
                inst->opcode = OP_ADD_REG_IMM;
                inst->dest = dest;
                inst->imm = parse_num(line);
            }
        } else if (str_compare_local(op, "sub") == 0) {
            line = skip_spaces(line);
            int dest = parse_reg(line);
            while (*line && *line != ',') line++;
            if (*line == ',') line++;
            line = skip_spaces(line);
            int src = parse_reg(line);
            if (src != -1) {
                inst->opcode = OP_SUB_REG_REG;
                inst->dest = dest;
                inst->src = src;
            } else {
                inst->opcode = OP_SUB_REG_IMM;
                inst->dest = dest;
                inst->imm = parse_num(line);
            }
        } else if (str_compare_local(op, "cmp") == 0) {
            line = skip_spaces(line);
            int dest = parse_reg(line);
            while (*line && *line != ',') line++;
            if (*line == ',') line++;
            line = skip_spaces(line);
            int src = parse_reg(line);
            if (src != -1) {
                inst->opcode = OP_CMP_REG_REG;
                inst->dest = dest;
                inst->src = src;
            } else {
                inst->opcode = OP_CMP_REG_IMM;
                inst->dest = dest;
                inst->imm = parse_num(line);
            }
        } else if (str_compare_local(op, "jmp") == 0 || str_compare_local(op, "je") == 0 ||
                   str_compare_local(op, "jne") == 0 || str_compare_local(op, "jl") == 0 ||
                   str_compare_local(op, "jg") == 0 || str_compare_local(op, "call") == 0) {
            if (str_compare_local(op, "jmp") == 0) inst->opcode = OP_JMP;
            else if (str_compare_local(op, "je") == 0) inst->opcode = OP_JE;
            else if (str_compare_local(op, "jne") == 0) inst->opcode = OP_JNE;
            else if (str_compare_local(op, "jl") == 0) inst->opcode = OP_JL;
            else if (str_compare_local(op, "jg") == 0) inst->opcode = OP_JG;
            else if (str_compare_local(op, "call") == 0) inst->opcode = OP_CALL;

            line = skip_spaces(line);
            if (line[0] >= '0' && line[0] <= '9') {
                inst->imm = parse_num(line);
            } else {
                int sym = find_symbol(line);
                if (sym != -1) inst->imm = symbol_table[sym].offset;
            }
        } else if (str_compare_local(op, "push") == 0) {
            inst->opcode = OP_PUSH;
            inst->dest = parse_reg(line);
        } else if (str_compare_local(op, "pop") == 0) {
            inst->opcode = OP_POP;
            inst->dest = parse_reg(line);
        } else if (str_compare_local(op, "sys") == 0) {
            inst->opcode = OP_SYS;
            inst->imm = parse_num(line);
        }

        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    /* Write binary to disk */
    VfsFile *df = vfs_open_flags(dest_path, VFS_O_CREATE | VFS_O_WRITE | VFS_O_TRUNC);
    if (!df) {
        serial_write_string("WynASM: ERROR - Could not create output file!\r\n");
        return false;
    }

    VMHeader hdr;
    hdr.magic = VM_MAGIC;
    hdr.code_size = inst_count * sizeof(VMInstruction);
    hdr.data_size = data_offset;

    vfs_write(df, &hdr, sizeof(VMHeader));
    vfs_write(df, instructions, hdr.code_size);
    if (hdr.data_size > 0) {
        vfs_write(df, data_section, hdr.data_size);
    }
    vfs_close(df);

    serial_write_string("WynASM: Compilation successful.\r\n");
    return true;
}

/* ============================================================
 * INTERPRETER AND VM RUNTIME (WynVM)
 * ============================================================ */

#define MAX_VM_WINDOWS 16
static Window *vm_windows[MAX_VM_WINDOWS];
static uint32_t vm_window_count = 0;

static void draw_vm_window_content(Window *self)
{
    if (!self->backing_store) return;
    uint32_t cx = self->x + 1;
    uint32_t cy = self->y + THEME_TITLEBAR_HEIGHT + 1;
    uint32_t cw = self->w - 2;
    uint32_t ch = self->h - THEME_TITLEBAR_HEIGHT - 2;

    uint32_t bw = comp_get_width();
    uint32_t *back_buffer = comp_get_backbuffer();

    for (uint32_t row = 0; row < ch; row++) {
        memcpy(&back_buffer[(cy + row) * bw + cx], &self->backing_store[row * cw], cw * 4);
    }
}

static Window *find_vm_window(uint32_t win_id)
{
    for (uint32_t i = 0; i < vm_window_count; i++) {
        if (vm_windows[i] && vm_windows[i]->id == win_id) {
            return vm_windows[i];
        }
    }
    return NULL;
}

static void vm_draw_char(Window *win, int32_t x, int32_t y, char c, uint32_t color)
{
    uint32_t cw = win->w - 2;
    uint32_t ch = win->h - THEME_TITLEBAR_HEIGHT - 2;
    const uint8_t *glyph = font_8x16[(uint8_t)c];

    for (uint32_t row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        int32_t py = y + row;
        if (py < 0 || py >= (int32_t)ch) continue;
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                int32_t px = x + col;
                if (px >= 0 && px < (int32_t)cw) {
                    win->backing_store[py * cw + px] = color & 0x00FFFFFF;
                }
            }
        }
    }
}

static void handle_vm_mouse(Window *self, int32_t mx, int32_t my, uint8_t buttons)
{
    (void)self; (void)mx; (void)my; (void)buttons;
    /* Redraw window when mouse clicked */
    comp_mark_dirty();
}

static void handle_vm_key(Window *self, uint8_t scancode, char ascii)
{
    (void)self; (void)scancode; (void)ascii;
}

static void execute_syscall(uint32_t sys_num, uint32_t *regs, char *data_base)
{
    switch (sys_num) {
        case 1: {
            /* sys_print: r1 = string ptr (offset) */
            char *str = data_base + regs[1];
            serial_write_string(str);
            serial_write_string("\r\n");
            break;
        }
        case 2: {
            /* sys_create_window: r1=x, r2=y, r3=w, r4=h, r5=title offset. Returns ID in r0 */
            if (vm_window_count >= MAX_VM_WINDOWS) {
                regs[0] = 0;
                break;
            }
            Window *win = (Window *)kmalloc(sizeof(Window));
            memset(win, 0, sizeof(Window));

            /* Use arbitrary distinct ID sequence */
            static uint32_t g_vm_win_ids = 100;
            win->id = g_vm_win_ids++;
            win->x = regs[1];
            win->y = regs[2];
            win->w = regs[3];
            win->h = regs[4];
            
            char *title = data_base + regs[5];
            uint32_t ti = 0;
            while (title[ti] && ti < MAX_TITLE_LEN - 1) {
                win->title[ti] = title[ti];
                ti++;
            }
            win->title[ti] = '\0';
            win->is_visible = true;
            win->is_focused = true;
            win->draw_content = draw_vm_window_content;
            win->handle_mouse = handle_vm_mouse;
            win->handle_key = handle_vm_key;

            /* Allocate backing store for client area */
            uint32_t cw = win->w - 2;
            uint32_t ch = win->h - THEME_TITLEBAR_HEIGHT - 2;
            win->backing_store = (uint32_t *)kmalloc(cw * ch * 4);
            /* Fill with paper color */
            for (uint32_t i = 0; i < cw * ch; i++) {
                win->backing_store[i] = 0x00ECEFF4;
            }

            vm_windows[vm_window_count++] = win;
            wm_register_window(win);
            wm_raise_window(win);

            regs[0] = win->id;
            break;
        }
        case 3: {
            /* sys_fill_rect: r0=winID, r1=x, r2=y, r3=w, r4=h, r5=color */
            Window *win = find_vm_window(regs[0]);
            if (win && win->backing_store) {
                uint32_t cw = win->w - 2;
                uint32_t ch = win->h - THEME_TITLEBAR_HEIGHT - 2;
                int32_t x = regs[1], y = regs[2];
                uint32_t w = regs[3], h = regs[4], color = regs[5];

                for (uint32_t dy = 0; dy < h; dy++) {
                    int32_t py = y + dy;
                    if (py < 0 || py >= (int32_t)ch) continue;
                    for (uint32_t dx = 0; dx < w; dx++) {
                        int32_t px = x + dx;
                        if (px < 0 || px >= (int32_t)cw) continue;
                        win->backing_store[py * cw + px] = color & 0x00FFFFFF;
                    }
                }
                comp_mark_dirty();
            }
            break;
        }
        case 4: {
            /* sys_draw_string: r0=winID, r1=x, r2=y, r3=color, r4=string offset */
            Window *win = find_vm_window(regs[0]);
            if (win && win->backing_store) {
                int32_t x = regs[1], y = regs[2];
                uint32_t color = regs[3];
                char *str = data_base + regs[4];

                while (*str) {
                    vm_draw_char(win, x, y, *str, color);
                    x += 9;
                    str++;
                }
                comp_mark_dirty();
            }
            break;
        }
        case 5: {
            /* sys_draw_line: r0=winID, r1=x1, r2=y1, r3=x2, r4=y2, r5=color */
            Window *win = find_vm_window(regs[0]);
            if (win && win->backing_store) {
                uint32_t cw = win->w - 2;
                uint32_t ch = win->h - THEME_TITLEBAR_HEIGHT - 2;

                int32_t x0 = regs[1], y0 = regs[2], x1 = regs[3], y1 = regs[4];
                uint32_t color = regs[5];

                int32_t dx = x1 - x0; if (dx < 0) dx = -dx;
                int32_t dy = y1 - y0; if (dy < 0) dy = -dy;
                int32_t sx = x0 < x1 ? 1 : -1;
                int32_t sy = y0 < y1 ? 1 : -1;
                int32_t err = dx - dy;

                while (1) {
                    if (x0 >= 0 && x0 < (int32_t)cw && y0 >= 0 && y0 < (int32_t)ch) {
                        win->backing_store[y0 * cw + x0] = color & 0x00FFFFFF;
                    }
                    if (x0 == x1 && y0 == y1) break;
                    int32_t e2 = 2 * err;
                    if (e2 > -dy) { err -= dy; x0 += sx; }
                    if (e2 < dx) { err += dx; y0 += sy; }
                }
                comp_mark_dirty();
            }
            break;
        }
        case 6: {
            /* sys_draw_circle: r0=winID, r1=cx, r2=cy, r3=radius, r4=color */
            Window *win = find_vm_window(regs[0]);
            if (win && win->backing_store) {
                uint32_t cw = win->w - 2;
                uint32_t ch = win->h - THEME_TITLEBAR_HEIGHT - 2;
                int32_t cx = regs[1], cy = regs[2], r = regs[3];
                uint32_t color = regs[4];

                for (int32_t dy = -r; dy <= r; dy++) {
                    for (int32_t dx = -r; dx <= r; dx++) {
                        if (dx*dx + dy*dy <= r*r) {
                            int32_t px = cx + dx;
                            int32_t py = cy + dy;
                            if (px >= 0 && px < (int32_t)cw && py >= 0 && py < (int32_t)ch) {
                                win->backing_store[py * cw + px] = color & 0x00FFFFFF;
                            }
                        }
                    }
                }
                comp_mark_dirty();
            }
            break;
        }
        case 7: {
            /* sys_get_mouse: r0=winID. Returns: r0=mx, r1=my, r2=buttons */
            Window *win = find_vm_window(regs[0]);
            if (win) {
                int32_t mx = (int32_t)mouse_get_x() - win->x - 1;
                int32_t my = (int32_t)mouse_get_y() - win->y - THEME_TITLEBAR_HEIGHT - 1;
                regs[0] = mx;
                regs[1] = my;
                regs[2] = mouse_get_buttons();
            } else {
                regs[0] = 0; regs[1] = 0; regs[2] = 0;
            }
            break;
        }
        case 8: {
            /* sys_yield */
            sched_yield();
            break;
        }
    }
}

static void vm_run_thread(void *arg)
{
    char *bin_data = (char *)arg;
    VMHeader *hdr = (VMHeader *)bin_data;
    VMInstruction *code = (VMInstruction *)(bin_data + sizeof(VMHeader));
    char *data_base = bin_data + sizeof(VMHeader) + hdr->code_size;

    uint32_t regs[8] = {0};
    uint32_t pc = 0;
    uint32_t max_pc = hdr->code_size / sizeof(VMInstruction);

    /* VM stack */
    uint32_t stack[256];
    uint32_t sp = 0;

    bool flag_eq = false;
    bool flag_lt = false;
    bool flag_gt = false;

    while (pc < max_pc) {
        VMInstruction inst = code[pc++];
        
        switch (inst.opcode) {
            case OP_HALT:
                goto vm_finished;

            case OP_MOV_REG_REG:
                regs[inst.dest] = regs[inst.src];
                break;

            case OP_MOV_REG_IMM:
                regs[inst.dest] = inst.imm;
                break;

            case OP_ADD_REG_REG:
                regs[inst.dest] += regs[inst.src];
                break;

            case OP_ADD_REG_IMM:
                regs[inst.dest] += inst.imm;
                break;

            case OP_SUB_REG_REG:
                regs[inst.dest] -= regs[inst.src];
                break;

            case OP_SUB_REG_IMM:
                regs[inst.dest] -= inst.imm;
                break;

            case OP_CMP_REG_REG: {
                uint32_t d = regs[inst.dest];
                uint32_t s = regs[inst.src];
                flag_eq = (d == s);
                flag_lt = (d < s);
                flag_gt = (d > s);
                break;
            }
            case OP_CMP_REG_IMM: {
                uint32_t d = regs[inst.dest];
                uint32_t imm = inst.imm;
                flag_eq = (d == imm);
                flag_lt = (d < imm);
                flag_gt = (d > imm);
                break;
            }
            case OP_JMP:
                pc = inst.imm / sizeof(VMInstruction);
                break;

            case OP_JE:
                if (flag_eq) pc = inst.imm / sizeof(VMInstruction);
                break;

            case OP_JNE:
                if (!flag_eq) pc = inst.imm / sizeof(VMInstruction);
                break;

            case OP_JL:
                if (flag_lt) pc = inst.imm / sizeof(VMInstruction);
                break;

            case OP_JG:
                if (flag_gt) pc = inst.imm / sizeof(VMInstruction);
                break;

            case OP_PUSH:
                if (sp < 256) stack[sp++] = regs[inst.dest];
                break;

            case OP_POP:
                if (sp > 0) regs[inst.dest] = stack[--sp];
                break;

            case OP_CALL:
                if (sp < 256) stack[sp++] = pc * sizeof(VMInstruction);
                pc = inst.imm / sizeof(VMInstruction);
                break;

            case OP_RET:
                if (sp > 0) {
                    uint32_t ret_offset = stack[--sp];
                    pc = ret_offset / sizeof(VMInstruction);
                }
                break;

            case OP_SYS:
                execute_syscall(inst.imm, regs, data_base);
                break;

            default:
                goto vm_finished;
        }
    }

vm_finished:
    /* Clean up binary memory and thread structure */
    kfree(bin_data);
    thread_exit();
}

bool wynvm_run(const char *bin_path)
{
    VfsFile *bf = vfs_open(bin_path);
    if (!bf) {
        serial_write_string("WynVM: ERROR - Could not open binary file!\r\n");
        return false;
    }

    /* Read header first */
    VMHeader hdr;
    if (vfs_read(bf, &hdr, sizeof(VMHeader)) != sizeof(VMHeader)) {
        vfs_close(bf);
        return false;
    }

    if (hdr.magic != VM_MAGIC) {
        serial_write_string("WynVM: ERROR - Invalid binary magic!\r\n");
        vfs_close(bf);
        return false;
    }

    /* Allocate continuous memory buffer for execution */
    uint32_t total_size = sizeof(VMHeader) + hdr.code_size + hdr.data_size;
    char *bin_data = (char *)kmalloc(total_size);
    if (!bin_data) {
        vfs_close(bf);
        return false;
    }

    /* Copy header into buffer, then read code and data */
    memcpy(bin_data, &hdr, sizeof(VMHeader));
    vfs_read(bf, bin_data + sizeof(VMHeader), hdr.code_size + hdr.data_size);
    vfs_close(bf);

    /* Create scheduler thread and yield */
    thread_create(vm_run_thread, bin_data);
    sched_yield();

    return true;
}
