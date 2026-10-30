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
#include <QtWaylandCompositor/QWaylandCompositor>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "zerp2.h"
#include "greeter.h"
#include "hypripc.h"
#include "zlayershell.h"
#include "zglobalshortcuts.h"
#include "keybinds.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

// The input devices are root's: whoever starts the display server (the
// login manager, init) opens them and passes the fds in WYNLAND_KBD_FD /
// WYNLAND_MICE_FD. They stay ours alone: close-on-exec, and the variables
// do not reach our clients. Run as root, we open them ourselves.
// No GPU of ours on this machine: the kernel's DRM node is a simpledrm
// (the firmware's framebuffer; drivers/video/virtgpu_drm.c). Everything
// renders with llvmpipe then -- we through GBM's kms_swrast, the clients
// into shared memory (wl_shm): no GPU buffers to pass around.
static QByteArray drmDriver()
{
    struct { int major, minor, patch; size_t name_len; char *name; size_t date_len; char *date;
             size_t desc_len; char *desc; } v = {};
    char name[32] = {};
    v.name_len = sizeof name - 1;
    v.name = name;
    const int fd = ::open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return QByteArray();
    const bool ok = ::ioctl(fd, _IOWR('d', 0x00, decltype(v)), &v) == 0;   // DRM_IOCTL_VERSION
    ::close(fd);
    return ok ? QByteArray(name) : QByteArray();
}

static int inputFd(const char *var, const char *path)
{
    int fd = -1;
    if (const char *v = getenv(var)) fd = atoi(v);
    else fd = ::open(path, O_RDONLY);
    unsetenv(var);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}
static int g_kbdFd = -1, g_miceFd = -1;   // the mouse stream (syscall 412) needs the mice fd held

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
// (their text: the layout's, zerp_layouts.h)

// the demo's "snapat=key+N": run once by the first global shortcut pressed
static std::function<void()> g_onGlobal;

class InputPump : public QObject
{
public:
    // greeter: the login screen -- no WM bindings (they start programs)
    InputPump(QQuickWindow *w, ZServer *srv, bool greeter = false) : m_win(w), m_srv(srv), m_greeter(greeter)
    {
        m_kbd = g_kbdFd;
        // the configured bindings (keybinds.h: Caelestia's, Hyprland style)
        // and keyboard layouts (again on hyprctl reload: keyboard(1)); the
        // login screen: the machine's layouts (/etc/wynland/keyboard.conf)
        if (!m_greeter) {
            m_binds.setRun([this](const KeyBinds::Bind &b, bool pressed) { run(b, pressed); });
            reload();
            connect(srv, &ZServer::reloadRequested, this, &InputPump::reload);
        } else {
            m_binds.loadKeyboard(true);
            m_srv->setLayouts(m_binds.layouts());
        }
        connect(&m_timer, &QTimer::timeout, this, &InputPump::poll);
        m_timer.start(4);
    }

