/*
 * WynlandOS - WynLang Compiler (Python-like → WynASM)
 * ============================================================================
 */

#include <wynland/types.h>
#include <wynland/vfs.h>
#include <wynland/heap.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

/* ============================================================
 * LOCAL STRING HELPERS
 * ============================================================ */

static bool wl_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static bool wl_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool wl_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool wl_is_alnum(char c) {
    return wl_is_alpha(c) || wl_is_digit(c);
}

static int wl_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static int wl_strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static size_t wl_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static void wl_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void wl_strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ============================================================
 * COMPILER STATE
 * ============================================================ */

#define WL_MAX_VARS    32
#define WL_MAX_STRINGS 64
#define WL_MAX_LINES   256
#define WL_OUT_SIZE    16384
#define WL_SRC_SIZE    8192

typedef struct {
    char name[32];
    uint8_t reg_index;
} WLVar;

typedef struct {
    char label[16];
    char content[128];
} WLString;

typedef struct {
    const char *start;
    uint32_t len;
    uint32_t indent;
} WLLine;

typedef struct {
    /* Variable table: r0-r5 user regs, r6-r7 temps */
    WLVar vars[WL_MAX_VARS];
    uint32_t var_count;
    uint32_t next_reg;

    /* String constants */
    WLString strings[WL_MAX_STRINGS];
    uint32_t string_count;

    /* Parsed source lines */
    WLLine lines[WL_MAX_LINES];
    uint32_t line_count;

    /* Output buffer */
    char *out;
    uint32_t out_pos;

    /* Label counters */
    uint32_t label_counter;
} WLState;

/* ============================================================
 * OUTPUT HELPERS
 * ============================================================ */

static void wl_emit(WLState *st, const char *s) {
    while (*s && st->out_pos < WL_OUT_SIZE - 1) {
        st->out[st->out_pos++] = *s++;
    }
}

static void wl_emit_num(WLState *st, uint32_t val) {
    char buf[16];
    uint_to_str(val, buf);
    wl_emit(st, buf);
}

static void wl_emit_hex(WLState *st, uint32_t val) {
    /* Manual hex emit for 0xNNNNNN */
    char buf[12];
    buf[0] = '0';
    buf[1] = 'x';
    int idx = 2;

    if (val == 0) {
        buf[idx++] = '0';
    } else {
        char tmp[8];
        int ti = 0;
        uint32_t v = val;
        while (v > 0) {
            uint32_t d = v & 0xF;
            tmp[ti++] = d < 10 ? '0' + d : 'A' + d - 10;
            v >>= 4;
        }
        /* Reverse */
        for (int i = ti - 1; i >= 0; i--) {
            buf[idx++] = tmp[i];
        }
    }
    buf[idx] = '\0';
    wl_emit(st, buf);
}

static void wl_emit_line(WLState *st, const char *s) {
    wl_emit(st, "    ");
    wl_emit(st, s);
    wl_emit(st, "\n");
}

/* ============================================================
 * VARIABLE & STRING MANAGEMENT
 * ============================================================ */

