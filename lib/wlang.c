/*
 * WynlandOS - WynLang Interpreter
 * ================================
 * A complete tree-walk interpreter for WynLang.
 *
 * Architecture: Source → Lexer (tokens) → Parser (AST) → Interpreter (values)
 *
 * Block syntax uses ':' + 'end' (Python-inspired, explicit end markers):
 *   if x > 0:
 *       print(x)
 *   end
 */

#include <wynland/wlang.h>
#include <wynland/types.h>
#include <wynland/vfs.h>
#include <wynland/http.h>

extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);

/* Output callback — implemented in kernel/main.c */
extern void wlang_print_output(const char *text);

/* ============================================================
 * Constants & Limits
 * ============================================================ */

#define WL_MAX_NAME    64
#define WL_MAX_STR     512
#define WL_MAX_VARS    256
#define WL_MAX_CHILDREN 128
#define WL_MAX_PARAMS  8
#define WL_MAX_ALLOCS  4096

/* ============================================================
 * String helpers (freestanding)
 * ============================================================ */

static uint32_t wl_strlen(const char *s) {
    uint32_t n = 0;
    while (*s++) n++;
    return n;
}

static void wl_strcpy(char *d, const char *s) {
    while ((*d++ = *s++));
}

static void wl_strncpy(char *d, const char *s, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

static int wl_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(const unsigned char *)a - *(const unsigned char *)b;
}

static int wl_strncmp(const char *a, const char *b, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0) {
        if (*a != *b) return (int)(*(const unsigned char *)a - *(const unsigned char *)b);
        if (*a == '\0') return 0;
        a++;
        b++;
    }
    return 0;
}

static void wl_strcat(char *d, const char *s) {
    while (*d) d++;
    wl_strcpy(d, s);
}

static bool wl_isdigit(char c) { return c >= '0' && c <= '9'; }
static bool wl_isalpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool wl_isalnum(char c) { return wl_isalpha(c) || wl_isdigit(c); }
static bool wl_isspace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static void wl_int_to_str(int64_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[24];
    int i = 0;
    bool neg = false;
    if (v < 0) { neg = true; v = -v; }
    while (v > 0) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static int64_t wl_str_to_int(const char *s) {
    int64_t r = 0;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    while (wl_isdigit(*s)) { r = r * 10 + (*s - '0'); s++; }
    return neg ? -r : r;
}

/* ============================================================
 * Token Types
 * ============================================================ */

enum {
    T_INT, T_STR, T_TRUE, T_FALSE, T_NIL, T_IDENT,
    T_IF, T_ELIF, T_ELSE, T_WHILE, T_FOR, T_IN, T_FN, T_RETURN,
    T_AND, T_OR, T_NOT, T_END,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET,
    T_EQ, T_NEQ, T_LT, T_GT, T_LE, T_GE, T_ASSIGN,
    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_COLON, T_NEWLINE,
    T_EOF, T_ERROR
};

typedef struct {
    int type;
    int64_t num;
    char str[WL_MAX_STR];
    int line;
} Token;

/* ============================================================
 * Lexer
 * ============================================================ */

typedef struct {
    const char *src;
    int pos, len, line;
} Lexer;

static void lex_init(Lexer *l, const char *src) {
    l->src = src;
    l->pos = 0;
    l->len = (int)wl_strlen(src);
    l->line = 1;
}

static char lex_peek(Lexer *l) {
    return l->pos < l->len ? l->src[l->pos] : '\0';
}

static char lex_advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') l->line++;
    return c;
}

static void lex_skip_whitespace(Lexer *l) {
    while (l->pos < l->len && wl_isspace(l->src[l->pos])) l->pos++;
}

static void lex_skip_comment(Lexer *l) {
    if (l->pos < l->len && l->src[l->pos] == '#') {
        while (l->pos < l->len && l->src[l->pos] != '\n') l->pos++;
    }
}

static Token lex_next(Lexer *l) {
    Token t;
    memset(&t, 0, sizeof(Token));
    t.line = l->line;

    for (;;) {
        lex_skip_whitespace(l);
        lex_skip_comment(l);
        if (l->pos >= l->len) { t.type = T_EOF; return t; }
        if (l->src[l->pos] != '#') break;
    }

    char c = lex_peek(l);

    /* Newlines */
    if (c == '\n') {
        lex_advance(l);
        /* Collapse multiple newlines */
        while (l->pos < l->len && (l->src[l->pos] == '\n' || wl_isspace(l->src[l->pos]))) {
            if (l->src[l->pos] == '\n') l->line++;
            l->pos++;
        }
        t.type = T_NEWLINE;
        return t;
    }

    /* Numbers */
    if (wl_isdigit(c)) {
        int64_t val = 0;
        while (l->pos < l->len && wl_isdigit(l->src[l->pos])) {
            val = val * 10 + (l->src[l->pos] - '0');
            l->pos++;
        }
        t.type = T_INT;
        t.num = val;
        return t;
    }

    /* Strings */
    if (c == '"') {
        l->pos++; /* skip opening quote */
        int i = 0;
        while (l->pos < l->len && l->src[l->pos] != '"' && i < WL_MAX_STR - 1) {
            if (l->src[l->pos] == '\\' && l->pos + 1 < l->len) {
                l->pos++;
                switch (l->src[l->pos]) {
                    case 'n': t.str[i++] = '\n'; break;
                    case 't': t.str[i++] = '\t'; break;
                    case '\\': t.str[i++] = '\\'; break;
                    case '"': t.str[i++] = '"'; break;
                    default: t.str[i++] = l->src[l->pos]; break;
                }
                l->pos++;
            } else {
                t.str[i++] = l->src[l->pos++];
            }
        }
        t.str[i] = '\0';
        if (l->pos < l->len && l->src[l->pos] == '"') l->pos++;
        t.type = T_STR;
        return t;
    }

    /* Identifiers and keywords */
    if (wl_isalpha(c)) {
        int i = 0;
        while (l->pos < l->len && wl_isalnum(l->src[l->pos]) && i < WL_MAX_NAME - 1) {
            t.str[i++] = l->src[l->pos++];
        }
        t.str[i] = '\0';

        if (wl_strcmp(t.str, "if") == 0)      t.type = T_IF;
        else if (wl_strcmp(t.str, "elif") == 0) t.type = T_ELIF;
        else if (wl_strcmp(t.str, "else") == 0) t.type = T_ELSE;
        else if (wl_strcmp(t.str, "while") == 0) t.type = T_WHILE;
        else if (wl_strcmp(t.str, "for") == 0)   t.type = T_FOR;
        else if (wl_strcmp(t.str, "in") == 0)    t.type = T_IN;
        else if (wl_strcmp(t.str, "fn") == 0)    t.type = T_FN;
        else if (wl_strcmp(t.str, "return") == 0) t.type = T_RETURN;
        else if (wl_strcmp(t.str, "and") == 0)   t.type = T_AND;
        else if (wl_strcmp(t.str, "or") == 0)    t.type = T_OR;
        else if (wl_strcmp(t.str, "not") == 0)   t.type = T_NOT;
        else if (wl_strcmp(t.str, "end") == 0)   t.type = T_END;
        else if (wl_strcmp(t.str, "true") == 0)  t.type = T_TRUE;
        else if (wl_strcmp(t.str, "false") == 0) t.type = T_FALSE;
        else if (wl_strcmp(t.str, "nil") == 0)   t.type = T_NIL;
        else t.type = T_IDENT;
        return t;
    }

    /* Operators and delimiters */
    l->pos++;
    switch (c) {
        case '+': t.type = T_PLUS; break;
        case '-': t.type = T_MINUS; break;
        case '*': t.type = T_STAR; break;
        case '/': t.type = T_SLASH; break;
        case '%': t.type = T_PERCENT; break;
        case '^': t.type = T_CARET; break;
        case '(': t.type = T_LPAREN; break;
        case ')': t.type = T_RPAREN; break;
        case '[': t.type = T_LBRACKET; break;
        case ']': t.type = T_RBRACKET; break;
        case ',': t.type = T_COMMA; break;
        case ':': t.type = T_COLON; break;
        case '=':
            if (l->pos < l->len && l->src[l->pos] == '=') { l->pos++; t.type = T_EQ; }
            else t.type = T_ASSIGN;
            break;
        case '!':
            if (l->pos < l->len && l->src[l->pos] == '=') { l->pos++; t.type = T_NEQ; }
            else { t.type = T_ERROR; wl_strcpy(t.str, "unexpected '!'"); }
            break;
        case '<':
            if (l->pos < l->len && l->src[l->pos] == '=') { l->pos++; t.type = T_LE; }
            else t.type = T_LT;
            break;
        case '>':
            if (l->pos < l->len && l->src[l->pos] == '=') { l->pos++; t.type = T_GE; }
            else t.type = T_GT;
            break;
        default:
            t.type = T_ERROR;
            t.str[0] = c; t.str[1] = '\0';
            break;
    }
    return t;
}

