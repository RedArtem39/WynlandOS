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

#include <cstdio>
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
    explicit InputPump(QQuickWindow *w) : m_win(w)
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

    void key(unsigned char sc)
    {
        if (sc == 0xE0) { m_e0 = true; return; }
        const bool release = sc & 0x80;
        const int code = sc & 0x7F;
        const bool e0 = m_e0;
        m_e0 = false;

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
    qputenv("QSG_INFO", "1");
    if (!qEnvironmentVariableIsSet("HOME")) qputenv("HOME", "/tmp");
    if (!qEnvironmentVariableIsSet("XDG_RUNTIME_DIR")) qputenv("XDG_RUNTIME_DIR", "/tmp");

    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &ws) {
        for (const QQmlError &w : ws) fprintf(stderr, "[zerp2] %s\n", qPrintable(w.toString()));
    });
    engine.rootContext()->setContextProperty(QStringLiteral("wallpaperUrl"),
        QFile::exists(QStringLiteral("/wall.png")) ? QStringLiteral("file:///wall.png") : QString());
    engine.load(QUrl::fromLocalFile(QStringLiteral("/usr/share/zerp2/qml/Shell.qml")));
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "[zerp2] FAIL: Shell.qml did not load\n");
        return 1;
    }
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!win) { fprintf(stderr, "[zerp2] FAIL: root is not a Window\n"); return 1; }
    InputPump pump(win);
    QObject::connect(win, &QQuickWindow::frameSwapped, win, [] {
        static int frames = 0;
        if (++frames == 1 || frames % 600 == 0) fprintf(stderr, "[zerp2] frame %d\n", frames);
    });
    fprintf(stderr, "[zerp2] shell up\n");

    if (bootCfg().contains("snapshot")) {
        QTimer::singleShot(20000, win, [win] {
            QImage img = win->grabWindow().scaledToWidth(1280, Qt::SmoothTransformation);
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