static int wl_find_var(WLState *st, const char *name) {
    for (uint32_t i = 0; i < st->var_count; i++) {
        if (wl_strcmp(st->vars[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int wl_alloc_var(WLState *st, const char *name) {
    int idx = wl_find_var(st, name);
    if (idx >= 0) return idx;

    if (st->var_count >= WL_MAX_VARS || st->next_reg > 5) {
        serial_write_string("WynLang: ERROR - Too many variables!\r\n");
        return -1;
    }

    WLVar *v = &st->vars[st->var_count];
    wl_strncpy(v->name, name, 31);
    v->reg_index = st->next_reg++;
    return (int)st->var_count++;
}

static const char *wl_var_reg(WLState *st, int var_idx) {
    static char reg_names[6][3] = {"r0","r1","r2","r3","r4","r5"};
    if (var_idx < 0 || var_idx >= (int)st->var_count) return "r6";
    return reg_names[st->vars[var_idx].reg_index];
}

static uint32_t wl_add_string(WLState *st, const char *text, uint32_t len) {
    if (st->string_count >= WL_MAX_STRINGS) return st->string_count;
    WLString *s = &st->strings[st->string_count];
    char buf[4];
    buf[0] = 's'; buf[1] = 't'; buf[2] = 'r'; buf[3] = '_';
    wl_strncpy(s->label, buf, 4);
    /* Append number */
    char num_buf[8];
    uint_to_str(st->string_count, num_buf);
    size_t l = wl_strlen(s->label);
    size_t nl = wl_strlen(num_buf);
    size_t j = 0;
    while (j < nl && l + j < 15) {
        s->label[l + j] = num_buf[j];
        j++;
    }
    s->label[l + j] = '\0';

    /* Copy content */
    uint32_t clen = len < 127 ? len : 127;
    for (uint32_t i = 0; i < clen; i++) {
        s->content[i] = text[i];
    }
    s->content[clen] = '\0';

    return st->string_count++;
}

/* ============================================================
 * SOURCE TOKENIZING
 * ============================================================ */

static void wl_parse_lines(WLState *st, const char *src) {
    const char *p = src;
    st->line_count = 0;

    while (*p && st->line_count < WL_MAX_LINES) {
        /* Skip blank lines and comments */
        if (*p == '\n') { p++; continue; }

        /* Measure indentation (spaces only, 4 per level) */
        uint32_t spaces = 0;
        const char *ls = p;
        while (*p == ' ') { spaces++; p++; }
        uint32_t indent = spaces / 4;

        /* Skip comment lines */
        if (*p == '#') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* Skip empty lines (only spaces) */
        if (*p == '\n' || *p == '\0') {
            if (*p == '\n') p++;
            continue;
        }

        /* Record line start (after indent) and length */
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        uint32_t line_len = (uint32_t)(p - line_start);

        /* Trim trailing \r */
        while (line_len > 0 && line_start[line_len - 1] == '\r') line_len--;

        WLLine *wl = &st->lines[st->line_count++];
        wl->start = line_start;
        wl->len = line_len;
        wl->indent = indent;

        (void)ls;
        if (*p == '\n') p++;
    }
}

/* ============================================================
 * TOKEN EXTRACTION HELPERS
 * ============================================================ */

/* Extract an identifier from line at offset, write to buf, return new offset */
static uint32_t wl_read_ident(const char *line, uint32_t len, uint32_t off, char *buf, uint32_t bufsize) {
    uint32_t i = 0;
    while (off < len && wl_is_alnum(line[off]) && i < bufsize - 1) {
        buf[i++] = line[off++];
    }
    buf[i] = '\0';
    return off;
}

/* Skip whitespace in line from offset, return new offset */
static uint32_t wl_skip_ws(const char *line, uint32_t len, uint32_t off) {
    while (off < len && wl_is_space(line[off])) off++;
    return off;
}

/* Parse a numeric literal (decimal or 0x hex) from line at offset */
static uint32_t wl_parse_number(const char *line, uint32_t len, uint32_t off, uint32_t *val) {
    *val = 0;
    if (off + 1 < len && line[off] == '0' && (line[off + 1] == 'x' || line[off + 1] == 'X')) {
        off += 2;
        while (off < len) {
            char c = line[off];
            if (c >= '0' && c <= '9') *val = *val * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') *val = *val * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *val = *val * 16 + (c - 'A' + 10);
            else break;
            off++;
        }
    } else {
        while (off < len && line[off] >= '0' && line[off] <= '9') {
            *val = *val * 10 + (line[off] - '0');
            off++;
        }
    }
    return off;
}

/* Parse a string literal from line at offset (skips opening quote, returns after closing quote) */
static uint32_t wl_parse_string_lit(const char *line, uint32_t len, uint32_t off, char *buf, uint32_t bufsize) {
    uint32_t i = 0;
    if (off < len && line[off] == '"') off++;
    while (off < len && line[off] != '"' && i < bufsize - 1) {
        buf[i++] = line[off++];
    }
    buf[i] = '\0';
    if (off < len && line[off] == '"') off++;
    return off;
}

/* ============================================================
 * EXPRESSION COMPILATION
 * ============================================================ */

/* Emit code for: dest_reg = <expr>
 * Supports: literal, variable, var OP var, var OP literal */
static void wl_compile_expr(WLState *st, const char *dest_reg,
                            const char *line, uint32_t len, uint32_t off) {
    off = wl_skip_ws(line, len, off);

    /* Check if first token is a number (decimal or hex) */
    if (wl_is_digit(line[off])) {
        uint32_t val;
        wl_parse_number(line, len, off, &val);
        wl_emit(st, "    mov ");
        wl_emit(st, dest_reg);
        wl_emit(st, ", ");
        if (val > 255) {
            wl_emit_hex(st, val);
        } else {
            wl_emit_num(st, val);
        }
        wl_emit(st, "\n");
        return;
    }

    /* First token is a variable name */
    char first[32];
    uint32_t after = wl_read_ident(line, len, off, first, sizeof(first));

    /* Check for operator after */
    uint32_t opos = wl_skip_ws(line, len, after);
    if (opos >= len || line[opos] == '\n' || line[opos] == '\r' || line[opos] == '#') {
        /* Simple assignment: dest = var */
        int vi = wl_find_var(st, first);
        if (vi >= 0) {
            wl_emit(st, "    mov ");
            wl_emit(st, dest_reg);
            wl_emit(st, ", ");
            wl_emit(st, wl_var_reg(st, vi));
            wl_emit(st, "\n");
        }
        return;
    }

    char op_char = line[opos];
    if (op_char != '+' && op_char != '-') {
        /* Simple assignment from var */
        int vi = wl_find_var(st, first);
        if (vi >= 0) {
            wl_emit(st, "    mov ");
            wl_emit(st, dest_reg);
            wl_emit(st, ", ");
            wl_emit(st, wl_var_reg(st, vi));
            wl_emit(st, "\n");
        }
        return;
    }

    /* Binary expression: first OP second */
    int lhs_vi = wl_find_var(st, first);
    if (lhs_vi < 0) return;

    uint32_t rhs_off = wl_skip_ws(line, len, opos + 1);

    /* Emit: mov dest, lhs_reg */
    wl_emit(st, "    mov ");
    wl_emit(st, dest_reg);
    wl_emit(st, ", ");
    wl_emit(st, wl_var_reg(st, lhs_vi));
    wl_emit(st, "\n");

    const char *asm_op = (op_char == '+') ? "add" : "sub";

    if (wl_is_digit(line[rhs_off])) {
        /* RHS is number */
        uint32_t rhs_val;
        wl_parse_number(line, len, rhs_off, &rhs_val);
        wl_emit(st, "    ");
        wl_emit(st, asm_op);
        wl_emit(st, " ");
        wl_emit(st, dest_reg);
        wl_emit(st, ", ");
        wl_emit_num(st, rhs_val);
        wl_emit(st, "\n");
    } else {
        /* RHS is variable */
        char rhs_name[32];
        wl_read_ident(line, len, rhs_off, rhs_name, sizeof(rhs_name));
        int rhs_vi = wl_find_var(st, rhs_name);
        if (rhs_vi >= 0) {
            wl_emit(st, "    ");
            wl_emit(st, asm_op);
            wl_emit(st, " ");
            wl_emit(st, dest_reg);
            wl_emit(st, ", ");
            wl_emit(st, wl_var_reg(st, rhs_vi));
            wl_emit(st, "\n");
        }
    }
}

/* ============================================================
 * GUI CALL HELPERS
 * ============================================================ */

/* Parse comma-separated arguments for a function call.
 * Returns the number of args parsed. Each arg is either a number,
 * a variable name, or a string literal (stored and replaced by label). */
static uint32_t wl_parse_args(WLState *st, const char *line, uint32_t len, uint32_t off,
                              char args[][32], uint32_t max_args) {
    uint32_t argc = 0;
    off = wl_skip_ws(line, len, off);
    if (off >= len || line[off] != '(') return 0;
    off++; /* skip '(' */

    while (off < len && line[off] != ')' && argc < max_args) {
        off = wl_skip_ws(line, len, off);
        if (off >= len || line[off] == ')') break;

        if (line[off] == '"') {
            /* String literal → add to string table */
            char strbuf[128];
            off = wl_parse_string_lit(line, len, off, strbuf, sizeof(strbuf));
            uint32_t sid = wl_add_string(st, strbuf, wl_strlen(strbuf));
            /* Store label reference */
            wl_strcpy(args[argc], st->strings[sid].label);
        } else if (wl_is_digit(line[off])) {
            /* Number literal */
            uint32_t val;
            uint32_t noff = wl_parse_number(line, len, off, &val);
            /* Store as numeric string for later emit */
            if (val > 255) {
                /* Store hex */
                char hbuf[12];
                hbuf[0] = '0'; hbuf[1] = 'x';
                int hi = 2;
                if (val == 0) { hbuf[hi++] = '0'; }
                else {
                    char tmp[8]; int ti = 0;
                    uint32_t v = val;
                    while (v > 0) { uint32_t d = v & 0xF; tmp[ti++] = d < 10 ? '0' + d : 'A' + d - 10; v >>= 4; }
                    for (int i = ti - 1; i >= 0; i--) hbuf[hi++] = tmp[i];
                }
                hbuf[hi] = '\0';
                wl_strcpy(args[argc], hbuf);
            } else {
                char nbuf[16];
                uint_to_str(val, nbuf);
                wl_strcpy(args[argc], nbuf);
            }
            off = noff;
        } else {
            /* Variable name */
            off = wl_read_ident(line, len, off, args[argc], 32);
        }
        argc++;

        off = wl_skip_ws(line, len, off);
        if (off < len && line[off] == ',') off++;
    }
    return argc;
}

/* Load a parsed arg into register rN (by name). */
static void wl_load_arg(WLState *st, const char *reg, const char *arg) {
    /* Check if arg is a string label (starts with "str_") */
    if (wl_strncmp(arg, "str_", 4) == 0) {
        wl_emit(st, "    mov ");
        wl_emit(st, reg);
        wl_emit(st, ", ");
        wl_emit(st, arg);
        wl_emit(st, "\n");
        return;
    }

    /* Check if arg is a number */
    if (wl_is_digit(arg[0])) {
        wl_emit(st, "    mov ");
        wl_emit(st, reg);
        wl_emit(st, ", ");
        wl_emit(st, arg);
        wl_emit(st, "\n");
        return;
    }

    /* It's a variable */
    int vi = wl_find_var(st, arg);
    if (vi >= 0) {
        wl_emit(st, "    mov ");
        wl_emit(st, reg);
        wl_emit(st, ", ");
        wl_emit(st, wl_var_reg(st, vi));
        wl_emit(st, "\n");
    }
}

/* ============================================================
 * CONDITION HELPERS
 * ============================================================ */

/* Parse condition like "x > 5" or "x == y" and emit cmp + inverse jump */
static void wl_compile_condition(WLState *st, const char *line, uint32_t len,
                                 uint32_t off, const char *end_label) {
    char lhs[32];
    off = wl_skip_ws(line, len, off);
    off = wl_read_ident(line, len, off, lhs, sizeof(lhs));
    off = wl_skip_ws(line, len, off);

    /* Read operator */
    char op[3] = {0, 0, 0};
    if (off < len) op[0] = line[off++];
    if (off < len && (line[off] == '=' || line[off] == '<' || line[off] == '>'))
        op[1] = line[off++];

    off = wl_skip_ws(line, len, off);

    int lhs_vi = wl_find_var(st, lhs);
    if (lhs_vi < 0) return;

    /* Emit cmp */
    wl_emit(st, "    cmp ");
    wl_emit(st, wl_var_reg(st, lhs_vi));
    wl_emit(st, ", ");

    if (wl_is_digit(line[off])) {
        uint32_t val;
        wl_parse_number(line, len, off, &val);
        wl_emit_num(st, val);
    } else {
        char rhs[32];
        wl_read_ident(line, len, off, rhs, sizeof(rhs));
        int rhs_vi = wl_find_var(st, rhs);
        if (rhs_vi >= 0) {
            wl_emit(st, wl_var_reg(st, rhs_vi));
        }
    }
    wl_emit(st, "\n");

    /* Emit inverse conditional jump to end_label */
    const char *jmp_op = "jmp";
    if (op[0] == '>' && op[1] == '\0')       jmp_op = "jle";
    else if (op[0] == '<' && op[1] == '\0')  jmp_op = "jge";
    else if (op[0] == '=' && op[1] == '=')   jmp_op = "jne";
    else if (op[0] == '!' && op[1] == '=')   jmp_op = "je";
    else if (op[0] == '>' && op[1] == '=')   jmp_op = "jl";
    else if (op[0] == '<' && op[1] == '=')   jmp_op = "jg";

    wl_emit(st, "    ");
    wl_emit(st, jmp_op);
    wl_emit(st, " ");
    wl_emit(st, end_label);
    wl_emit(st, "\n");
}

/* ============================================================
 * FUNCTION BODY COMPILATION
 * ============================================================ */

/* Compile a block of lines at a given indent level, starting from index *idx.
 * Stops when indent drops below the expected body_indent. */
static void wl_compile_block(WLState *st, uint32_t *idx, uint32_t body_indent, bool is_main);

/* Compile a single statement line */
static void wl_compile_stmt(WLState *st, uint32_t *idx, uint32_t body_indent, bool is_main) {
    WLLine *ln = &st->lines[*idx];
    const char *line = ln->start;
    uint32_t len = ln->len;

    /* --- print "text" --- */
    if (wl_strncmp(line, "print ", 6) == 0) {
        uint32_t off = 6;
        off = wl_skip_ws(line, len, off);
        if (off < len && line[off] == '"') {
            char strbuf[128];
            wl_parse_string_lit(line, len, off, strbuf, sizeof(strbuf));
            uint32_t sid = wl_add_string(st, strbuf, wl_strlen(strbuf));
            wl_emit(st, "    mov r1, ");
            wl_emit(st, st->strings[sid].label);
            wl_emit(st, "\n");
            wl_emit_line(st, "sys 1");
        }
        (*idx)++;
        return;
    }

    /* --- if condition: --- */
    if (wl_strncmp(line, "if ", 3) == 0) {
        uint32_t lbl = st->label_counter++;
        char endif_label[32];
        wl_strcpy(endif_label, "_endif_");
        char num_buf[8];
        uint_to_str(lbl, num_buf);
        size_t el = wl_strlen(endif_label);
        size_t nl = wl_strlen(num_buf);
        for (size_t i = 0; i < nl; i++) endif_label[el + i] = num_buf[i];
        endif_label[el + nl] = '\0';

        /* Find the colon to get condition end */
        uint32_t cond_end = len;
        for (uint32_t i = 3; i < len; i++) {
            if (line[i] == ':') { cond_end = i; break; }
        }

        wl_compile_condition(st, line, cond_end, 3, endif_label);
        (*idx)++;

        /* Compile body (lines with indent > body_indent) */
        wl_compile_block(st, idx, body_indent + 1, is_main);

        /* Emit endif label */
        wl_emit(st, endif_label);
        wl_emit(st, ":\n");
        return;
    }

    /* --- while condition: --- */
    if (wl_strncmp(line, "while ", 6) == 0) {
        uint32_t lbl = st->label_counter++;
        char while_label[32], endwhile_label[32];
        char num_buf[8];
        uint_to_str(lbl, num_buf);

        wl_strcpy(while_label, "_while_");
        size_t wl2 = wl_strlen(while_label);
        size_t nl = wl_strlen(num_buf);
        for (size_t i = 0; i < nl; i++) while_label[wl2 + i] = num_buf[i];
        while_label[wl2 + nl] = '\0';

        wl_strcpy(endwhile_label, "_endwhile_");
        size_t ewl = wl_strlen(endwhile_label);
        for (size_t i = 0; i < nl; i++) endwhile_label[ewl + i] = num_buf[i];
        endwhile_label[ewl + nl] = '\0';

        /* Find colon */
        uint32_t cond_end = len;
        for (uint32_t i = 6; i < len; i++) {
            if (line[i] == ':') { cond_end = i; break; }
        }

        /* Emit while label */
        wl_emit(st, while_label);
        wl_emit(st, ":\n");

        wl_compile_condition(st, line, cond_end, 6, endwhile_label);
        (*idx)++;

        /* Compile body */
        wl_compile_block(st, idx, body_indent + 1, is_main);

        /* Jump back to while */
        wl_emit(st, "    jmp ");
        wl_emit(st, while_label);
        wl_emit(st, "\n");

        /* Emit endwhile label */
        wl_emit(st, endwhile_label);
        wl_emit(st, ":\n");
        return;
    }

    /* --- GUI calls & function calls --- */
    /* Check for assignment: var = func_call(...) */
    uint32_t eq_pos = 0;
    bool has_eq = false;
    for (uint32_t i = 0; i < len; i++) {
        if (line[i] == '=' && (i + 1 >= len || line[i + 1] != '=') &&
            (i == 0 || (line[i - 1] != '!' && line[i - 1] != '<' && line[i - 1] != '>'))) {
            eq_pos = i;
            has_eq = true;
            break;
        }
    }

    if (has_eq) {
        /* Extract LHS variable name */
        char lhs_name[32];
        uint32_t loff = 0;
        loff = wl_skip_ws(line, len, loff);
        loff = wl_read_ident(line, len, loff, lhs_name, sizeof(lhs_name));

        /* Allocate variable */
        int var_idx = wl_alloc_var(st, lhs_name);
        if (var_idx < 0) { (*idx)++; return; }

        /* RHS: after '=' */
        uint32_t rhs_off = wl_skip_ws(line, len, eq_pos + 1);

        /* Check if RHS is a function call */
        char func_name[32];
        uint32_t fn_end = rhs_off;
        if (wl_is_alpha(line[rhs_off])) {
            fn_end = wl_read_ident(line, len, rhs_off, func_name, sizeof(func_name));
            uint32_t paren_off = wl_skip_ws(line, len, fn_end);
            if (paren_off < len && line[paren_off] == '(') {
                /* It's a function call with return value */
                char args[8][32];
                uint32_t argc = wl_parse_args(st, line, len, paren_off, args, 8);

                if (wl_strcmp(func_name, "create_window") == 0 && argc >= 5) {
                    /* create_window(x, y, w, h, "title") → r1-r5, sys 2 */
                    wl_load_arg(st, "r1", args[0]);
                    wl_load_arg(st, "r2", args[1]);
                    wl_load_arg(st, "r3", args[2]);
                    wl_load_arg(st, "r4", args[3]);
                    wl_load_arg(st, "r5", args[4]);
                    wl_emit_line(st, "sys 2");
                    /* Store r0 (window ID) into the var's register */
                    if (wl_strcmp(wl_var_reg(st, var_idx), "r0") != 0) {
                        wl_emit(st, "    mov ");
                        wl_emit(st, wl_var_reg(st, var_idx));
                        wl_emit(st, ", r0\n");
                    }
                } else {
                    /* Generic function call with assignment */
                    wl_emit(st, "    call _");
                    wl_emit(st, func_name);
                    wl_emit(st, "\n");
                    if (wl_strcmp(wl_var_reg(st, var_idx), "r0") != 0) {
                        wl_emit(st, "    mov ");
                        wl_emit(st, wl_var_reg(st, var_idx));
                        wl_emit(st, ", r0\n");
                    }
                }
                (*idx)++;
                return;
            }
        }

        /* Regular expression assignment */
        wl_compile_expr(st, wl_var_reg(st, var_idx), line, len, rhs_off);
        (*idx)++;
        return;
    }

    /* --- Standalone function/GUI calls (no assignment) --- */
    /* Check for function call pattern: ident(...) */
    if (wl_is_alpha(line[0])) {
        char func_name[32];
        uint32_t fn_end = wl_read_ident(line, len, 0, func_name, sizeof(func_name));
        uint32_t paren_off = wl_skip_ws(line, len, fn_end);

        if (paren_off < len && line[paren_off] == '(') {
            char args[8][32];
            uint32_t argc = wl_parse_args(st, line, len, paren_off, args, 8);

            if (wl_strcmp(func_name, "fill_rect") == 0 && argc >= 6) {
                /* fill_rect(win, x, y, w, h, color) → r0=win, r1-r5, sys 3 */
                wl_load_arg(st, "r0", args[0]);
                wl_load_arg(st, "r1", args[1]);
                wl_load_arg(st, "r2", args[2]);
                wl_load_arg(st, "r3", args[3]);
                wl_load_arg(st, "r4", args[4]);
                wl_load_arg(st, "r5", args[5]);
                wl_emit_line(st, "sys 3");
            } else if (wl_strcmp(func_name, "draw_text") == 0 && argc >= 5) {
                /* draw_text(win, x, y, color, "text") → r0=win, r1-r4, sys 4 */
                wl_load_arg(st, "r0", args[0]);
                wl_load_arg(st, "r1", args[1]);
                wl_load_arg(st, "r2", args[2]);
                wl_load_arg(st, "r3", args[3]);
                wl_load_arg(st, "r4", args[4]);
                wl_emit_line(st, "sys 4");
            } else if (wl_strcmp(func_name, "draw_line") == 0 && argc >= 6) {
                /* draw_line(win, x1, y1, x2, y2, color) → r0=win, r1-r5, sys 5 */
                wl_load_arg(st, "r0", args[0]);
                wl_load_arg(st, "r1", args[1]);
                wl_load_arg(st, "r2", args[2]);
                wl_load_arg(st, "r3", args[3]);
                wl_load_arg(st, "r4", args[4]);
                wl_load_arg(st, "r5", args[5]);
                wl_emit_line(st, "sys 5");
            } else if (wl_strcmp(func_name, "draw_circle") == 0 && argc >= 5) {
                /* draw_circle(win, cx, cy, r, color) → r0=win, r1-r4, sys 6 */
                wl_load_arg(st, "r0", args[0]);
                wl_load_arg(st, "r1", args[1]);
                wl_load_arg(st, "r2", args[2]);
                wl_load_arg(st, "r3", args[3]);
                wl_load_arg(st, "r4", args[4]);
                wl_emit_line(st, "sys 6");
            } else if (wl_strcmp(func_name, "get_mouse") == 0 && argc >= 1) {
                /* get_mouse(win) → r0=win, sys 7 */
                wl_load_arg(st, "r0", args[0]);
                wl_emit_line(st, "sys 7");
            } else if (wl_strcmp(func_name, "yield_cpu") == 0) {
                /* yield_cpu() → sys 8 */
                wl_emit_line(st, "sys 8");
            } else if (argc == 0) {
                /* Simple function call with no args */
                wl_emit(st, "    call _");
                wl_emit(st, func_name);
                wl_emit(st, "\n");
            } else {
                /* Unknown function with args - treat as call */
                wl_emit(st, "    call _");
                wl_emit(st, func_name);
                wl_emit(st, "\n");
            }
            (*idx)++;
            return;
        }
    }

    /* Unknown statement, skip */
    (*idx)++;
}

static void wl_compile_block(WLState *st, uint32_t *idx, uint32_t body_indent, bool is_main) {
    while (*idx < st->line_count && st->lines[*idx].indent >= body_indent) {
        wl_compile_stmt(st, idx, body_indent, is_main);
    }
}

/* ============================================================
 * PRE-SCAN: collect all string constants and variables
 * ============================================================ */

static void wl_prescan_line(WLState *st, const char *line, uint32_t len) {
    /* Scan for string literals to pre-register */
    /* (Strings are added during compilation, not here) */

    /* Scan for variable assignments: "ident = ..." */
    if (wl_is_alpha(line[0])) {
        char name[32];
        uint32_t off = wl_read_ident(line, len, 0, name, sizeof(name));
        off = wl_skip_ws(line, len, off);
        if (off < len && line[off] == '=' && (off + 1 >= len || line[off + 1] != '=')) {
            wl_alloc_var(st, name);
        }
    }
}

static void wl_prescan(WLState *st) {
    for (uint32_t i = 0; i < st->line_count; i++) {
        WLLine *ln = &st->lines[i];
        /* Skip def lines */
        if (wl_strncmp(ln->start, "def ", 4) != 0) {
            wl_prescan_line(st, ln->start, ln->len);
        }
    }
}

/* ============================================================
 * MAIN COMPILER ENTRY POINT
 * ============================================================ */

bool wynlang_compile(const char *src_path, const char *dest_path)
{
    VfsFile *sf = vfs_open(src_path);
    if (!sf) {
        serial_write_string("WynLang: ERROR - Could not open source file!\r\n");
        return false;
    }

    /* Read entire source into buffer */
    static char src_buf[WL_SRC_SIZE];
    int bytes = vfs_read(sf, src_buf, sizeof(src_buf) - 1);
    vfs_close(sf);
    if (bytes <= 0) {
        serial_write_string("WynLang: ERROR - Empty or unreadable source!\r\n");
        return false;
    }
    src_buf[bytes] = '\0';

    /* Allocate output buffer */
    char *out_buf = (char *)kmalloc(WL_OUT_SIZE);
    if (!out_buf) {
        serial_write_string("WynLang: ERROR - Out of memory!\r\n");
        return false;
    }
    memset(out_buf, 0, WL_OUT_SIZE);

    /* Initialize compiler state */
    static WLState state;
    memset(&state, 0, sizeof(WLState));
    state.out = out_buf;
    state.out_pos = 0;
    state.label_counter = 0;

    /* Parse source into lines */
    wl_parse_lines(&state, src_buf);

    if (state.line_count == 0) {
        serial_write_string("WynLang: ERROR - No valid source lines!\r\n");
        kfree(out_buf);
        return false;
    }

    /* Pre-scan to allocate variables */
    wl_prescan(&state);

    /* Reset variable table (will be re-allocated during compilation) */
    state.var_count = 0;
    state.next_reg = 0;

    /* ---- Collect function definitions ---- */
    /* Find all "def funcname():" lines and their body ranges */
    typedef struct {
        char name[32];
        uint32_t start_line;
        uint32_t body_indent;
        bool is_main;
    } FuncDef;

    FuncDef funcs[32];
    uint32_t func_count = 0;

    for (uint32_t i = 0; i < state.line_count; i++) {
        WLLine *ln = &state.lines[i];
        if (wl_strncmp(ln->start, "def ", 4) == 0) {
            char fname[32];
            wl_read_ident(ln->start, ln->len, 4, fname, sizeof(fname));

            FuncDef *fd = &funcs[func_count++];
            wl_strcpy(fd->name, fname);
            fd->start_line = i + 1; /* Body starts next line */
            fd->body_indent = ln->indent + 1;
            fd->is_main = (wl_strcmp(fname, "main") == 0);
        }
    }

    if (func_count == 0) {
        serial_write_string("WynLang: ERROR - No functions found!\r\n");
        kfree(out_buf);
        return false;
    }

    /* ---- First pass: compile all function bodies to collect strings ---- */
    /* We actually do a single pass: emit code, collect strings along the way,
     * then prepend .data section. Use a two-buffer approach. */

    /* Emit .code section into main buffer */
    wl_emit(&state, ".code\n");

    /* Emit entry point: jmp _main */
    wl_emit_line(&state, "jmp _main");

    /* Emit each function */
    for (uint32_t f = 0; f < func_count; f++) {
        FuncDef *fd = &funcs[f];

        /* Emit function label */
        wl_emit(&state, "_");
        wl_emit(&state, fd->name);
        wl_emit(&state, ":\n");

        /* Compile function body */
        uint32_t idx = fd->start_line;
        wl_compile_block(&state, &idx, fd->body_indent, fd->is_main);

        /* Emit halt for main, ret for others */
        if (fd->is_main) {
            wl_emit_line(&state, "halt");
        } else {
            wl_emit_line(&state, "ret");
        }
    }

    /* ---- Build final output: .data then .code ---- */
    char *final_buf = (char *)kmalloc(WL_OUT_SIZE);
    if (!final_buf) {
        kfree(out_buf);
        serial_write_string("WynLang: ERROR - Out of memory!\r\n");
        return false;
    }
    memset(final_buf, 0, WL_OUT_SIZE);

    uint32_t fpos = 0;

    /* Emit .data section */
    if (state.string_count > 0) {
        const char *dh = ".data\n";
        while (*dh && fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = *dh++;

        for (uint32_t i = 0; i < state.string_count; i++) {
            const char *indent = "    ";
            while (*indent && fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = *indent++;

            /* label: "content" */
            const char *lbl = state.strings[i].label;
            while (*lbl && fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = *lbl++;
            if (fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = ':';
            if (fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = ' ';
            if (fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = '"';

            const char *cnt = state.strings[i].content;
            while (*cnt && fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = *cnt++;

            if (fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = '"';
            if (fpos < WL_OUT_SIZE - 1) final_buf[fpos++] = '\n';
        }
    }

    /* Copy .code section */
    for (uint32_t i = 0; i < state.out_pos && fpos < WL_OUT_SIZE - 1; i++) {
        final_buf[fpos++] = state.out[i];
    }
    final_buf[fpos] = '\0';

    /* Write output file via VFS */
    VfsFile *df = vfs_open_flags(dest_path, VFS_O_CREATE | VFS_O_WRITE | VFS_O_TRUNC);
    if (!df) {
        serial_write_string("WynLang: ERROR - Could not create output file!\r\n");
        kfree(out_buf);
        kfree(final_buf);
        return false;
    }

    vfs_write(df, final_buf, fpos);
    vfs_close(df);

    kfree(out_buf);
    kfree(final_buf);

    serial_write_string("WynLang: Compilation successful.\r\n");
    return true;
}