/* ============================================================
 * AST Nodes
 * ============================================================ */

enum {
    N_INT, N_STR, N_BOOL, N_NIL,
    N_VAR, N_ASSIGN, N_BINOP, N_UNOP,
    N_IF, N_WHILE, N_FOR,
    N_BLOCK, N_FNDEF, N_CALL,
    N_RETURN, N_INDEX, N_ARRAY,
    N_EXPR_STMT
};

typedef struct Node {
    int type, line, op;
    int64_t num;
    char name[WL_MAX_NAME];
    struct Node *left, *right, *extra;
    struct Node *children[WL_MAX_CHILDREN];
    int nchildren;
    char params[WL_MAX_PARAMS][WL_MAX_NAME];
    int nparams;
} Node;

/* ============================================================
 * Interpreter Context
 * ============================================================ */

typedef enum { V_INT, V_STR, V_BOOL, V_NIL, V_FN } ValType;

typedef struct {
    ValType type;
    int64_t ival;
    const char *sval;
    bool    bval;
    Node   *fn_node;
    void   *fn_env; /* closure environment */
} Val;

typedef struct Env {
    char  names[WL_MAX_VARS][WL_MAX_NAME];
    Val   vals[WL_MAX_VARS];
    int   count;
    struct Env *parent;
} Env;

typedef struct {
    void *allocs[WL_MAX_ALLOCS];
    int   nallocs;
    bool  had_error;
    char  error[256];
    Env  *global;
    Val   ret_val;
    bool  returning;
} WLCtx;

static WLCtx wl_ctx;
static bool wl_persistent_mode = false;
static Env *wl_browser_env = NULL;
static char wl_http_buf[16384];

static const char *wl_strstr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return haystack;
        }
    }
    return NULL;
}

/* ============================================================
 * Memory management
 * ============================================================ */

static void *wl_alloc(size_t size) {
    void *p = kmalloc(size);
    if (p) {
        memset(p, 0, size);
        if (!wl_persistent_mode && wl_ctx.nallocs < WL_MAX_ALLOCS) {
            wl_ctx.allocs[wl_ctx.nallocs++] = p;
        }
    }
    return p;
}

static void wl_cleanup(void) {
    for (int i = 0; i < wl_ctx.nallocs; i++) {
        kfree(wl_ctx.allocs[i]);
    }
    wl_ctx.nallocs = 0;
}

static Node *node_new(int type, int line) {
    Node *n = (Node *)wl_alloc(sizeof(Node));
    if (n) { n->type = type; n->line = line; }
    return n;
}

static Env *env_new(Env *parent) {
    Env *e = (Env *)wl_alloc(sizeof(Env));
    if (e) { e->parent = parent; e->count = 0; }
    return e;
}

/* ============================================================
 * Error handling
 * ============================================================ */

static void wl_error(int line, const char *msg) {
    if (wl_ctx.had_error) return;
    wl_ctx.had_error = true;
    char buf[32];
    wl_int_to_str(line, buf);
    wl_strcpy(wl_ctx.error, "WynLang error (line ");
    wl_strcat(wl_ctx.error, buf);
    wl_strcat(wl_ctx.error, "): ");
    /* Append msg safely */
    uint32_t cur = wl_strlen(wl_ctx.error);
    uint32_t mlen = wl_strlen(msg);
    if (cur + mlen >= 255) mlen = 255 - cur;
    wl_strncpy(wl_ctx.error + cur, msg, mlen);
}

/* ============================================================
 * Parser
 * ============================================================ */

typedef struct {
    Lexer lex;
    Token cur;
    Token peeked;
    bool has_peek;
} Parser;

static Token parser_next(Parser *p) {
    if (p->has_peek) { p->has_peek = false; return p->peeked; }
    return lex_next(&p->lex);
}

/*
static Token parser_peek(Parser *p) {
    if (!p->has_peek) { p->peeked = lex_next(&p->lex); p->has_peek = true; }
    return p->peeked;
}
*/

static bool parser_match(Parser *p, int type) {
    if (p->cur.type == type) { p->cur = parser_next(p); return true; }
    return false;
}

static void parser_expect(Parser *p, int type) {
    if (p->cur.type != type) {
        wl_error(p->cur.line, "unexpected token");
        return;
    }
    p->cur = parser_next(p);
}

static void parser_skip_newlines(Parser *p) {
    while (p->cur.type == T_NEWLINE) p->cur = parser_next(p);
}

/* Forward declarations */
static Node *parse_expr(Parser *p);
static Node *parse_stmt(Parser *p);
static Node *parse_block(Parser *p);

/* ---- Expression parsing (precedence climbing) ---- */

