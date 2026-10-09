// WynlandOS - Zerp 2.0: the desktop as a Qt Quick scene on the GPU.
//
// Qt's eglfs platform (KMS/GBM backend) draws straight to /dev/dri/card0
// through Mesa/virgl -- the same DRM path tests/kmstest.c exercises -- so
// QML shaders (rounded corners, shadows, blur) run on the host GPU.
// Input does not go through evdev/libinput (this OS has neither): the
// kernel's own pointer stream (syscall 412) and raw keyboard scancodes
// (/dev/input/kbd) are polled here and handed to the window as Qt events.
// The pointer image itself is the virtio-gpu cursor plane, which the
// kernel moves from the mouse IRQ.
//
// Debug aid: with "snapshot" in /etc/wynland/boot.cfg the scene is grabbed
// after a while and written to the serial log as base64 PNG
// ("ZSNAP ..." lines), since a GL scanout can't be screendumped.

#include <QtCore/QBuffer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickWindow>
#include <QtGui/QScreen>
#include <QtQml/qqml.h>

#include "zerp2.h"
#include "greeter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

struct ZMouseEvent { int32_t x, y; uint32_t buttons; };
static long mouse_events(ZMouseEvent *buf, long max) { return syscall(412, buf, max, 0); }

// PS/2 set-1 make codes -> Qt keys / text (US layout)
static const int kKeys[59] = {
    0, Qt::Key_Escape, Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5, Qt::Key_6,
    Qt::Key_7, Qt::Key_8, Qt::Key_9, Qt::Key_0, Qt::Key_Minus, Qt::Key_Equal, Qt::Key_Backspace,
    Qt::Key_Tab, Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R, Qt::Key_T, Qt::Key_Y, Qt::Key_U,
    Qt::Key_I, Qt::Key_O, Qt::Key_P, Qt::Key_BracketLeft, Qt::Key_BracketRight, Qt::Key_Return,
    Qt::Key_Control, Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F, Qt::Key_G, Qt::Key_H, Qt::Key_J,
    Qt::Key_K, Qt::Key_L, Qt::Key_Semicolon, Qt::Key_Apostrophe, Qt::Key_QuoteLeft, Qt::Key_Shift,
    Qt::Key_Backslash, Qt::Key_Z, Qt::Key_X, Qt::Key_C, Qt::Key_V, Qt::Key_B, Qt::Key_N, Qt::Key_M,
    Qt::Key_Comma, Qt::Key_Period, Qt::Key_Slash, Qt::Key_Shift, Qt::Key_Asterisk, Qt::Key_Alt,
    Qt::Key_Space, Qt::Key_CapsLock };
static const char kLower[59] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0 };
static const char kUpper[59] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0 };

class InputPump : public QObject
{
public:
    // greeter: the login screen -- no WM bindings (they start programs)
    InputPump(QQuickWindow *w, ZServer *srv, bool greeter = false) : m_win(w), m_srv(srv), m_greeter(greeter)
    {
        m_kbd = ::open("/dev/input/kbd", O_RDONLY);
        connect(&m_timer, &QTimer::timeout, this, &InputPump::poll);
        m_timer.start(4);
    }

private:
    void poll()
    {
        ZMouseEvent ev[64];
        long n;
        while ((n = mouse_events(ev, 64)) > 0) {
            for (long i = 0; i < n; i++) mouse(ev[i]);
            if (n < 64) break;
        }
        if (m_kbd >= 0) {
            unsigned char kb[64];
            ssize_t k;
            while ((k = ::read(m_kbd, kb, sizeof kb)) > 0)
                for (ssize_t i = 0; i < k; i++) key(kb[i]);
        }
    }

