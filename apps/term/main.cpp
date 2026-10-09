// WynlandOS - Terminal: a terminal emulator for Zerp 2.0.
//
// A Zerp client (the qwynlandfb QPA plugin compiled in, like Files): a
// PTY with the user's shell on it -- fish unless $SHELL says otherwise --
// and libvterm to understand what programs print (colours, cursor
// movement, the alternate screen, bracketed paste, the queries a shell
// asks a terminal...). The window is drawn with QPainter: a grid of
// cells in JetBrains Mono over a see-through background that Zerp lays
// on its frosted glass.
//
// Keys: Ctrl+Shift+= / - font size; the wheel scrolls back (2000 lines).
//
// Copyright (C) 2026 Red_Artem39. GPL-2.0-only.

#include <QtCore/QSocketNotifier>
#include <QtCore/QTimer>
#include <QtCore/QtPlugin>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetricsF>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QRasterWindow>
#include <QtGui/QSurfaceFormat>
#include <QtGui/QWheelEvent>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern "C" {
#include <vterm.h>
}

#include "../../qt_qpa/qwynlandfb_zerpargs.h"

Q_IMPORT_PLUGIN(QWynlandFbIntegrationPlugin)

// the look: a dark, slightly see-through background and a 16-colour
// palette in the same spirit (Catppuccin Mocha)
static const QColor kBg(17, 17, 27, 222);
static const QColor kFg(205, 214, 244);
static const QColor kCursor(245, 224, 220);
static const uint8_t kPalette[16][3] = {
    { 69, 71, 90 }, { 243, 139, 168 }, { 166, 227, 161 }, { 249, 226, 175 },
    { 137, 180, 250 }, { 245, 194, 231 }, { 148, 226, 213 }, { 186, 194, 222 },
    { 88, 91, 112 }, { 243, 139, 168 }, { 166, 227, 161 }, { 249, 226, 175 },
    { 137, 180, 250 }, { 245, 194, 231 }, { 148, 226, 213 }, { 166, 173, 200 },
};
static const int kPad = 10;
static const int kScrollback = 2000;

class Terminal : public QRasterWindow
{
public:
    Terminal()
    {
        QSurfaceFormat f = format();
        f.setAlphaBufferSize(8);          // see-through: Zerp's glass behind it
        setFormat(f);
        setTitle(QStringLiteral("Terminal"));
        m_font = QFont(QStringLiteral("JetBrains Mono"));
        if (!QFontInfo(m_font).fixedPitch()) m_font = QFont(QStringLiteral("DejaVu Sans Mono"));
        m_font.setPixelSize(15);
        m_font.setStyleHint(QFont::Monospace);
        measure();

        m_vt = vterm_new(24, 80);
        vterm_set_utf8(m_vt, 1);
        vterm_output_set_callback(m_vt, &Terminal::onOutput, this);
        m_screen = vterm_obtain_screen(m_vt);
        static VTermScreenCallbacks cb;
        memset(&cb, 0, sizeof cb);
        cb.damage = [](VTermRect, void *u) { static_cast<Terminal *>(u)->dirty(); return 1; };
        cb.movecursor = [](VTermPos p, VTermPos, int vis, void *u) {
            auto *t = static_cast<Terminal *>(u);
            t->m_cursor = p;
            t->m_cursorVisible = vis;
            t->dirty();
            return 1;
        };
        cb.settermprop = [](VTermProp prop, VTermValue *v, void *u) { return static_cast<Terminal *>(u)->termProp(prop, v); };
        cb.sb_pushline = [](int cols, const VTermScreenCell *cells, void *u) {
            auto *t = static_cast<Terminal *>(u);
            t->m_scroll.emplace_back(cells, cells + cols);
            if (t->m_scroll.size() > size_t(kScrollback)) t->m_scroll.pop_front();
            return 1;
        };
        cb.sb_popline = [](int cols, VTermScreenCell *cells, void *u) {
            auto *t = static_cast<Terminal *>(u);
            if (t->m_scroll.empty()) return 0;
            const auto &line = t->m_scroll.back();
            for (int i = 0; i < cols; i++) {
                if (i < int(line.size())) cells[i] = line[i];
                else { memset(&cells[i], 0, sizeof cells[i]); cells[i].width = 1; }
            }
            t->m_scroll.pop_back();
            return 1;
        };
        cb.sb_clear = [](void *u) { static_cast<Terminal *>(u)->m_scroll.clear(); return 1; };
        vterm_screen_set_callbacks(m_screen, &cb, this);
        vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
        vterm_screen_enable_altscreen(m_screen, 1);
        VTermState *st = vterm_obtain_state(m_vt);
        VTermColor fg, bg;
        vterm_color_rgb(&fg, kFg.red(), kFg.green(), kFg.blue());
        vterm_color_rgb(&bg, kBg.red(), kBg.green(), kBg.blue());
        vterm_state_set_default_colors(st, &fg, &bg);
        for (int i = 0; i < 16; i++) {
            VTermColor c;
            vterm_color_rgb(&c, kPalette[i][0], kPalette[i][1], kPalette[i][2]);
            vterm_state_set_palette_color(st, i, &c);
        }
        vterm_screen_reset(m_screen, 1);

        m_repaint.setSingleShot(true);
        m_repaint.setInterval(8);          // batch a burst of output into one frame
        QObject::connect(&m_repaint, &QTimer::timeout, this, [this] { update(); });
    }