static Node *parse_primary(Parser *p) {
    Token t = p->cur;
    Node *n;

    if (t.type == T_INT) {
        p->cur = parser_next(p);
        n = node_new(N_INT, t.line);
        n->num = t.num;
        return n;
    }
    if (t.type == T_STR) {
        p->cur = parser_next(p);
        n = node_new(N_STR, t.line);
        wl_strcpy(n->name, t.str);
        return n;
    }
    if (t.type == T_TRUE || t.type == T_FALSE) {
        p->cur = parser_next(p);
        n = node_new(N_BOOL, t.line);
        n->num = (t.type == T_TRUE) ? 1 : 0;
        return n;
    }
    if (t.type == T_NIL) {
        p->cur = parser_next(p);
        return node_new(N_NIL, t.line);
    }
    if (t.type == T_IDENT) {
        p->cur = parser_next(p);
        n = node_new(N_VAR, t.line);
        wl_strcpy(n->name, t.str);
        return n;
    }
    if (t.type == T_LPAREN) {
        p->cur = parser_next(p);
        n = parse_expr(p);
        parser_expect(p, T_RPAREN);
        return n;
    }
    if (t.type == T_LBRACKET) {
        p->cur = parser_next(p);
        n = node_new(N_ARRAY, t.line);
        while (p->cur.type != T_RBRACKET && p->cur.type != T_EOF) {
            if (n->nchildren >= WL_MAX_CHILDREN) break;
            n->children[n->nchildren++] = parse_expr(p);
            if (!parser_match(p, T_COMMA)) break;
        }
        parser_expect(p, T_RBRACKET);
        return n;
    }
    if (t.type == T_MINUS) {
        p->cur = parser_next(p);
        n = node_new(N_UNOP, t.line);
        n->op = T_MINUS;
        n->left = parse_primary(p);
        return n;
    }
    if (t.type == T_NOT) {
        p->cur = parser_next(p);
        n = node_new(N_UNOP, t.line);
        n->op = T_NOT;
        n->left = parse_expr(p);
        return n;
    }

    wl_error(t.line, "expected expression");
    p->cur = parser_next(p);
    return node_new(N_NIL, t.line);
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);

    for (;;) {
        if (p->cur.type == T_LPAREN) {
            /* Function call */
            int line = p->cur.line;
            p->cur = parser_next(p);
            Node *call = node_new(N_CALL, line);
            call->left = n;
            while (p->cur.type != T_RPAREN && p->cur.type != T_EOF) {
                if (call->nchildren >= WL_MAX_CHILDREN) break;
                call->children[call->nchildren++] = parse_expr(p);
                if (!parser_match(p, T_COMMA)) break;
            }
            parser_expect(p, T_RPAREN);
            n = call;
        } else if (p->cur.type == T_LBRACKET) {
            /* Index access */
            int line = p->cur.line;
            p->cur = parser_next(p);
            Node *idx = node_new(N_INDEX, line);
            idx->left = n;
            idx->right = parse_expr(p);
            parser_expect(p, T_RBRACKET);
            n = idx;
        } else {
            break;
        }
    }
    return n;
}

static Node *parse_pow(Parser *p) {
    Node *left = parse_postfix(p);
    while (p->cur.type == T_CARET) {
        int op = p->cur.type;
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_BINOP, line);
        n->op = op; n->left = left; n->right = parse_postfix(p);
        left = n;
    }
    return left;
}

static Node *parse_mul(Parser *p) {
    Node *left = parse_pow(p);
    while (p->cur.type == T_STAR || p->cur.type == T_SLASH || p->cur.type == T_PERCENT) {
        int op = p->cur.type;
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_BINOP, line);
        n->op = op; n->left = left; n->right = parse_pow(p);
        left = n;
    }
    return left;
}

static Node *parse_add(Parser *p) {
    Node *left = parse_mul(p);
    while (p->cur.type == T_PLUS || p->cur.type == T_MINUS) {
        int op = p->cur.type;
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_BINOP, line);
        n->op = op; n->left = left; n->right = parse_mul(p);
        left = n;
    }
    return left;
}

static Node *parse_cmp(Parser *p) {
    Node *left = parse_add(p);
    if (p->cur.type == T_EQ || p->cur.type == T_NEQ ||
        p->cur.type == T_LT || p->cur.type == T_GT ||
        p->cur.type == T_LE || p->cur.type == T_GE) {
        int op = p->cur.type;
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_BINOP, line);
        n->op = op; n->left = left; n->right = parse_add(p);
        return n;
    }
    return left;
}

static Node *parse_and(Parser *p) {
    Node *left = parse_cmp(p);
    while (p->cur.type == T_AND) {
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_BINOP, line);
        n->op = T_AND; n->left = left; n->right = parse_cmp(p);
        left = n;
    }
    return left;
}

static Node *parse_expr(Parser *p) {
    Node *left = parse_and(p);
    while (p->cur.type == T_OR) {
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_BINOP, line);
        n->op = T_OR; n->left = left; n->right = parse_and(p);
        left = n;
    }
    return left;
}

/* ---- Block parsing ---- */

static Node *parse_block(Parser *p) {
    Node *block = node_new(N_BLOCK, p->cur.line);
    parser_skip_newlines(p);
    while (p->cur.type != T_END && p->cur.type != T_ELIF &&
           p->cur.type != T_ELSE && p->cur.type != T_EOF) {
        if (wl_ctx.had_error) break;
        if (block->nchildren >= WL_MAX_CHILDREN) { wl_error(p->cur.line, "block too large"); break; }
        Node *s = parse_stmt(p);
        if (s) block->children[block->nchildren++] = s;
        parser_skip_newlines(p);
    }
    return block;
}

/* ---- Statement parsing ---- */

static Node *parse_if(Parser *p) {
    int line = p->cur.line;
    p->cur = parser_next(p); /* skip 'if' */
    Node *n = node_new(N_IF, line);
    n->left = parse_expr(p);   /* condition */
    parser_expect(p, T_COLON);
    parser_skip_newlines(p);
    n->right = parse_block(p); /* then-block */

    /* elif branches stored in children (pairs: condition, block) */
    while (p->cur.type == T_ELIF) {
        p->cur = parser_next(p);
        if (n->nchildren + 1 >= WL_MAX_CHILDREN) break;
        n->children[n->nchildren++] = parse_expr(p); /* elif condition */
        parser_expect(p, T_COLON);
        parser_skip_newlines(p);
        n->children[n->nchildren++] = parse_block(p); /* elif block */
    }

    /* else branch */
    if (p->cur.type == T_ELSE) {
        p->cur = parser_next(p);
        parser_expect(p, T_COLON);
        parser_skip_newlines(p);
        n->extra = parse_block(p);
    }

    parser_expect(p, T_END);
    return n;
}

static Node *parse_while(Parser *p) {
    int line = p->cur.line;
    p->cur = parser_next(p);
    Node *n = node_new(N_WHILE, line);
    n->left = parse_expr(p);
    parser_expect(p, T_COLON);
    parser_skip_newlines(p);
    n->right = parse_block(p);
    parser_expect(p, T_END);
    return n;
}

static Node *parse_for(Parser *p) {
    int line = p->cur.line;
    p->cur = parser_next(p);
    Node *n = node_new(N_FOR, line);
    if (p->cur.type != T_IDENT) { wl_error(line, "expected variable after 'for'"); return n; }
    wl_strcpy(n->name, p->cur.str);
    p->cur = parser_next(p);
    parser_expect(p, T_IN);
    n->left = parse_expr(p); /* iterable (range call or array) */
    parser_expect(p, T_COLON);
    parser_skip_newlines(p);
    n->right = parse_block(p);
    parser_expect(p, T_END);
    return n;
}