    void mouse(const ZMouseEvent &e)
    {
        const QPointF p(e.x, e.y);
        Qt::MouseButtons now;
        if (e.buttons & 1) now |= Qt::LeftButton;
        if (e.buttons & 2) now |= Qt::RightButton;
        if (e.buttons & 4) now |= Qt::MiddleButton;
        if (const int wheel = (int8_t)((e.buttons >> 8) & 0xFF)) {   // + = down
            QWheelEvent we(p, p, QPoint(), QPoint(0, -wheel * 120), m_buttons, m_mods,
                           Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(m_win, &we);
            return;
        }
        if (p != m_pos || now == m_buttons) {
            m_pos = p;
            QMouseEvent mv(QEvent::MouseMove, p, p, Qt::NoButton, m_buttons, m_mods);
            QCoreApplication::sendEvent(m_win, &mv);
        }
        const Qt::MouseButtons changed = now ^ m_buttons;
        for (Qt::MouseButton b : { Qt::LeftButton, Qt::RightButton, Qt::MiddleButton }) {
            if (!(changed & b)) continue;
            m_buttons ^= b;
            QMouseEvent be((now & b) ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                           p, p, b, m_buttons, m_mods);
            QCoreApplication::sendEvent(m_win, &be);
        }
    }

    // WM bindings (mod = Alt or Super; Super is usually eaten by the host)
    bool binding(bool e0, int code)
    {
        if (e0) {
            switch (code) {
            case 0x4B: m_srv->focusDirection(-1, 0); return true;
            case 0x4D: m_srv->focusDirection(1, 0); return true;
            case 0x48: m_srv->focusDirection(0, -1); return true;
            case 0x50: m_srv->focusDirection(0, 1); return true;
            }
            return false;
        }
        if (code >= 0x02 && code <= 0x0A) {          // 1..9
            if (m_shift) m_srv->moveFocusedTo(code - 1);
            else m_srv->setWorkspace(code - 1);
            return true;
        }
        switch (code) {
        case 0x1C: m_srv->spawn(QStringLiteral("/usr/bin/term")); return true;     // Enter: the terminal (fish)
        case 0x10: m_srv->closeFocused(); return true;                            // Q
        case 0x20: m_srv->spawn(QStringLiteral("/zerp_rofi.elf")); return true;   // D
        case 0x21: m_srv->toggleFullscreen(); return true;                        // F
        case 0x19: m_srv->setShowFps(!m_srv->showFps()); return true;             // P: FPS
        case 0x24: m_srv->focusDirection(-1, 0); return true;                     // H J K L
        case 0x26: m_srv->focusDirection(1, 0); return true;
        case 0x25: m_srv->focusDirection(0, -1); return true;
        case 0x23: m_srv->focusDirection(0, 1); return true;
        }
        return false;
    }

    void key(unsigned char sc)
    {
        if (sc == 0xE0) { m_e0 = true; return; }
        const bool release = sc & 0x80;
        const int code = sc & 0x7F;
        const bool e0 = m_e0;
        m_e0 = false;

        // modifier tracking for the bindings
        if (!e0 && code == 0x38) m_alt = !release;
        if (e0 && code == 0x38) m_altR = !release;
        if (e0 && (code == 0x5B || code == 0x5C)) m_super = !release;
        if (!e0 && (code == 0x2A || code == 0x36)) m_shift = !release;
        const int kid = code + (e0 ? 128 : 0);
        if (release && m_swallow[kid]) { m_swallow[kid] = false; return; }
        if (!m_greeter && !release && (m_alt || m_altR || m_super) && binding(e0, code)) {
            m_swallow[kid] = true;   // its release must not reach the client either
            return;
        }
        // everything else: raw scancodes to the focused client
        if (ZClient *c = m_srv->focused()) { c->sendKey(e0, sc); return; }

        int qk = 0;
        QString text;
        if (e0) {
            switch (code) {
            case 0x48: qk = Qt::Key_Up; break;
            case 0x50: qk = Qt::Key_Down; break;
            case 0x4B: qk = Qt::Key_Left; break;
            case 0x4D: qk = Qt::Key_Right; break;
            case 0x47: qk = Qt::Key_Home; break;
            case 0x4F: qk = Qt::Key_End; break;
            case 0x53: qk = Qt::Key_Delete; break;
            case 0x1D: qk = Qt::Key_Control; break;
            case 0x38: qk = Qt::Key_Alt; break;
            case 0x5B: case 0x5C: qk = Qt::Key_Meta; break;
            default: return;
            }
        } else if (code < 59) {
            qk = kKeys[code];
        }
        if (!qk) return;

        // modifier state
        Qt::KeyboardModifier mod = Qt::NoModifier;
        if (qk == Qt::Key_Shift) mod = Qt::ShiftModifier;
        else if (qk == Qt::Key_Control) mod = Qt::ControlModifier;
        else if (qk == Qt::Key_Alt) mod = Qt::AltModifier;
        else if (qk == Qt::Key_Meta) mod = Qt::MetaModifier;
        if (mod != Qt::NoModifier) {
            if (release) m_mods &= ~mod; else m_mods |= mod;
        }
        if (qk == Qt::Key_CapsLock && !release) m_caps = !m_caps;

        if (!e0 && code < 59) {
            const bool shift = m_mods & Qt::ShiftModifier;
            char c = shift ? kUpper[code] : kLower[code];
            if (m_caps && c >= 'a' && c <= 'z') c = char(c - 32);
            else if (m_caps && c >= 'A' && c <= 'Z') c = char(c + 32);
            if (c && !(m_mods & Qt::ControlModifier)) text = QString(QChar(c));
        }
        QKeyEvent ke(release ? QEvent::KeyRelease : QEvent::KeyPress, qk, m_mods, text);
        QCoreApplication::sendEvent(m_win, &ke);
    }

    QQuickWindow *m_win;
    ZServer *m_srv;
    bool m_greeter = false;
    bool m_alt = false, m_altR = false, m_super = false, m_shift = false;
    bool m_swallow[256] = {};
    QTimer m_timer;
    int m_kbd = -1;
    QPointF m_pos;
    Qt::MouseButtons m_buttons;
    Qt::KeyboardModifiers m_mods;
    bool m_e0 = false, m_caps = false;
};

static QByteArray bootCfg()
{
    QFile f(QStringLiteral("/etc/wynland/boot.cfg"));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "eglfs");
    qputenv("QT_QPA_EGLFS_INTEGRATION", "eglfs_kms");
    qputenv("QT_QPA_EGLFS_KMS_CONFIG", "/usr/share/zerp2/kms.json");
    qputenv("QT_QPA_EGLFS_KMS_ATOMIC", "0");
    qputenv("QT_QPA_EGLFS_DISABLE_INPUT", "1");   // no evdev here -- InputPump instead
    qputenv("QT_QPA_EGLFS_HIDECURSOR", "1");      // the GPU cursor plane is the pointer
    qputenv("QSG_RHI_BACKEND", "opengl");
    /* scene graph diagnostics on request only (BOOT_EXTRA=qsginfo): every
       Qt client inherits it, and it all lands on the synchronous serial
       console */
    if (bootCfg().contains("qsginfo")) qputenv("QSG_INFO", "1");
    if (!qEnvironmentVariableIsSet("HOME")) qputenv("HOME", "/tmp");
    if (!qEnvironmentVariableIsSet("XDG_RUNTIME_DIR")) qputenv("XDG_RUNTIME_DIR", "/tmp");