    void reload()
    {
        m_binds.load();
        m_srv->setLayouts(m_binds.layouts());
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
        HyprIpc::setCursor(QPoint(e.x, e.y));
        m_srv->setCursor(QPoint(e.x, e.y));
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

    // a configured binding's action (Hyprland's dispatchers that mean
    // something here). Globals hear both the press and the release; the
    // rest act on the press.
    void run(const KeyBinds::Bind &b, bool pressed)
    {
        const QString &a = b.args;
        if (b.dispatcher == QLatin1String("global")) {
            if (ZGlobalShortcuts *g = ZGlobalShortcuts::instance()) g->fire(a, pressed);
            if (pressed && g_onGlobal) { auto f = std::move(g_onGlobal); g_onGlobal = nullptr; f(); }
            return;
        }
        if (!pressed) return;
        const QString d = b.dispatcher;
        if (d == QLatin1String("exec")) { m_srv->run(a); return; }
        if (d == QLatin1String("killactive")) { m_srv->closeFocused(); return; }
        if (d == QLatin1String("fullscreen")) { m_srv->toggleFullscreen(); return; }
        if (d == QLatin1String("exit")) { QCoreApplication::quit(); return; }
        if (d == QLatin1String("togglecanvas")) { m_srv->toggleCanvas(); return; }
        if (d == QLatin1String("canvaszoom")) { m_srv->canvasZoom(a.trimmed()); return; }
        if (d == QLatin1String("canvas")) { m_srv->canvasAction(a.trimmed()); return; }
        if (d == QLatin1String("cyclewindows")) { m_srv->cycleWindows(a.trimmed().startsWith('p') ? -1 : 1); return; }
        if (d == QLatin1String("movefocus")) {
            const QString w = a.trimmed().toLower();
            if (w.startsWith('l')) m_srv->focusDirection(-1, 0);
            else if (w.startsWith('r')) m_srv->focusDirection(1, 0);
            else if (w.startsWith('u')) m_srv->focusDirection(0, -1);
            else if (w.startsWith('d')) m_srv->focusDirection(0, 1);
            return;
        }
        if (d == QLatin1String("workspace") || d == QLatin1String("movetoworkspace") ||
            d == QLatin1String("movetoworkspacesilent")) {
            // a number (1-10), or relative: +1, -1, e+1, r-1
            QString w = a.trimmed();
            if (w.startsWith('e') || w.startsWith('r') || w.startsWith('m')) w = w.mid(1);
            int target;
            bool ok = false;
            if (w.startsWith('+') || w.startsWith('-')) target = m_srv->workspace() + w.toInt(&ok);
            else target = w.toInt(&ok) - 1;
            if (!ok) return;
            target = qBound(0, target, 9);
            if (d == QLatin1String("workspace")) m_srv->setWorkspace(target);
            else m_srv->moveFocusedTo(target);
            return;
        }
        fprintf(stderr, "[zerp2] binding (line %d): dispatcher %s is not supported\n", b.line, qPrintable(d));
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

    // The keyboard layout's switch. Alt+Shift: on letting go of either,
    // if nothing else was pressed meanwhile (Super+Shift+Alt+S stays a
    // shortcut); the keys still reach the window -- they are modifiers.
    // Super+Space: on the press, Space swallowed (true).
    bool layoutToggle(bool e0, int code, bool release)
    {
        const bool alt = code == 0x38, shift = !e0 && (code == 0x2A || code == 0x36);        if (!m_greeter && m_binds.toggle() == KeyBinds::SuperSpace) {
            if (!release && !e0 && code == 0x39 && m_super) { m_srv->nextLayout(); return true; }
            return false;
        }
        if (!release) {
            if ((alt && m_shift) || (shift && (m_alt || m_altR))) m_layoutArmed = true;
            else if (!alt && !shift) m_layoutArmed = false;
        } else if (m_layoutArmed && (alt || shift)) {
            m_layoutArmed = false;
            m_srv->nextLayout();
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
        if (!e0 && code == 0x38) { m_alt = !release; if (release) m_srv->altReleased(); }
        if (e0 && code == 0x38) m_altR = !release;
        if (e0 && (code == 0x5B || code == 0x5C)) m_super = !release;
        if (!e0 && (code == 0x2A || code == 0x36)) m_shift = !release;
        const int kid = code + (e0 ? 128 : 0);
        if (layoutToggle(e0, code, release)) { m_swallow[kid] = true; return; }
        if (!m_greeter && m_binds.key(kid, release)) return;
        if (release && m_swallow[kid]) { m_swallow[kid] = false; return; }
        if (!m_greeter && !release && (m_alt || m_altR || m_super) && binding(e0, code)) {
            m_swallow[kid] = true;   // its release must not reach the client either
            return;
        }
        // everything else: raw scancodes to the focused client
        if (ZClient *c = m_srv->focused()) {
            // the modifiers held, for Wayland clients
            Qt::KeyboardModifier m = Qt::NoModifier;
            if (code == 0x2A || code == 0x36) m = e0 ? Qt::NoModifier : Qt::ShiftModifier;
            else if (code == 0x1D) m = Qt::ControlModifier;
            else if (code == 0x38) m = Qt::AltModifier;
            else if (e0 && (code == 0x5B || code == 0x5C)) m = Qt::MetaModifier;
            if (m != Qt::NoModifier) { if (release) m_mods &= ~m; else m_mods |= m; }
            c->sendKey(e0, sc, m_mods);
            return;
        }

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
            const uint16_t c = zerp_key_char(m_srv->layout(), code, (m_mods & Qt::ShiftModifier) != 0, m_caps);
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
    KeyBinds m_binds;
    bool m_layoutArmed = false;   // Alt+Shift held, nothing else yet
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

// The screen mode chosen with resolution(1): "mode = WxH" in the user's
// ~/.config/wynland/display.conf, else the machine's /etc/wynland/
// display.conf (the login screen: only that one). Empty: the connector's
// preferred mode (the boot size).
static QByteArray displayMode(bool greeter)
{
    QStringList files;
    if (!greeter) files << QString::fromLocal8Bit(qgetenv("HOME")) + QStringLiteral("/.config/wynland/display.conf");
    files << QStringLiteral("/etc/wynland/display.conf");
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        for (const QByteArray &raw : f.readAll().split('\n')) {
            const QByteArray line = raw.trimmed();
            if (!line.startsWith("mode")) continue;
            const int eq = line.indexOf('=');
            if (eq < 0) continue;
            const QByteArray v = line.mid(eq + 1).trimmed();
            const QList<QByteArray> wh = v.split('x');
            if (wh.size() == 2 && wh[0].toInt() > 0 && wh[1].toInt() > 0) return v;
        }
        return QByteArray();   // the user's file without a mode: the boot size
    }
    return QByteArray();
}

// eglfs's KMS configuration (/usr/share/zerp2/kms.json), with that mode
// for the one output (Qt names it Virtual1: virtio-gpu's connector), in
// the session's runtime directory
static QByteArray kmsConfig(const QByteArray &mode)
{
    const QByteArray base = "/usr/share/zerp2/kms.json";
    if (mode.isEmpty()) return base;
    QFile in(QString::fromLatin1(base));
    if (!in.open(QIODevice::ReadOnly)) return base;
    QByteArray json = in.readAll().trimmed();
    if (!json.endsWith('}')) return base;
    json.chop(1);
    json += ",\n    \"outputs\": [ { \"name\": \"Virtual1\", \"mode\": \"" + mode + "\" } ]\n}\n";
    const QByteArray path = qgetenv("XDG_RUNTIME_DIR") + "/zerp2-kms.json";
    QFile out(QString::fromLocal8Bit(path));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(json) != json.size()) return base;
    return path;
}

int main(int argc, char **argv)
{
    g_kbdFd = inputFd("WYNLAND_KBD_FD", "/dev/input/kbd");
    g_miceFd = inputFd("WYNLAND_MICE_FD", "/dev/input/mice");
    fprintf(stderr, "[zerp2] input: keyboard %s, pointer %s\n", g_kbdFd >= 0 ? "ok" : "MISSING",
            g_miceFd >= 0 ? "ok" : "MISSING");
    qputenv("QT_QPA_PLATFORM", "eglfs");
    qputenv("QT_QPA_EGLFS_INTEGRATION", "eglfs_kms");
    qputenv("QT_QPA_EGLFS_KMS_ATOMIC", "0");
    qputenv("QT_QPA_EGLFS_DISABLE_INPUT", "1");   // no evdev here -- InputPump instead
    qputenv("QT_QPA_EGLFS_HIDECURSOR", "1");      // the GPU cursor plane is the pointer
    qputenv("QSG_RHI_BACKEND", "opengl");
    // GPU buffers from clients: linux-dmabuf (ours, version 4 -- see
    // tools/zerp2-dmabuf). Ubuntu's Mesa serves no wl_drm, so there is no
    // eglBindWaylandDisplayWL for the plugin to wait on
    qputenv("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION", "linux-dmabuf-unstable-v1");
    qputenv("QT_WAYLAND_IGNORE_BIND_DISPLAY", "1");
    const QByteArray driver = drmDriver();
    const bool software = driver == "simpledrm";
    // The Intel GPU (drivers/gpu/i915_uapi.c): its depth (HiZ) and colour
    // compression (CCS) surfaces don't work on our driver yet -- depth
    // tests fail everywhere (only the clear colour shows) and compressed
    // draws hang the GPU. Mesa without them, for us and (inherited) the
    // clients, until the kernel side is right.
    if (driver == "i915" && !qEnvironmentVariableIsSet("INTEL_DEBUG")) {
        fprintf(stderr, "[zerp2] Intel GPU: Mesa without HiZ/CCS (INTEL_DEBUG=noccs,nohiz)\n");
        qputenv("INTEL_DEBUG", "noccs,nohiz");
    }
    if (software) {
        fprintf(stderr, "[zerp2] no GPU (simpledrm): llvmpipe for us and the clients\n");
        qputenv("GBM_ALWAYS_SOFTWARE", "1");
        qputenv("LIBGL_ALWAYS_SOFTWARE", "1");   // inherited: the clients' EGL draws into wl_shm
        qunsetenv("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION");
    }
    /* scene graph diagnostics on request only (BOOT_EXTRA=qsginfo): every
       Qt client inherits it, and it all lands on the synchronous serial
       console */
    if (bootCfg().contains("qsginfo")) qputenv("QSG_INFO", "1");
    // BOOT_EXTRA=hwdebug: how client GPU buffers come in (linux-dmabuf)
    if (bootCfg().contains("hwdebug"))
        qputenv("QT_LOGGING_RULES", "qt.waylandcompositor.hardwareintegration.debug=true");
    if (!qEnvironmentVariableIsSet("HOME")) qputenv("HOME", "/tmp");
    // the session's own runtime directory: the Wayland socket lives there,
    // and /tmp/wayland-0 left by another user's session could not be
    // replaced (sticky /tmp)
    if (!qEnvironmentVariableIsSet("XDG_RUNTIME_DIR") || qgetenv("XDG_RUNTIME_DIR") == "/tmp") {
        const QByteArray rt = "/tmp/xdg-" + QByteArray::number(uint(getuid()));
        ::mkdir(rt.constData(), 0700);
        qputenv("XDG_RUNTIME_DIR", rt);
    }

    // zerp2 --greeter REQ_FD REP_FD [--first]: the login screen (wynlogin)
    const bool greeterMode = argc >= 4 && !strcmp(argv[1], "--greeter");
    const int greeterReq = greeterMode ? atoi(argv[2]) : -1;
    const int greeterRep = greeterMode ? atoi(argv[3]) : -1;
    const bool firstBoot = greeterMode && argc >= 5 && !strcmp(argv[4], "--first");
    const QByteArray mode = displayMode(greeterMode);
    qputenv("QT_QPA_EGLFS_KMS_CONFIG", kmsConfig(mode));

    QGuiApplication app(argc, argv);
    qmlRegisterType<ZSurface>("Zerp", 1, 0, "ZSurface");
    qmlRegisterUncreatableType<ZClient>("Zerp", 1, 0, "ZClient", QStringLiteral("made by the server"));
    qmlRegisterType<ZLayerShellQuickExtension>("Zerp", 1, 0, "LayerShell");
    qmlRegisterType<ZGlobalShortcutsQuickExtension>("Zerp", 1, 0, "GlobalShortcuts");
    qmlRegisterUncreatableType<ZLayerSurface>("Zerp", 1, 0, "LayerSurface", QStringLiteral("made by the layer shell"));
    const QSize scr = app.primaryScreen() ? app.primaryScreen()->size() : QSize(1920, 1080);
    fprintf(stderr, "[zerp2] screen %s %dx%d (mode %s)\n",
            app.primaryScreen() ? qPrintable(app.primaryScreen()->name()) : "?", scr.width(), scr.height(),
            mode.isEmpty() ? "preferred" : mode.constData());
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
    const QString outputName = app.primaryScreen() ? app.primaryScreen()->name() : QStringLiteral("Virtual-1");
    engine.rootContext()->setContextProperty(QStringLiteral("outputName"), outputName);
    const QString qml = greeterMode ? QStringLiteral("Greeter.qml") : QStringLiteral("Shell.qml");
    engine.load(QUrl::fromLocalFile(QStringLiteral("/usr/share/zerp2/qml/") + qml));
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "[zerp2] FAIL: %s did not load\n", qPrintable(qml));
        return 1;
    }
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!win) { fprintf(stderr, "[zerp2] FAIL: root is not a Window\n"); return 1; }
    // the keyboard layout's keymap for Wayland clients goes through its seat
    server.setCompositor(win->findChild<QWaylandCompositor *>());
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
            if (np.size() == 2 || np.size() == 3) {   // NAME:PASSWORD[:DESKTOP] (zerp, caelestia)
                const int when = cfg.contains("snapat=") ? 30000 : 8000;   // after the screen's own picture
                QTimer::singleShot(when, greeter, [greeter, np] {
                    if (greeter->firstBoot()) greeter->createAccount(QString::fromUtf8(np[0]), QString(), QString::fromUtf8(np[1]),
                                                                     np.size() == 3 ? QString::fromUtf8(np[2]) : QString());
                    else greeter->login(QString::fromUtf8(np[0]), QString::fromUtf8(np[1]));
                });
            }
        }
    }
    if (!greeterMode) {
        // what the session starts inherits these: Wayland programs find
        // the compositor (Shell.qml), Qt ones take its platform plugin
        // (ours, the Zerp clients, pick their own)
        qputenv("WAYLAND_DISPLAY", "wayland-0");
        qputenv("QT_QPA_PLATFORM", "wayland");
        // Qt clients read the same variable for their side: theirs is
        // wayland-egl (Mesa's EGL; it speaks linux-dmabuf to us)
        qputenv("QT_WAYLAND_CLIENT_BUFFER_INTEGRATION", "wayland-egl");
        qunsetenv("QT_WAYLAND_IGNORE_BIND_DISPLAY");
        if (software) qunsetenv("GBM_ALWAYS_SOFTWARE");
        // shells made for Hyprland (Quickshell's) talk to us as to it
        new HyprIpc(&server, outputName, scr, &app);
        server.spawn(QStringLiteral("/usr/bin/term"));   // a terminal to start with
        // the desktop the account chose on its first boot (wynlogin saved
        // it; boot.cfg "desktop=NAME" overrides it, for screenshots): a
        // Quickshell shell (xvlet, Caelestia) takes the edges, our bar and
        // dock step aside (Shell.qml: externalShell) -- and come back if it dies
        QByteArray desktop;
        {
            QFile df(qEnvironmentVariable("HOME") + QStringLiteral("/.config/wynland/desktop"));
            if (df.open(QIODevice::ReadOnly)) desktop = df.readAll().trimmed();
            const QByteArray bc = bootCfg();
            const int d = bc.indexOf("desktop=");
            if (d >= 0 && (d == 0 || bc[d - 1] == '\n')) desktop = bc.mid(d + 8).split('\n').first().trimmed();
        }
        if (desktop == "xvlet" && QFile::exists(QStringLiteral("/usr/bin/quickshell")) &&
            QFile::exists(QStringLiteral("/usr/share/xvlet/shell.qml"))) {
            QTimer::singleShot(1000, &server, [&server] {
                server.run(QStringLiteral("mkdir -p \"$HOME/.cache\"; QT_QUICK_BACKEND=opengl "
                                          "exec quickshell -p /usr/share/xvlet > \"$HOME/.cache/xvlet.log\" 2>&1"));
            });
        }
        {
            if (desktop == "caelestia" &&
                QFile::exists(QStringLiteral("/usr/bin/quickshell"))) {
                QTimer::singleShot(1000, &server, [&server] {
                    // (its state directory: the launcher's app database is
                    // there -- Caelestia's CLI makes it, not in the image)
                    server.run(QStringLiteral("mkdir -p \"$HOME/.cache\" \"$HOME/.local/state/caelestia\"; QT_QUICK_BACKEND=opengl "
                                              "exec quickshell -c caelestia > \"$HOME/.cache/caelestia.log\" 2>&1"));
                });
            }
        }
        // debugging: boot.cfg "wlrun=CMD[;CMD...]" -- programs started a few
        // seconds in (Wayland clients, for the picture and the tests)
        const QByteArray cfg = bootCfg();
        const int at = cfg.indexOf("wlrun=");
        if (at >= 0) {
            const QList<QByteArray> cmds = cfg.mid(at + 6).split('\n').first().trimmed().split(';');
            int when = 4000;
            for (const QByteArray &c : cmds) {
                const QString cmd = QString::fromUtf8(c).replace(QLatin1Char('+'), QLatin1Char(' '));
                QTimer::singleShot(when, &server, [&server, cmd] { server.run(cmd); });
                when += 2500;
            }
        }
    }
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
        // /tmp/zerp2-snap.png at full size (the serial log can drop lines);
        // "snapat=key+N": N seconds after the first global shortcut
        int snapAt = 60;
        bool afterKey = false;
        QList<int> series;   // "snapat=40,70,100": several, /tmp/zerp2-snap-N.png (no serial copy)
        const QByteArray cfg = bootCfg();
        const int at = cfg.indexOf("snapat=");
        if (at >= 0) {
            QByteArray v = cfg.mid(at + 7).split('\n').first().trimmed();
            if (v.startsWith("key+")) { afterKey = true; v = v.mid(4); }
            if (!afterKey && v.contains(',')) for (const QByteArray &s : v.split(',')) series.append(qMax(5, s.trimmed().toInt()));
            snapAt = afterKey ? qMax(1, v.toInt()) : qMax(5, v.toInt());
        }
        for (int k = 0; k < series.size(); k++) {
            QTimer::singleShot(series[k] * 1000, win, [win, k] {
                const QString f = QStringLiteral("/tmp/zerp2-snap-%1.png").arg(k + 1);
                if (win->grabWindow().save(f, "PNG")) { ::sync(); fprintf(stderr, "[zerp2] snapshot saved: %s\n", qPrintable(f)); }
            });
        }
        auto snapshot = [win, greeterMode, firstBoot] {
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
        };
        if (afterKey)
            g_onGlobal = [win, snapAt, snapshot] { QTimer::singleShot(snapAt * 1000, win, snapshot); };
        else if (series.isEmpty())
            QTimer::singleShot(snapAt * 1000, win, snapshot);
    }
    const int rc = app.exec();
    fprintf(stderr, "[zerp2] session ends (status %d)\n", rc);
    // no teardown: QtWaylandCompositor's destructors crashed with clients
    // still connected (and the status -- a restart asked for -- was lost
    // to the signal). Nothing is left to save; the kernel gives the
    // screen back to the console when the DRM fd closes.
    fflush(nullptr);
    ::_exit(rc);
}