static Node *parse_fn(Parser *p) {
    int line = p->cur.line;
    p->cur = parser_next(p);
    Node *n = node_new(N_FNDEF, line);
    if (p->cur.type != T_IDENT) { wl_error(line, "expected function name"); return n; }
    wl_strcpy(n->name, p->cur.str);
    p->cur = parser_next(p);
    parser_expect(p, T_LPAREN);
    while (p->cur.type == T_IDENT && n->nparams < WL_MAX_PARAMS) {
        wl_strcpy(n->params[n->nparams++], p->cur.str);
        p->cur = parser_next(p);
        if (!parser_match(p, T_COMMA)) break;
    }
    parser_expect(p, T_RPAREN);
    parser_expect(p, T_COLON);
    parser_skip_newlines(p);
    n->right = parse_block(p);
    parser_expect(p, T_END);
    return n;
}

static Node *parse_return(Parser *p) {
    int line = p->cur.line;
    p->cur = parser_next(p);
    Node *n = node_new(N_RETURN, line);
    if (p->cur.type != T_NEWLINE && p->cur.type != T_EOF && p->cur.type != T_END) {
        n->left = parse_expr(p);
    }
    return n;
}

static Node *parse_stmt(Parser *p) {
    parser_skip_newlines(p);
    if (wl_ctx.had_error || p->cur.type == T_EOF) return NULL;

    if (p->cur.type == T_IF)     return parse_if(p);
    if (p->cur.type == T_WHILE)  return parse_while(p);
    if (p->cur.type == T_FOR)    return parse_for(p);
    if (p->cur.type == T_FN)     return parse_fn(p);
    if (p->cur.type == T_RETURN) return parse_return(p);

    /* Assignment or expression statement */
    Node *expr = parse_expr(p);
    if (!expr) return NULL;

    /* Check for assignment: IDENT = expr */
    if (expr->type == N_VAR && p->cur.type == T_ASSIGN) {
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_ASSIGN, line);
        wl_strcpy(n->name, expr->name);
        n->left = parse_expr(p);
        parser_match(p, T_NEWLINE);
        return n;
    }

    /* Index assignment: arr[i] = expr */
    if (expr->type == N_INDEX && p->cur.type == T_ASSIGN) {
        int line = p->cur.line;
        p->cur = parser_next(p);
        Node *n = node_new(N_ASSIGN, line);
        n->left = parse_expr(p);
        n->right = expr->right; /* index expr */
        n->extra = expr->left;  /* array expr */
        n->name[0] = '\0'; /* empty name signals index assignment */
        parser_match(p, T_NEWLINE);
        return n;
    }

    /* Expression statement */
    Node *s = node_new(N_EXPR_STMT, expr->line);
    s->left = expr;
    parser_match(p, T_NEWLINE);
    return s;
}

static Node *parse_program(Parser *p) {
    Node *prog = node_new(N_BLOCK, 1);
    parser_skip_newlines(p);
    while (p->cur.type != T_EOF && !wl_ctx.had_error) {
        if (prog->nchildren >= WL_MAX_CHILDREN) { wl_error(p->cur.line, "program too large"); break; }
        Node *s = parse_stmt(p);
        if (s) prog->children[prog->nchildren++] = s;
        parser_skip_newlines(p);
    }
    return prog;
}

/* ============================================================
 * Environment (variable scope)
 * ============================================================ */

static bool env_get(Env *e, const char *name, Val *out) {
    while (e) {
        for (int i = 0; i < e->count; i++) {
            if (wl_strcmp(e->names[i], name) == 0) {
                *out = e->vals[i];
                return true;
            }
        }
        e = e->parent;
    }
    return false;
}

static void env_set(Env *e, const char *name, Val val) {
    /* Update existing in current or parent scope */
    Env *search = e;
    while (search) {
        for (int i = 0; i < search->count; i++) {
            if (wl_strcmp(search->names[i], name) == 0) {
                search->vals[i] = val;
                return;
            }
        }
        search = search->parent;
    }
    /* New variable in current scope */
    if (e->count < WL_MAX_VARS) {
        wl_strcpy(e->names[e->count], name);
        e->vals[e->count] = val;
        e->count++;
    }
}

static void env_set_local(Env *e, const char *name, Val val) {
    /* Only set in current scope (for function params) */
    for (int i = 0; i < e->count; i++) {
        if (wl_strcmp(e->names[i], name) == 0) {
            e->vals[i] = val;
            return;
        }
    }
    if (e->count < WL_MAX_VARS) {
        wl_strcpy(e->names[e->count], name);
        e->vals[e->count] = val;
        e->count++;
    }
}

/* ============================================================
 * Value helpers
 * ============================================================ */

static Val val_int(int64_t v)  { Val r; memset(&r, 0, sizeof(r)); r.type = V_INT; r.ival = v; return r; }
static Val val_bool(bool v)    { Val r; memset(&r, 0, sizeof(r)); r.type = V_BOOL; r.bval = v; return r; }
static Val val_nil(void)       { Val r; memset(&r, 0, sizeof(r)); r.type = V_NIL; return r; }

static Val val_str(const char *s) {
    Val r; memset(&r, 0, sizeof(r));
    r.type = V_STR;
    char *dup = (char *)wl_alloc(wl_strlen(s) + 1);
    if (dup) wl_strcpy(dup, s);
    r.sval = dup;
    return r;
}

static Val val_fn(Node *n, Env *e) {
    Val r; memset(&r, 0, sizeof(r));
    r.type = V_FN;
    r.fn_node = n;
    r.fn_env = e;
    return r;
}

static bool val_truthy(Val v) {
    switch (v.type) {
        case V_INT:  return v.ival != 0;
        case V_STR:  return v.sval && v.sval[0] != '\0';
        case V_BOOL: return v.bval;
        case V_NIL:  return false;
        case V_FN:   return true;
    }
    return false;
}

static void val_to_str(Val v, char *buf) {
    switch (v.type) {
        case V_INT:  wl_int_to_str(v.ival, buf); break;
        case V_STR:  wl_strcpy(buf, v.sval ? v.sval : ""); break;
        case V_BOOL: wl_strcpy(buf, v.bval ? "true" : "false"); break;
        case V_NIL:  wl_strcpy(buf, "nil"); break;
        case V_FN:   wl_strcpy(buf, "<function>"); break;
    }
}

/* ============================================================
 * Interpreter (tree-walk evaluator)
 * ============================================================ */

static Val eval(Node *node, Env *env);

static Val eval_block(Node *block, Env *env) {
    Val result = val_nil();
    for (int i = 0; i < block->nchildren; i++) {
        if (wl_ctx.had_error || wl_ctx.returning) break;
        result = eval(block->children[i], env);
    }
    return result;
}

static void wlang_ls_callback(VfsNode *node) {
    wlang_print_output(node->is_dir ? "  [DIR]  " : "  [FILE] ");
    wlang_print_output(node->name);
    if (!node->is_dir) {
        wlang_print_output(" (");
        char sz[32];
        wl_int_to_str((int64_t)node->size, sz);
        wlang_print_output(sz);
        wlang_print_output(" bytes)");
    }
    wlang_print_output("\n");
}