    ~Terminal() override
    {
        if (m_pid > 0) kill(m_pid, SIGHUP);
        vterm_free(m_vt);
    }

    bool start()
    {
        m_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (m_master < 0 || grantpt(m_master) < 0 || unlockpt(m_master) < 0) return false;
        const char *slave = ptsname(m_master);
        if (!slave) return false;
        const char *shell = getenv("SHELL");
        if (!shell || access(shell, X_OK)) shell = "/usr/bin/fish";
        if (access(shell, X_OK)) shell = "/bin/sh";
        m_pid = fork();
        if (m_pid < 0) return false;
        if (m_pid == 0) {
            setsid();
            int s = open(slave, O_RDWR);
            if (s < 0) _exit(126);
            ioctl(s, TIOCSCTTY, 0);
            dup2(s, 0); dup2(s, 1); dup2(s, 2);
            if (s > 2) ::close(s);
            setenv("TERM", "xterm-256color", 1);
            setenv("COLORTERM", "truecolor", 1);
            const char *home = getenv("HOME");
            if (home && ::chdir(home) != 0) { }
            const char *base = strrchr(shell, '/');
            // a login shell: "-fish"
            char argv0[64];
            snprintf(argv0, sizeof argv0, "-%s", base ? base + 1 : shell);
            execl(shell, argv0, (char *)nullptr);
            _exit(127);
        }
        fcntl(m_master, F_SETFL, O_NONBLOCK);
        m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
        QObject::connect(m_notifier, &QSocketNotifier::activated, this, [this] { readPty(); });
        resizeGrid();
        return true;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(0, 0, width(), height()), kBg);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setFont(m_font);
        const int top = qMin(m_offset, int(m_scroll.size()));   // lines of scrollback shown
        for (int row = 0; row < m_rows; row++) {
            const int src = row - top;                     // < 0: from the scrollback
            for (int col = 0; col < m_cols; ) {
                VTermScreenCell cell;
                if (src < 0) {
                    const auto &line = m_scroll[m_scroll.size() + size_t(src)];
                    if (col < int(line.size())) cell = line[size_t(col)];
                    else { memset(&cell, 0, sizeof cell); cell.width = 1; cell.fg.type = VTERM_COLOR_DEFAULT_FG; cell.bg.type = VTERM_COLOR_DEFAULT_BG; }
                } else {
                    vterm_screen_get_cell(m_screen, VTermPos{ src, col }, &cell);
                }
                const int w = cell.width > 0 ? cell.width : 1;
                drawCell(p, row, col, cell, src >= 0 && m_cursorVisible && m_offset == 0 &&
                         m_cursor.row == src && m_cursor.col == col);
                col += w;
            }
        }
    }

    void resizeEvent(QResizeEvent *) override { resizeGrid(); }

    void keyPressEvent(QKeyEvent *e) override
    {
        const Qt::KeyboardModifiers qm = e->modifiers();
        int mod = VTERM_MOD_NONE;
        if (qm & Qt::ShiftModifier) mod |= VTERM_MOD_SHIFT;
        if (qm & Qt::AltModifier) mod |= VTERM_MOD_ALT;
        if (qm & Qt::ControlModifier) mod |= VTERM_MOD_CTRL;
        if ((qm & Qt::ControlModifier) && (qm & Qt::ShiftModifier)) {
            if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) { zoom(1); return; }
            if (e->key() == Qt::Key_Minus || e->key() == Qt::Key_Underscore) { zoom(-1); return; }
        }
        m_offset = 0;                                       // typing goes back to the bottom
        VTermKey k = VTERM_KEY_NONE;
        switch (e->key()) {
        case Qt::Key_Return: case Qt::Key_Enter: k = VTERM_KEY_ENTER; break;
        case Qt::Key_Tab: case Qt::Key_Backtab: k = VTERM_KEY_TAB; break;
        case Qt::Key_Backspace: k = VTERM_KEY_BACKSPACE; break;
        case Qt::Key_Escape: k = VTERM_KEY_ESCAPE; break;
        case Qt::Key_Up: k = VTERM_KEY_UP; break;
        case Qt::Key_Down: k = VTERM_KEY_DOWN; break;
        case Qt::Key_Left: k = VTERM_KEY_LEFT; break;
        case Qt::Key_Right: k = VTERM_KEY_RIGHT; break;
        case Qt::Key_Insert: k = VTERM_KEY_INS; break;
        case Qt::Key_Delete: k = VTERM_KEY_DEL; break;
        case Qt::Key_Home: k = VTERM_KEY_HOME; break;
        case Qt::Key_End: k = VTERM_KEY_END; break;
        case Qt::Key_PageUp: k = VTERM_KEY_PAGEUP; break;
        case Qt::Key_PageDown: k = VTERM_KEY_PAGEDOWN; break;
        default:
            if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F12) k = VTermKey(VTERM_KEY_FUNCTION(e->key() - Qt::Key_F1 + 1));
        }
        if (k != VTERM_KEY_NONE) {
            vterm_keyboard_key(m_vt, k, VTermModifier(mod));
        } else if ((qm & Qt::ControlModifier) && e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) {
            vterm_keyboard_unichar(m_vt, uint32_t('a' + (e->key() - Qt::Key_A)), VTermModifier(mod & ~VTERM_MOD_SHIFT));
        } else if (!e->text().isEmpty()) {
            for (const uint c : e->text().toUcs4()) {
                if (c < 32 && !(qm & Qt::ControlModifier)) continue;
                // the text already carries Shift
                vterm_keyboard_unichar(m_vt, c, VTermModifier(mod & ~VTERM_MOD_SHIFT));
            }
        }
        flushOutput();
    }

    void wheelEvent(QWheelEvent *e) override
    {
        const int lines = e->angleDelta().y() / 40;
        m_offset = qBound(0, m_offset + lines, int(m_scroll.size()));
        update();
    }

