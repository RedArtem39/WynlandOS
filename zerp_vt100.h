/*
 * WynlandOS / Zerp - minimal VT100/ECMA-48 interpreter for zerp_term.c's
 * PTY-active mode (Phase 18h). Implements exactly the subset ncurses'
 * --with-fallbacks=vt100 build emits (confirmed empirically via
 * test_ncurses.c's smoke test, not guessed): cursor positioning (CUP),
 * relative cursor movement, erase-display/erase-line, plain CR/LF/BS,
 * and graceful pass-through (recognize-and-ignore) of sequences this v1
 * doesn't act on (SGR colors, DECSTBM scroll region, charset selection
 * via ESC ( / ESC ), SI/SO, DEC private modes like ESC[?7h) -- ignoring
 * them cleanly is enough for a monochrome, non-scrolling-aware v1 since
 * nano repaints its screen via explicit cursor positioning + erase
 * rather than relying on terminal-side scrolling.
 * Header-only, `static` -- same convention as zerp_client.h.
 */
#ifndef ZERP_VT100_H
#define ZERP_VT100_H

#define VT_MAX_ROWS 64
#define VT_MAX_COLS 200

static char g_vt_cell[VT_MAX_ROWS][VT_MAX_COLS];
static int  g_vt_rows = 24, g_vt_cols = 80;
static int  g_vt_cx = 0, g_vt_cy = 0;

typedef enum { VT_NORMAL, VT_ESC, VT_ESC_CHARSET, VT_CSI } VtState;
static VtState g_vt_state = VT_NORMAL;
static int g_vt_params[8];
static int g_vt_param_count;

static void vt_reset(int rows, int cols) {
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;
    g_vt_rows = rows;
    g_vt_cols = cols;
    g_vt_cx = 0;
    g_vt_cy = 0;
    g_vt_state = VT_NORMAL;
    for (int r = 0; r < VT_MAX_ROWS; r++)
        for (int c = 0; c < VT_MAX_COLS; c++)
            g_vt_cell[r][c] = ' ';
}

static void vt_clear_all(void) {
    for (int r = 0; r < g_vt_rows; r++)
        for (int c = 0; c < g_vt_cols; c++)
            g_vt_cell[r][c] = ' ';
}

static void vt_clear_line(int row) {
    if (row < 0 || row >= g_vt_rows) return;
    for (int c = 0; c < g_vt_cols; c++) g_vt_cell[row][c] = ' ';
}

/* No real scroll-up model in v1 (nano repaints explicitly rather than
   relying on terminal scrolling) -- clamp at the last row instead. */
static void vt_line_feed(void) {
    g_vt_cy++;
    if (g_vt_cy >= g_vt_rows) g_vt_cy = g_vt_rows - 1;
}

static void vt_dispatch_csi(char final, int private_mode) {
    int p0 = g_vt_param_count > 0 ? g_vt_params[0] : 0;
    int p1 = g_vt_param_count > 1 ? g_vt_params[1] : 0;
    (void)private_mode;

    switch (final) {
        case 'H': case 'f': /* CUP */
            g_vt_cy = (p0 > 0 ? p0 - 1 : 0);
            g_vt_cx = (p1 > 0 ? p1 - 1 : 0);
            if (g_vt_cy >= g_vt_rows) g_vt_cy = g_vt_rows - 1;
            if (g_vt_cy < 0) g_vt_cy = 0;
            if (g_vt_cx >= g_vt_cols) g_vt_cx = g_vt_cols - 1;
            if (g_vt_cx < 0) g_vt_cx = 0;
            break;
        case 'A': g_vt_cy -= (p0 > 0 ? p0 : 1); if (g_vt_cy < 0) g_vt_cy = 0; break;
        case 'B': g_vt_cy += (p0 > 0 ? p0 : 1); if (g_vt_cy >= g_vt_rows) g_vt_cy = g_vt_rows - 1; break;
        case 'C': g_vt_cx += (p0 > 0 ? p0 : 1); if (g_vt_cx >= g_vt_cols) g_vt_cx = g_vt_cols - 1; break;
        case 'D': g_vt_cx -= (p0 > 0 ? p0 : 1); if (g_vt_cx < 0) g_vt_cx = 0; break;
        case 'J': /* ED -- v1 simplification: any param clears the whole screen */
            vt_clear_all();
            break;
        case 'K': /* EL -- v1 simplification: clears the whole current line regardless of param */
            vt_clear_line(g_vt_cy);
            break;
        default:
            /* 'm' (SGR/colors), 'r' (DECSTBM scroll region), 'h'/'l'
               (mode set/reset incl. DEC private modes) -- recognized,
               deliberately ignored (monochrome, non-scrolling v1). */
            break;
    }
}

static void vt_feed_byte(uint8_t c) {
    switch (g_vt_state) {
        case VT_NORMAL:
            if (c == 0x1B) { g_vt_state = VT_ESC; return; }
            if (c == '\r') { g_vt_cx = 0; return; }
            if (c == '\n') { vt_line_feed(); return; }
            if (c == '\b') { if (g_vt_cx > 0) g_vt_cx--; return; }
            if (c < 0x20) return; /* BEL, SI, SO, other control bytes -- ignore */
            if (g_vt_cy >= 0 && g_vt_cy < g_vt_rows && g_vt_cx >= 0 && g_vt_cx < g_vt_cols)
                g_vt_cell[g_vt_cy][g_vt_cx] = (char)c;
            g_vt_cx++;
            if (g_vt_cx >= g_vt_cols) { g_vt_cx = 0; vt_line_feed(); }
            return;

        case VT_ESC:
            if (c == '[') {
                g_vt_state = VT_CSI;
                g_vt_param_count = 0;
                g_vt_params[0] = 0;
                return;
            }
            if (c == '(' || c == ')') { g_vt_state = VT_ESC_CHARSET; return; }
            /* Other single-byte ESC sequences (=, >, D, M, c, ...) --
               consumed and ignored, one byte total after ESC. */
            g_vt_state = VT_NORMAL;
            return;

        case VT_ESC_CHARSET:
            /* Consumes the charset-designator byte (B, 0, U, ...) after
               ESC( or ESC) -- ignored, charset switching not modeled. */
            g_vt_state = VT_NORMAL;
            return;

        case VT_CSI: {
            static int private_mode = 0;
            if (c == '?') { private_mode = 1; return; }
            if (c >= '0' && c <= '9') {
                if (g_vt_param_count < 8)
                    g_vt_params[g_vt_param_count] = g_vt_params[g_vt_param_count] * 10 + (c - '0');
                return;
            }
            if (c == ';') {
                if (g_vt_param_count < 7) g_vt_param_count++;
                g_vt_params[g_vt_param_count] = 0;
                return;
            }
            /* Any other byte terminates the CSI sequence. */
            g_vt_param_count++;
            vt_dispatch_csi((char)c, private_mode);
            private_mode = 0;
            g_vt_state = VT_NORMAL;
            return;
        }
    }
}

#endif /* ZERP_VT100_H */