static void sanitize_html(const char *html, char *out, uint32_t max_len)
{
    uint32_t in_pos = 0;
    uint32_t out_pos = 0;
    bool in_script = false;
    bool in_style = false;
    bool in_head = false;
    bool last_was_space = false;
    
    while (html[in_pos] && out_pos < max_len - 1) {
        if (html[in_pos] == '<') {
            bool keep = false;
            
            if (wl_strncmp(html + in_pos, "<a ", 3) == 0 ||
                wl_strncmp(html + in_pos, "<a>", 3) == 0 ||
                wl_strncmp(html + in_pos, "</a>", 4) == 0 ||
                wl_strncmp(html + in_pos, "<h1>", 4) == 0 ||
                wl_strncmp(html + in_pos, "</h1>", 5) == 0 ||
                wl_strncmp(html + in_pos, "<h2>", 4) == 0 ||
                wl_strncmp(html + in_pos, "</h2>", 5) == 0 ||
                wl_strncmp(html + in_pos, "<p>", 3) == 0 ||
                wl_strncmp(html + in_pos, "<p ", 3) == 0 ||
                wl_strncmp(html + in_pos, "</p>", 4) == 0 ||
                wl_strncmp(html + in_pos, "<br>", 4) == 0 ||
                wl_strncmp(html + in_pos, "<br/>", 5) == 0 ||
                wl_strncmp(html + in_pos, "<br ", 4) == 0) {
                keep = true;
            }
            
            if (wl_strncmp(html + in_pos, "<script", 7) == 0) {
                in_script = true;
            } else if (wl_strncmp(html + in_pos, "<style", 6) == 0) {
                in_style = true;
            } else if (wl_strncmp(html + in_pos, "<head", 5) == 0) {
                in_head = true;
            } else if (wl_strncmp(html + in_pos, "</script>", 9) == 0) {
                in_script = false;
                in_pos += 9;
                continue;
            } else if (wl_strncmp(html + in_pos, "</style>", 8) == 0) {
                in_style = false;
                in_pos += 8;
                continue;
            } else if (wl_strncmp(html + in_pos, "</head>", 7) == 0) {
                in_head = false;
                in_pos += 7;
                continue;
            }
            
            if (keep) {
                while (html[in_pos] && html[in_pos] != '>') {
                    if (out_pos < max_len - 1) {
                        out[out_pos++] = html[in_pos++];
                    } else {
                        break;
                    }
                }
                if (html[in_pos] == '>') {
                    if (out_pos < max_len - 1) {
                        out[out_pos++] = html[in_pos++];
                    }
                }
                continue;
            } else {
                if (wl_strncmp(html + in_pos, "<div", 4) == 0 ||
                    wl_strncmp(html + in_pos, "</div", 5) == 0 ||
                    wl_strncmp(html + in_pos, "<tr", 3) == 0 ||
                    wl_strncmp(html + in_pos, "<li", 3) == 0 ||
                    wl_strncmp(html + in_pos, "<ul", 3) == 0) {
                    if (out_pos > 0 && out[out_pos - 1] != '\n') {
                        out[out_pos++] = '\n';
                    }
                }
                
                while (html[in_pos] && html[in_pos] != '>') {
                    in_pos++;
                }
                if (html[in_pos] == '>') {
                    in_pos++;
                }
                continue;
            }
        } else {
            char c = html[in_pos];
            if (in_script || in_style || in_head) {
                in_pos++;
                continue;
            }
            
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if (!last_was_space) {
                    out[out_pos++] = (c == '\n' || c == '\r') ? '\n' : ' ';
                    last_was_space = true;
                }
            } else {
                out[out_pos++] = c;
                last_was_space = false;
            }
            in_pos++;
        }
    }
    out[out_pos] = '\0';
}

static void sanitize_buf(int len) {
    if (len < 0) return;
    wl_http_buf[len] = '\0';
    char *temp_html = (char *)kmalloc(16384);
    if (temp_html) {
        wl_strcpy(temp_html, wl_http_buf);
        sanitize_html(temp_html, wl_http_buf, sizeof(wl_http_buf));
        kfree(temp_html);
    }
}