private:
    static void onOutput(const char *s, size_t len, void *user)
    {
        static_cast<Terminal *>(user)->m_out.append(s, qsizetype(len));
    }

    void flushOutput()
    {
        const char *p = m_out.constData();
        qsizetype left = m_out.size();
        while (left > 0) {
            ssize_t w = ::write(m_master, p, size_t(left));
            if (w < 0 && errno == EAGAIN) { usleep(1000); continue; }
            if (w <= 0) break;
            p += w;
            left -= w;
        }
        m_out.clear();
    }

    void readPty()
    {
        char buf[16384];
        for (;;) {
            ssize_t n = ::read(m_master, buf, sizeof buf);
            if (n > 0) { vterm_input_write(m_vt, buf, size_t(n)); continue; }
            if (n < 0 && errno == EAGAIN) break;
            // the shell is gone (EIO / EOF): so is the window
            m_notifier->setEnabled(false);
            int st;
            waitpid(m_pid, &st, WNOHANG);
            m_pid = -1;
            QCoreApplication::quit();
            return;
        }
        vterm_screen_flush_damage(m_screen);
        flushOutput();                                      // answers to queries (DA, cursor position)
    }

    int termProp(VTermProp prop, VTermValue *v)
    {
        if (prop == VTERM_PROP_TITLE) {
            if (v->string.initial) m_title.clear();
            m_title.append(v->string.str, qsizetype(v->string.len));
            if (v->string.final) setTitle(QString::fromUtf8(m_title));
        } else if (prop == VTERM_PROP_CURSORVISIBLE) {
            m_cursorVisible = v->boolean;
            dirty();
        }
        return 1;
    }

    void dirty() { if (!m_repaint.isActive()) m_repaint.start(); }

    void measure()
    {
        QFontMetricsF fm(m_font);
        m_cw = fm.horizontalAdvance(QLatin1Char('M'));
        m_ch = qCeil(fm.height());
        m_ascent = fm.ascent();
    }

    void zoom(int d)
    {
        m_font.setPixelSize(qBound(9, m_font.pixelSize() + d, 32));
        measure();
        resizeGrid();
    }

    void resizeGrid()
    {
        const int cols = qMax(10, int((width() - 2 * kPad) / m_cw));
        const int rows = qMax(3, int((height() - 2 * kPad) / m_ch));
        if (cols == m_cols && rows == m_rows) { update(); return; }
        m_cols = cols;
        m_rows = rows;
        vterm_set_size(m_vt, rows, cols);
        if (m_master >= 0) {
            struct winsize ws = { uint16_t(rows), uint16_t(cols), uint16_t(width()), uint16_t(height()) };
            ioctl(m_master, TIOCSWINSZ, &ws);
        }
        update();
    }

    QColor color(VTermColor c) const
    {
        vterm_screen_convert_color_to_rgb(m_screen, &c);
        return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
    }

    void drawCell(QPainter &p, int row, int col, const VTermScreenCell &cell, bool cursor)
    {
        const QRectF r(kPad + col * m_cw, kPad + row * m_ch, m_cw * (cell.width > 1 ? cell.width : 1), m_ch);
        const bool defBg = VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
        QColor fg = VTERM_COLOR_IS_DEFAULT_FG(&cell.fg) ? kFg : color(cell.fg);
        QColor bg = defBg ? QColor() : color(cell.bg);
        if (cell.attrs.reverse) { QColor t = bg.isValid() ? bg : QColor(kBg.red(), kBg.green(), kBg.blue()); bg = fg; fg = t; }
        if (cursor) { bg = kCursor; fg = QColor(kBg.red(), kBg.green(), kBg.blue()); }
        if (bg.isValid()) p.fillRect(r, bg);
        if (!cell.chars[0] || cell.chars[0] == ' ') return;
        QString s;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) s += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
        QFont f = m_font;
        if (cell.attrs.bold) f.setBold(true);
        if (cell.attrs.italic) f.setItalic(true);
        if (cell.attrs.underline) f.setUnderline(true);
        if (cell.attrs.strike) f.setStrikeOut(true);
        if (f != p.font()) p.setFont(f);
        p.setPen(fg);
        p.drawText(QPointF(r.x(), r.y() + m_ascent), s);
        if (f != m_font) p.setFont(m_font);
    }

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    QFont m_font;
    qreal m_cw = 8, m_ascent = 12;
    int m_ch = 16;
    int m_rows = 0, m_cols = 0;
    VTermPos m_cursor { 0, 0 };
    bool m_cursorVisible = true;
    std::deque<std::vector<VTermScreenCell>> m_scroll;
    int m_offset = 0;                                       // lines scrolled back
    int m_master = -1;
    pid_t m_pid = -1;
    QSocketNotifier *m_notifier = nullptr;
    QByteArray m_out, m_title;
    QTimer m_repaint;
};

int main(int argc, char **argv)
{
    zerp_qpa_capture_args(argc, argv);
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    if (!qEnvironmentVariableIsSet("FONTCONFIG_FILE"))
        qputenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf");
    signal(SIGPIPE, SIG_IGN);
    QGuiApplication app(argc, argv);
    Terminal t;
    t.show();
    if (!t.start()) {
        fprintf(stderr, "[term] FAIL: no pty or no shell\n");
        return 1;
    }
    fprintf(stderr, "[term] up: %s\n", getenv("SHELL") ? getenv("SHELL") : "/usr/bin/fish");
    return app.exec();
}