    // zerp2 --greeter REQ_FD REP_FD [--first]: the login screen (wynlogin)
    const bool greeterMode = argc >= 4 && !strcmp(argv[1], "--greeter");
    const int greeterReq = greeterMode ? atoi(argv[2]) : -1;
    const int greeterRep = greeterMode ? atoi(argv[3]) : -1;
    const bool firstBoot = greeterMode && argc >= 5 && !strcmp(argv[4], "--first");

    QGuiApplication app(argc, argv);
    qmlRegisterType<ZSurface>("Zerp", 1, 0, "ZSurface");
    qmlRegisterUncreatableType<ZClient>("Zerp", 1, 0, "ZClient", QStringLiteral("made by the server"));
    const QSize scr = app.primaryScreen() ? app.primaryScreen()->size() : QSize(1920, 1080);
    ZServer server(uint32_t(scr.width()) * uint32_t(scr.height()) * 4u);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("zerp"), &server);
    engine.addImportPath(QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &ws) {
        for (const QQmlError &w : ws) fprintf(stderr, "[zerp2] %s\n", qPrintable(w.toString()));
    });
    engine.rootContext()->setContextProperty(QStringLiteral("wallpaperUrl"),
        QFile::exists(QStringLiteral("/wall.png")) ? QStringLiteral("file:///wall.png") : QString());
    Greeter *greeter = greeterMode ? new Greeter(greeterReq, greeterRep, firstBoot, &app) : nullptr;
    engine.rootContext()->setContextProperty(QStringLiteral("greeter"), greeter);
    engine.rootContext()->setContextProperty(QStringLiteral("userName"),
        qEnvironmentVariableIsSet("USER") ? qEnvironmentVariable("USER") : QStringLiteral("user"));
    const QString qml = greeterMode ? QStringLiteral("Greeter.qml") : QStringLiteral("Shell.qml");
    engine.load(QUrl::fromLocalFile(QStringLiteral("/usr/share/zerp2/qml/") + qml));
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "[zerp2] FAIL: %s did not load\n", qPrintable(qml));
        return 1;
    }
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!win) { fprintf(stderr, "[zerp2] FAIL: root is not a Window\n"); return 1; }
    InputPump pump(win, &server, greeterMode);
    // debugging: boot.cfg "logoutat=N" -- the session logs out after N s
    // (the login screen with an account, for its picture)
    if (!greeterMode) {
        const QByteArray cfg = bootCfg();
        const int at = cfg.indexOf("logoutat=");
        if (at >= 0) {
            const int secs = cfg.mid(at + 9).split('\n').first().trimmed().toInt();
            if (secs > 0) QTimer::singleShot(secs * 1000, &app, [] { QCoreApplication::quit(); });
        }
    }
    // debugging: boot.cfg "greeterdemo=NAME:PASSWORD" -- the login screen
    // fills itself in after a while (creates the account on a first boot),
    // so the whole way to a session can be watched without a keyboard
    if (greeter) {
        const QByteArray cfg = bootCfg();
        const int at = cfg.indexOf("greeterdemo=");
        if (at >= 0) {
            const QList<QByteArray> np = cfg.mid(at + 12).split('\n').first().trimmed().split(':');
            if (np.size() == 2) {
                const int when = cfg.contains("snapat=") ? 30000 : 8000;   // after the screen's own picture
                QTimer::singleShot(when, greeter, [greeter, np] {
                    if (greeter->firstBoot()) greeter->createAccount(QString::fromUtf8(np[0]), QString(), QString::fromUtf8(np[1]));
                    else greeter->login(QString::fromUtf8(np[0]), QString::fromUtf8(np[1]));
                });
            }
        }
    }
    if (!greeterMode) server.spawn(QStringLiteral("/usr/bin/term"));   // a terminal to start with
    QObject::connect(win, &QQuickWindow::frameSwapped, win, [] {
        static int frames = 0;
        if (++frames == 1 || frames % 600 == 0) fprintf(stderr, "[zerp2] frame %d\n", frames);
    });
    fprintf(stderr, greeterMode ? "[zerp2] login screen up%s\n" : "[zerp2] shell up%s\n",
            firstBoot ? " (first boot)" : "");

    if (bootCfg().contains("snapshot")) {
        // "snapweb": the browser full screen in the picture (and nothing
        // else); otherwise something to tile
        const bool web = bootCfg().contains("snapweb");
        if (greeterMode) {
            // the login screen alone in the picture
        } else if (web) {
            QTimer::singleShot(3000, &server, [&server] { server.spawn(QStringLiteral("/usr/bin/web")); });
            QTimer::singleShot(6000, &server, [&server] { server.toggleFullscreen(); });
        } else {
            QTimer::singleShot(3000, &server, [&server] { server.spawn(QStringLiteral("/usr/bin/files")); });
            if (!bootCfg().contains("noqml")) QTimer::singleShot(6000, &server, [&server] { server.spawn(QStringLiteral("/usr/bin/qmldemo")); });
            // "snapfiles": the file manager full screen in the picture
            if (bootCfg().contains("snapfiles")) QTimer::singleShot(5500, &server, [&server] { server.toggleFullscreen(); });
        }

        // measured load: frames per second while the pointer sweeps the dock
        // (magnification) and while workspaces switch (tiles slide)
        static int frames = 0;
        QObject::connect(win, &QQuickWindow::frameSwapped, win, [] {
            frames++;
            // every 60 frames: their wall time, average and worst frame
            static QElapsedTimer clk; static qint64 last = 0, worst = 0, start = 0; static int n = 0;
            if (!clk.isValid()) { clk.start(); last = start = 0; }
            const qint64 now = clk.elapsed();
            worst = qMax(worst, now - last);
            last = now;
            if (++n == 60) {
                fprintf(stderr, "[zerp2] 60 frames in %lld ms (%.1f fps), worst frame %lld ms\n",
                        (long long)(now - start), 60000.0 / qMax<qint64>(1, now - start), (long long)worst);
                n = 0; worst = 0; start = now;
            }
        }, Qt::DirectConnection);
        auto *fpsTick = new QTimer(win);
        QObject::connect(fpsTick, &QTimer::timeout, win, [] {
            fprintf(stderr, "[zerp2] fps %d\n", frames);
            frames = 0;
        });
        QTimer::singleShot(70000, win, [fpsTick] { fpsTick->start(1000); });
        auto *sweep = new QTimer(win);
        QObject::connect(sweep, &QTimer::timeout, win, [win, sweep] {
            static int step = 0;
            const qreal y = win->height() - 40;
            const qreal x = win->width() / 2.0 - 140 + (step % 140) * 2;
            QMouseEvent mv(QEvent::MouseMove, QPointF(x, y), QPointF(x, y), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(win, &mv);
            if (++step > 280) { sweep->stop(); fprintf(stderr, "[zerp2] sweep done\n"); }
        });
        QTimer::singleShot(72000, win, [sweep] { fprintf(stderr, "[zerp2] dock sweep\n"); sweep->start(16); });
        QTimer::singleShot(80000, &server, [&server] { fprintf(stderr, "[zerp2] workspace 2\n"); server.setWorkspace(2); });
        QTimer::singleShot(82000, &server, [&server] { fprintf(stderr, "[zerp2] workspace 1\n"); server.setWorkspace(1); });
        QTimer::singleShot(86000, win, [fpsTick] { fpsTick->stop(); });
        // "snapat=N": the picture at N seconds (60 by default), also as
        // /tmp/zerp2-snap.png at full size (the serial log can drop lines)
        int snapAt = 60;
        const QByteArray cfg = bootCfg();
        const int at = cfg.indexOf("snapat=");
        if (at >= 0) snapAt = qMax(5, cfg.mid(at + 7).split('\n').first().trimmed().toInt());
        QTimer::singleShot(snapAt * 1000, win, [win, greeterMode, firstBoot] {
            ::syscall(1000);   /* WynlandOS: every process's time and threads, on the log */
            const QImage full = win->grabWindow();
            // the login screen (another account) keeps its own file
            const QString snap = greeterMode ? (firstBoot ? QStringLiteral("/tmp/greeter-first.png") : QStringLiteral("/tmp/greeter-snap.png")) : QStringLiteral("/tmp/zerp2-snap.png");
            if (full.save(snap, "PNG")) {
                ::sync();
                fprintf(stderr, "[zerp2] snapshot saved: %s\n", qPrintable(snap));
            }
            QImage img = full.scaledToWidth(960, Qt::SmoothTransformation);
            QBuffer buf; buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            const QByteArray b64 = buf.data().toBase64();
            for (qsizetype i = 0; i < b64.size(); i += 1024)
                fprintf(stderr, "ZSNAP %s\n", b64.mid(i, 1024).constData());
            fprintf(stderr, "ZSNAP-END %d\n", int(b64.size()));
        });
    }
    return app.exec();
}