/* Built-in function dispatch */
static bool call_builtin(const char *name, Val *args, int nargs, Val *out) {

    if (wl_strcmp(name, "print") == 0) {
        char buf[WL_MAX_STR];
        for (int i = 0; i < nargs; i++) {
            val_to_str(args[i], buf);
            wlang_print_output(buf);
            if (i < nargs - 1) wlang_print_output(" ");
        }
        wlang_print_output("\n");
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "len") == 0 && nargs == 1) {
        if (args[0].type == V_STR) {
            *out = val_int((int64_t)wl_strlen(args[0].sval ? args[0].sval : ""));
        } else {
            *out = val_int(0);
        }
        return true;
    }

    if (wl_strcmp(name, "str") == 0 && nargs == 1) {
        char buf[WL_MAX_STR];
        val_to_str(args[0], buf);
        *out = val_str(buf);
        return true;
    }

    if (wl_strcmp(name, "int") == 0 && nargs == 1) {
        if (args[0].type == V_STR) {
            *out = val_int(wl_str_to_int(args[0].sval ? args[0].sval : ""));
        } else if (args[0].type == V_INT) {
            *out = args[0];
        } else if (args[0].type == V_BOOL) {
            *out = val_int(args[0].bval ? 1 : 0);
        } else {
            *out = val_int(0);
        }
        return true;
    }

    if (wl_strcmp(name, "type") == 0 && nargs == 1) {
        const char *names[] = {"int", "str", "bool", "nil", "fn"};
        *out = val_str(names[args[0].type]);
        return true;
    }

    if (wl_strcmp(name, "range") == 0 && nargs >= 1) {
        /* range() returns a special marker; handled by for-loop */
        /* but if called standalone, return an integer (upper bound) */
        *out = args[0];
        return true;
    }

    /* OS built-ins */
    if (wl_strcmp(name, "ls") == 0 && nargs <= 1) {
        const char *path = (nargs == 1 && args[0].sval) ? args[0].sval : "/";
        if (!vfs_readdir(path, wlang_ls_callback)) {
            wlang_print_output("ls error: directory not found or failed to read\n");
            *out = val_bool(false);
        } else {
            *out = val_bool(true);
        }
        return true;
    }

    if (wl_strcmp(name, "mkdir") == 0 && nargs == 1) {
        *out = val_bool(args[0].sval ? vfs_mkdir(args[0].sval) : false);
        return true;
    }

    if (wl_strcmp(name, "rm") == 0 && nargs == 1) {
        *out = val_bool(args[0].sval ? vfs_delete(args[0].sval) : false);
        return true;
    }

    if (wl_strcmp(name, "input") == 0 && nargs <= 1) {
        if (nargs == 1) {
            char pbuf[WL_MAX_STR];
            val_to_str(args[0], pbuf);
            wlang_print_output(pbuf);
        }
        char buf[WL_MAX_STR];
        extern void wlang_read_input(char *buf, uint32_t max_len);
        wlang_read_input(buf, WL_MAX_STR);
        *out = val_str(buf);
        return true;
    }

    if (wl_strcmp(name, "read") == 0 && nargs == 1) {
        VfsFile *f = args[0].sval ? vfs_open(args[0].sval) : NULL;
        if (!f) { *out = val_nil(); return true; }
        
        #define WL_READ_BUFSZ 16384
        char *buf = (char *)kmalloc(WL_READ_BUFSZ);
        if (!buf) {
            vfs_close(f);
            *out = val_nil();
            return true;
        }
        
        int rd = vfs_read(f, buf, WL_READ_BUFSZ - 1);
        buf[rd > 0 ? rd : 0] = '\0';
        vfs_close(f);
        *out = val_str(buf);
        kfree(buf);
        return true;
    }

    if (wl_strcmp(name, "write") == 0 && nargs == 2) {
        VfsFile *f = args[0].sval ? vfs_open_flags(args[0].sval, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC) : NULL;
        if (!f) { *out = val_bool(false); return true; }
        char buf[WL_MAX_STR];
        val_to_str(args[1], buf);
        uint32_t slen = wl_strlen(buf);
        vfs_write(f, buf, slen);
        vfs_close(f);
        *out = val_bool(true);
        return true;
    }

    if (wl_strcmp(name, "exists") == 0 && nargs == 1) {
        VfsStat st;
        *out = val_bool(args[0].sval ? vfs_stat(args[0].sval, &st) : false);
        return true;
    }

    if (wl_strcmp(name, "exit") == 0) {
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "get_time") == 0 && nargs == 0) {
        extern uint64_t timer_get_ticks(void);
        *out = val_int((int64_t)(timer_get_ticks() * 10)); /* 100 Hz timer -> 10ms ticks */
        return true;
    }

    if (wl_strcmp(name, "network_ping") == 0 && nargs == 1) {
        if (args[0].type == V_STR) {
            extern uint32_t net_str_to_ip(const char *str);
            extern int net_ping(uint32_t ip);
            uint32_t target_ip = net_str_to_ip(args[0].sval ? args[0].sval : "");
            if (target_ip == 0) {
                *out = val_int(-1);
            } else {
                *out = val_int((int64_t)net_ping(target_ip));
            }
        } else {
            *out = val_int(-1);
        }
        return true;
    }

    if (wl_strcmp(name, "delay") == 0 && nargs == 1) {
        if (args[0].type == V_INT) {
            extern uint64_t timer_get_ticks(void);
            uint64_t start = timer_get_ticks();
            uint64_t ticks_to_wait = args[0].ival / 10;
            while (timer_get_ticks() - start < ticks_to_wait) {
                __asm__ volatile("hlt");
            }
        }
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "str_find") == 0 && nargs == 2) {
        if (args[0].type == V_STR && args[1].type == V_STR) {
            const char *haystack = args[0].sval ? args[0].sval : "";
            const char *needle = args[1].sval ? args[1].sval : "";
            const char *pos = wl_strstr(haystack, needle);
            if (pos) {
                *out = val_int((int64_t)(pos - haystack));
            } else {
                *out = val_int(-1);
            }
        } else {
            *out = val_int(-1);
        }
        return true;
    }

    if (wl_strcmp(name, "str_slice") == 0 && nargs == 3) {
        if (args[0].type == V_STR && args[1].type == V_INT && args[2].type == V_INT) {
            const char *s = args[0].sval ? args[0].sval : "";
            int64_t start = args[1].ival;
            int64_t length = args[2].ival;
            int64_t slen = (int64_t)wl_strlen(s);
            if (start < 0) start = 0;
            if (start > slen) start = slen;
            if (length < 0) length = 0;
            if (start + length > slen) length = slen - start;

            char *buf = (char *)wl_alloc(length + 1);
            if (buf) {
                for (int64_t i = 0; i < length; i++) {
                    buf[i] = s[start + i];
                }
                buf[length] = '\0';
            }
            *out = val_str(buf ? buf : "");
        } else {
            *out = val_str("");
        }
        return true;
    }

    if (wl_strcmp(name, "gui_get_x") == 0 && nargs == 0) {
        extern int32_t wm_get_browser_x(void);
        *out = val_int((int64_t)wm_get_browser_x());
        return true;
    }

    if (wl_strcmp(name, "gui_get_y") == 0 && nargs == 0) {
        extern int32_t wm_get_browser_y(void);
        *out = val_int((int64_t)wm_get_browser_y());
        return true;
    }

    if (wl_strcmp(name, "gui_get_w") == 0 && nargs == 0) {
        extern int32_t wm_get_browser_w(void);
        *out = val_int((int64_t)wm_get_browser_w());
        return true;
    }

    if (wl_strcmp(name, "gui_get_h") == 0 && nargs == 0) {
        extern int32_t wm_get_browser_h(void);
        *out = val_int((int64_t)wm_get_browser_h());
        return true;
    }

    if (wl_strcmp(name, "gui_draw_rect") == 0 && nargs == 5) {
        extern void comp_fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color);
        comp_fill_rect(
            (int32_t)args[0].ival,
            (int32_t)args[1].ival,
            (uint32_t)args[2].ival,
            (uint32_t)args[3].ival,
            (uint32_t)args[4].ival
        );
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "gui_draw_text") == 0 && nargs == 4) {
        extern void comp_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg);
        comp_draw_string(
            (int32_t)args[0].ival,
            (int32_t)args[1].ival,
            args[2].sval ? args[2].sval : "",
            (uint32_t)args[3].ival,
            0
        );
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "gui_draw_char") == 0 && nargs == 4) {
        extern void comp_draw_char(uint32_t x, uint32_t y, uint16_t c, uint32_t fg, uint32_t bg);
        const char *s = args[2].sval ? args[2].sval : "";
        comp_draw_char(
            (uint32_t)args[0].ival,
            (uint32_t)args[1].ival,
            (uint16_t)s[0],
            (uint32_t)args[3].ival,
            0
        );
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "gui_draw_rounded_rect") == 0 && nargs == 6) {
        extern void comp_draw_rounded_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);
        comp_draw_rounded_rect(
            (int32_t)args[0].ival,
            (int32_t)args[1].ival,
            (uint32_t)args[2].ival,
            (uint32_t)args[3].ival,
            (uint32_t)args[4].ival,
            (uint32_t)args[5].ival
        );
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "gui_draw_rounded_rect_border") == 0 && nargs == 6) {
        extern void comp_draw_rounded_rect_border(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);
        comp_draw_rounded_rect_border(
            (int32_t)args[0].ival,
            (int32_t)args[1].ival,
            (uint32_t)args[2].ival,
            (uint32_t)args[3].ival,
            (uint32_t)args[4].ival,
            (uint32_t)args[5].ival
        );
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "gui_mark_dirty") == 0 && nargs == 0) {
        extern void comp_mark_dirty(void);
        comp_mark_dirty();
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "http_get") == 0 && nargs == 3) {
        if (args[0].type == V_STR && args[1].type == V_INT && args[2].type == V_STR) {
            const char *host = args[0].sval ? args[0].sval : "";
            uint16_t port = (uint16_t)args[1].ival;
            const char *path = args[2].sval ? args[2].sval : "";

            HttpResponse resp;
            memset(wl_http_buf, 0, sizeof(wl_http_buf));
            int ret = http_get(host, port, path, wl_http_buf, sizeof(wl_http_buf) - 1, &resp);
            if (ret >= 0) {
                sanitize_buf(ret);
                *out = val_str(wl_http_buf);
            } else {
                extern uint32_t net_str_to_ip(const char *str);
                uint32_t ip = net_str_to_ip(host);
                if (ip != 0) {
                    ret = http_get_ip(ip, port, host, path, wl_http_buf, sizeof(wl_http_buf) - 1, &resp);
                }
                if (ret >= 0) {
                    sanitize_buf(ret);
                    *out = val_str(wl_http_buf);
                } else {
                    *out = val_str("<html><h1>Connection Error</h1><p>Could not connect to host.</p></html>");
                }
            }
        } else {
            *out = val_str("");
        }
        return true;
    }

    if (wl_strcmp(name, "https_get") == 0 && nargs == 3) {
        if (args[0].type == V_STR && args[1].type == V_INT && args[2].type == V_STR) {
            const char *host = args[0].sval ? args[0].sval : "";
            uint16_t port = (uint16_t)args[1].ival;
            const char *path = args[2].sval ? args[2].sval : "";

            HttpResponse resp;
            memset(wl_http_buf, 0, sizeof(wl_http_buf));
            int ret = https_get(host, port, path, wl_http_buf, sizeof(wl_http_buf) - 1, &resp);
            if (ret >= 0) {
                sanitize_buf(ret);
                *out = val_str(wl_http_buf);
            } else {
                extern uint32_t net_str_to_ip(const char *str);
                uint32_t ip = net_str_to_ip(host);
                if (ip != 0) {
                    ret = https_get_ip(ip, port, host, path, wl_http_buf, sizeof(wl_http_buf) - 1, &resp);
                }
                if (ret >= 0) {
                    sanitize_buf(ret);
                    *out = val_str(wl_http_buf);
                } else {
                    *out = val_str("<html><h1>Connection Error</h1><p>Could not connect to secure host.</p></html>");
                }
            }
        } else {
            *out = val_str("");
        }
        return true;
    }

    if (wl_strcmp(name, "js_init") == 0 && nargs == 0) {
        extern void js_engine_init(void);
        js_engine_init();
        *out = val_nil();
        return true;
    }

    if (wl_strcmp(name, "js_eval") == 0 && nargs == 1) {
        if (args[0].type == V_STR) {
            extern const char *js_engine_eval(const char *code);
            const char *code = args[0].sval ? args[0].sval : "";
            const char *res = js_engine_eval(code);
            *out = val_str(res ? res : "");
        } else {
            *out = val_str("Error: js_eval expects string");
        }
        return true;
    }

    if (wl_strcmp(name, "js_get_state") == 0 && nargs == 1) {
        if (args[0].type == V_STR) {
            extern int js_engine_get_state(const char *name);
            const char *state_name = args[0].sval ? args[0].sval : "";
            int val = js_engine_get_state(state_name);
            *out = val_int(val);
        } else {
            *out = val_int(-1);
        }
        return true;
    }

    return false;
}

static int64_t int_pow(int64_t base, int64_t exp) {
    if (exp < 0) return 0;
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return result;
}

static Val eval(Node *node, Env *env) {
    if (!node || wl_ctx.had_error || wl_ctx.returning) return val_nil();

    switch (node->type) {
    case N_INT:  return val_int(node->num);
    case N_STR:  return val_str(node->name);
    case N_BOOL: return val_bool(node->num != 0);
    case N_NIL:  return val_nil();

    case N_VAR: {
        Val v;
        if (env_get(env, node->name, &v)) return v;
        wl_error(node->line, "undefined variable");
        return val_nil();
    }

    case N_ASSIGN: {
        Val v = eval(node->left, env);
        if (node->name[0] != '\0') {
            /* Simple assignment: name = expr */
            env_set(env, node->name, v);
        }
        /* Index assignment handled separately if needed */
        return v;
    }

    case N_BINOP: {
        Val left = eval(node->left, env);
        Val right = eval(node->right, env);

        /* String concatenation */
        if (node->op == T_PLUS && (left.type == V_STR || right.type == V_STR)) {
            char buf[WL_MAX_STR], lb[WL_MAX_STR], rb[WL_MAX_STR];
            val_to_str(left, lb);
            val_to_str(right, rb);
            wl_strcpy(buf, lb);
            wl_strcat(buf, rb);
            return val_str(buf);
        }

        /* Equality works for all types */
        if (node->op == T_EQ) {
            if (left.type != right.type) return val_bool(false);
            switch (left.type) {
                case V_INT:  return val_bool(left.ival == right.ival);
                case V_STR:  return val_bool(wl_strcmp(left.sval ? left.sval : "", right.sval ? right.sval : "") == 0);
                case V_BOOL: return val_bool(left.bval == right.bval);
                case V_NIL:  return val_bool(true);
                default:     return val_bool(false);
            }
        }
        if (node->op == T_NEQ) {
            if (left.type != right.type) return val_bool(true);
            switch (left.type) {
                case V_INT:  return val_bool(left.ival != right.ival);
                case V_STR:  return val_bool(wl_strcmp(left.sval ? left.sval : "", right.sval ? right.sval : "") != 0);
                case V_BOOL: return val_bool(left.bval != right.bval);
                case V_NIL:  return val_bool(false);
                default:     return val_bool(true);
            }
        }

        /* Logical operators */
        if (node->op == T_AND) return val_bool(val_truthy(left) && val_truthy(right));
        if (node->op == T_OR)  return val_bool(val_truthy(left) || val_truthy(right));

        /* Arithmetic — require integers */
        if (left.type != V_INT || right.type != V_INT) {
            wl_error(node->line, "arithmetic requires integers");
            return val_nil();
        }
        switch (node->op) {
            case T_PLUS:  return val_int(left.ival + right.ival);
            case T_MINUS: return val_int(left.ival - right.ival);
            case T_STAR:  return val_int(left.ival * right.ival);
            case T_SLASH:
                if (right.ival == 0) { wl_error(node->line, "division by zero"); return val_nil(); }
                return val_int(left.ival / right.ival);
            case T_PERCENT:
                if (right.ival == 0) { wl_error(node->line, "modulo by zero"); return val_nil(); }
                return val_int(left.ival % right.ival);
            case T_CARET:
                return val_int(int_pow(left.ival, right.ival));
            case T_LT: return val_bool(left.ival < right.ival);
            case T_GT: return val_bool(left.ival > right.ival);
            case T_LE: return val_bool(left.ival <= right.ival);
            case T_GE: return val_bool(left.ival >= right.ival);
            default: break;
        }
        return val_nil();
    }

    case N_UNOP: {
        Val v = eval(node->left, env);
        if (node->op == T_MINUS) {
            if (v.type != V_INT) { wl_error(node->line, "negation requires integer"); return val_nil(); }
            return val_int(-v.ival);
        }
        if (node->op == T_NOT) return val_bool(!val_truthy(v));
        return val_nil();
    }

    case N_IF: {
        Val cond = eval(node->left, env);
        if (val_truthy(cond)) return eval_block(node->right, env);
        /* elif branches */
        for (int i = 0; i + 1 < node->nchildren; i += 2) {
            Val ec = eval(node->children[i], env);
            if (val_truthy(ec)) return eval_block(node->children[i + 1], env);
        }
        if (node->extra) return eval_block(node->extra, env);
        return val_nil();
    }

    case N_WHILE: {
        Val result = val_nil();
        int limit = 1000000; /* infinite loop protection */
        while (val_truthy(eval(node->left, env)) && !wl_ctx.had_error && !wl_ctx.returning && --limit > 0) {
            result = eval_block(node->right, env);
        }
        return result;
    }

    case N_FOR: {
        /* for VAR in ITER: ... end */
        Val iter = eval(node->left, env);
        Val result = val_nil();
        if (iter.type == V_INT) {
            /* range-like: iterate 0..n-1 */
            int64_t upper = iter.ival;
            for (int64_t i = 0; i < upper && !wl_ctx.had_error && !wl_ctx.returning; i++) {
                env_set(env, node->name, val_int(i));
                result = eval_block(node->right, env);
            }
        }
        return result;
    }

    case N_BLOCK:
        return eval_block(node, env);

    case N_FNDEF:
        env_set(env, node->name, val_fn(node, env));
        return val_nil();

    case N_CALL: {
        /* Check if callee is an identifier for built-in check */
        char fname[WL_MAX_NAME] = {0};
        if (node->left && node->left->type == N_VAR) {
            wl_strcpy(fname, node->left->name);
        }

        /* Evaluate arguments */
        Val args[WL_MAX_PARAMS];
        int nargs = node->nchildren;
        if (nargs > WL_MAX_PARAMS) nargs = WL_MAX_PARAMS;
        for (int i = 0; i < nargs; i++) {
            args[i] = eval(node->children[i], env);
        }

        /* Try built-in first */
        Val builtin_result;
        if (fname[0] && call_builtin(fname, args, nargs, &builtin_result)) {
            return builtin_result;
        }

        /* User-defined function */
        Val fn_val = eval(node->left, env);
        if (fn_val.type != V_FN || !fn_val.fn_node) {
            wl_error(node->line, "not a function");
            return val_nil();
        }

        Node *fndef = fn_val.fn_node;
        Env *fn_env = env_new((Env *)fn_val.fn_env);

        /* Bind parameters */
        for (int i = 0; i < fndef->nparams && i < nargs; i++) {
            env_set_local(fn_env, fndef->params[i], args[i]);
        }

        /* Execute function body */
        wl_ctx.returning = false;
        Val result = eval_block(fndef->right, fn_env);
        if (wl_ctx.returning) {
            result = wl_ctx.ret_val;
            wl_ctx.returning = false;
        }
        return result;
    }

    case N_RETURN:
        wl_ctx.ret_val = node->left ? eval(node->left, env) : val_nil();
        wl_ctx.returning = true;
        return wl_ctx.ret_val;

    case N_EXPR_STMT:
        return eval(node->left, env);

    case N_ARRAY:
    case N_INDEX:
        /* Minimal support — index not yet fully implemented */
        return val_nil();
    }

    return val_nil();
}

/* ============================================================
 * Public API
 * ============================================================ */

int wlang_run(const char *code) {
    /* Initialize context */
    memset(&wl_ctx, 0, sizeof(WLCtx));
    wl_ctx.global = env_new(NULL);

    /* Lex and parse */
    Parser parser;
    memset(&parser, 0, sizeof(Parser));
    lex_init(&parser.lex, code);
    parser.cur = parser_next(&parser);

    Node *program = parse_program(&parser);

    if (wl_ctx.had_error) {
        wlang_print_output(wl_ctx.error);
        wlang_print_output("\n");
        wl_cleanup();
        return WLANG_ERROR;
    }

    /* Execute */
    eval_block(program, wl_ctx.global);

    if (wl_ctx.had_error) {
        wlang_print_output(wl_ctx.error);
        wlang_print_output("\n");
        wl_cleanup();
        return WLANG_ERROR;
    }

    wl_cleanup();
    return WLANG_OK;
}

void wlang_browser_init(void) {
    wl_persistent_mode = true;
    wlang_run_file("/browser.wyn");
    if (!wl_ctx.had_error) {
        wl_browser_env = wl_ctx.global;
    }
    wl_persistent_mode = false;
}

void wlang_call_on_draw(void) {
    if (!wl_browser_env) return;
    Val fn_val;
    if (env_get(wl_browser_env, "on_draw", &fn_val) && fn_val.type == V_FN) {
        memset(&wl_ctx, 0, sizeof(WLCtx));
        Node *fndef = fn_val.fn_node;
        Env *fn_env = env_new((Env *)fn_val.fn_env);
        eval_block(fndef->right, fn_env);
        wl_cleanup();
    }
}

void wlang_call_on_key(uint8_t scancode, char ascii) {
    if (!wl_browser_env) return;
    Val fn_val;
    if (env_get(wl_browser_env, "on_key", &fn_val) && fn_val.type == V_FN) {
        memset(&wl_ctx, 0, sizeof(WLCtx));
        Node *fndef = fn_val.fn_node;
        Env *fn_env = env_new((Env *)fn_val.fn_env);
        if (fndef->nparams >= 1) env_set_local(fn_env, fndef->params[0], val_int(scancode));
        if (fndef->nparams >= 2) {
            char abuf[2] = {ascii, '\0'};
            env_set_local(fn_env, fndef->params[1], val_str(abuf));
        }
        eval_block(fndef->right, fn_env);
        wl_cleanup();
    }
}

void wlang_call_on_mouse(int32_t mx, int32_t my, uint8_t buttons) {
    if (!wl_browser_env) return;
    Val fn_val;
    if (env_get(wl_browser_env, "on_mouse", &fn_val) && fn_val.type == V_FN) {
        memset(&wl_ctx, 0, sizeof(WLCtx));
        Node *fndef = fn_val.fn_node;
        Env *fn_env = env_new((Env *)fn_val.fn_env);
        if (fndef->nparams >= 1) env_set_local(fn_env, fndef->params[0], val_int(mx));
        if (fndef->nparams >= 2) env_set_local(fn_env, fndef->params[1], val_int(my));
        if (fndef->nparams >= 3) env_set_local(fn_env, fndef->params[2], val_int(buttons));
        eval_block(fndef->right, fn_env);
        wl_cleanup();
    }
}

int wlang_run_file(const char *path) {
    VfsFile *file = vfs_open(path);
    if (!file) {
        wlang_print_output("WynLang: cannot open file: ");
        wlang_print_output(path);
        wlang_print_output("\n");
        return WLANG_ERROR;
    }

    #define WL_FILE_BUF_SIZE 65536
    char *buf = (char *)kmalloc(WL_FILE_BUF_SIZE);
    if (!buf) {
        wlang_print_output("WynLang: out of memory for file read\n");
        vfs_close(file);
        return WLANG_ERROR;
    }

    int total = 0;
    int rd;
    while ((rd = vfs_read(file, buf + total, (uint32_t)(WL_FILE_BUF_SIZE - 1 - total))) > 0) {
        total += rd;
        if (total >= (WL_FILE_BUF_SIZE - 1)) break;
    }
    buf[total] = '\0';
    vfs_close(file);

    int res = wlang_run(buf);
    kfree(buf);
    return res;
}
