// WynlandOS - Web: the browser. WPE WebKit pages as a Qt Quick item, in a
// Zerp window.
//
// Same plumbing as Files (qwynlandfb QPA plugin compiled in, software
// scene graph, fds from Zerp in argv[1..3]). WebKit runs on the GLib main
// loop, which is Qt's event loop here (Ubuntu's Qt dispatches through
// GLib); its frames come in through apps/web/wynwpe.c and are painted as
// an image. The UI is /usr/share/zerp/qml/web/Main.qml.

#include "webview.h"

#include <QtCore/QUrl>
#include <QtCore/QtPlugin>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQml/qqml.h>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <wpe/webkit.h>
#include "wynwpe.h"
}

#include "../../qt_qpa/qwynlandfb_zerpargs.h"

Q_IMPORT_PLUGIN(QWynlandFbIntegrationPlugin)

static WPEDisplay *g_display;

// ---------------------------------------------------------------- GLib side

static void on_frame(gpointer user, const guint8 *px, int w, int h, guint stride)
{
    static_cast<WebView *>(user)->frame(px, w, h, stride);
}

static void on_notify(GObject *, GParamSpec *, gpointer user)
{
    static_cast<WebView *>(user)->syncState();
}

static void on_load_changed(WebKitWebView *, WebKitLoadEvent ev, gpointer user)
{
    auto *v = static_cast<WebView *>(user);
    if (ev == WEBKIT_LOAD_STARTED) v->setError(QString());
    v->syncState();
}

static gboolean on_load_failed(WebKitWebView *, WebKitLoadEvent, char *uri, GError *err, gpointer user)
{
    if (g_error_matches(err, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)) return FALSE;
    static_cast<WebView *>(user)->setError(QString::fromUtf8(err->message) + QStringLiteral(" — ") +
                                           QString::fromUtf8(uri));
    return FALSE;
}

// target=_blank and window.open(): one window, one page -- open it here
static gboolean on_decide_policy(WebKitWebView *web, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type, gpointer)
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) return FALSE;
    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    webkit_web_view_load_request(web, webkit_navigation_action_get_request(action));
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

// ---------------------------------------------------------------- WebView

WebView::WebView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setFlag(ItemIsFocusScope, false);
    setActiveFocusOnTab(true);
    setFillColor(Qt::white);

    m_web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", g_display, NULL));
    m_view = webkit_web_view_get_wpe_view(m_web);
    wyn_wpe_view_set_frame_func(m_view, on_frame, this);

    WebKitSettings *s = webkit_web_view_get_settings(m_web);
    webkit_settings_set_enable_developer_extras(s, FALSE);
    webkit_settings_set_media_playback_requires_user_gesture(s, FALSE);

    g_signal_connect(m_web, "notify::uri", G_CALLBACK(on_notify), this);
    g_signal_connect(m_web, "notify::title", G_CALLBACK(on_notify), this);
    g_signal_connect(m_web, "notify::estimated-load-progress", G_CALLBACK(on_notify), this);
    g_signal_connect(m_web, "notify::is-loading", G_CALLBACK(on_notify), this);
    g_signal_connect(m_web, "load-changed", G_CALLBACK(on_load_changed), this);
    g_signal_connect(m_web, "load-failed", G_CALLBACK(on_load_failed), this);
    g_signal_connect(m_web, "decide-policy", G_CALLBACK(on_decide_policy), this);

    wpe_view_map(m_view);
    wpe_view_set_visible(m_view, TRUE);
}

WebView::~WebView()
{
    if (m_web) {
        g_signal_handlers_disconnect_by_data(m_web, this);
        wyn_wpe_view_set_frame_func(m_view, nullptr, nullptr);
        g_object_unref(m_web);
    }
}

bool WebView::canGoBack() const { return m_web && webkit_web_view_can_go_back(m_web); }
bool WebView::canGoForward() const { return m_web && webkit_web_view_can_go_forward(m_web); }

void WebView::load(const QString &text)
{
    QString t = text.trimmed();
    if (t.isEmpty()) return;
    QString url;
    if (t.contains(QLatin1String("://")) || t.startsWith(QLatin1String("about:")))
        url = t;
    else if (!t.contains(QLatin1Char(' ')) && (t.contains(QLatin1Char('.')) || t.startsWith(QLatin1String("localhost"))))
        url = QStringLiteral("https://") + t;
    else
        url = QStringLiteral("https://duckduckgo.com/html/?q=") + QString::fromUtf8(QUrl::toPercentEncoding(t));
    webkit_web_view_load_uri(m_web, url.toUtf8().constData());
    forceActiveFocus();
}

void WebView::goBack() { webkit_web_view_go_back(m_web); }
void WebView::goForward() { webkit_web_view_go_forward(m_web); }
void WebView::reload() { webkit_web_view_reload(m_web); }
void WebView::stop() { webkit_web_view_stop_loading(m_web); }

void WebView::setError(const QString &e)
{
    if (m_error == e) return;
    m_error = e;
    Q_EMIT errorChanged();
}

void WebView::syncState()
{
    const QString url = QString::fromUtf8(webkit_web_view_get_uri(m_web) ? webkit_web_view_get_uri(m_web) : "");
    const QString title = QString::fromUtf8(webkit_web_view_get_title(m_web) ? webkit_web_view_get_title(m_web) : "");
    const double progress = webkit_web_view_get_estimated_load_progress(m_web);
    const bool loading = webkit_web_view_is_loading(m_web);
    if (url != m_url) { m_url = url; Q_EMIT urlChanged(); }
    if (title != m_title) { m_title = title; Q_EMIT titleChanged(); }
    if (progress != m_progress) { m_progress = progress; Q_EMIT progressChanged(); }
    if (loading != m_loading) { m_loading = loading; Q_EMIT loadingChanged(); }
    Q_EMIT historyChanged();
}

void WebView::frame(const unsigned char *px, int w, int h, unsigned stride)
{
    static int frames;
    if (++frames == 1 || frames % 500 == 0) fprintf(stderr, "[web] frame %d: %dx%d\n", frames, w, h);
    // WebKit's ARGB8888 is QImage's premultiplied ARGB32
    m_frame = QImage(px, w, h, int(stride), QImage::Format_ARGB32_Premultiplied).copy();
    update();
}

void WebView::paint(QPainter *painter)
{
    if (!m_frame.isNull()) painter->drawImage(QPointF(0, 0), m_frame);
}

void WebView::resizeView()
{
    const int w = int(width()), h = int(height());
    if (w <= 0 || h <= 0) return;
    // the toplevel's size, and the view's: resizing a toplevel doesn't
    // resize its views by itself
    if (WPEToplevel *tl = wpe_view_get_toplevel(m_view)) wpe_toplevel_resize(tl, w, h);
    wpe_view_resized(m_view, w, h);
    fprintf(stderr, "[web] size %dx%d -> view %dx%d, mapped %d, visible %d\n", w, h,
            wpe_view_get_width(m_view), wpe_view_get_height(m_view),
            wpe_view_get_mapped(m_view), wpe_view_get_visible(m_view));
}

void WebView::geometryChange(const QRectF &n, const QRectF &o)
{
    QQuickPaintedItem::geometryChange(n, o);
    if (n.size() != o.size()) resizeView();
}

// ---------------------------------------------------------------- input

static guint32 now_ms() { return guint32(g_get_monotonic_time() / 1000); }

static WPEModifiers mods(Qt::KeyboardModifiers m, Qt::MouseButtons b = Qt::NoButton)
{
    unsigned r = 0;
    if (m & Qt::ControlModifier) r |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (m & Qt::ShiftModifier) r |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (m & Qt::AltModifier) r |= WPE_MODIFIER_KEYBOARD_ALT;
    if (m & Qt::MetaModifier) r |= WPE_MODIFIER_KEYBOARD_META;
    if (b & Qt::LeftButton) r |= WPE_MODIFIER_POINTER_BUTTON1;
    if (b & Qt::MiddleButton) r |= WPE_MODIFIER_POINTER_BUTTON2;
    if (b & Qt::RightButton) r |= WPE_MODIFIER_POINTER_BUTTON3;
    return WPEModifiers(r);
}

static guint wpe_button(Qt::MouseButton b)
{
    switch (b) {
    case Qt::RightButton: return WPE_BUTTON_SECONDARY;
    case Qt::MiddleButton: return WPE_BUTTON_MIDDLE;
    default: return WPE_BUTTON_PRIMARY;
    }
}

void WebView::pointer(int type, Qt::MouseButton button, const QPointF &p, Qt::KeyboardModifiers m)
{
    WPEEvent *ev;
    if (type == WPE_EVENT_POINTER_MOVE || type == WPE_EVENT_POINTER_LEAVE)
        ev = wpe_event_pointer_move_new(WPEEventType(type), m_view, WPE_INPUT_SOURCE_MOUSE, now_ms(),
                                        mods(m, m_buttons), p.x(), p.y(), 0, 0);
    else {
        const guint b = wpe_button(button);
        const guint count = type == WPE_EVENT_POINTER_DOWN
            ? wpe_view_compute_press_count(m_view, p.x(), p.y(), b, now_ms()) : 1;
        ev = wpe_event_pointer_button_new(WPEEventType(type), m_view, WPE_INPUT_SOURCE_MOUSE, now_ms(),
                                          mods(m, m_buttons), b, p.x(), p.y(), count);
    }
    wpe_view_event(m_view, ev);
    wpe_event_unref(ev);
}

void WebView::mousePressEvent(QMouseEvent *e)
{
    forceActiveFocus();
    m_buttons = e->buttons();
    pointer(WPE_EVENT_POINTER_DOWN, e->button(), e->position(), e->modifiers());
}
void WebView::mouseReleaseEvent(QMouseEvent *e)
{
    m_buttons = e->buttons();
    pointer(WPE_EVENT_POINTER_UP, e->button(), e->position(), e->modifiers());
}
void WebView::mouseMoveEvent(QMouseEvent *e) { pointer(WPE_EVENT_POINTER_MOVE, Qt::NoButton, e->position(), e->modifiers()); }
void WebView::hoverMoveEvent(QHoverEvent *e) { pointer(WPE_EVENT_POINTER_MOVE, Qt::NoButton, e->position(), e->modifiers()); }
void WebView::hoverLeaveEvent(QHoverEvent *e) { pointer(WPE_EVENT_POINTER_LEAVE, Qt::NoButton, e->position(), e->modifiers()); }

void WebView::wheelEvent(QWheelEvent *e)
{
    // one notch = 120; WebKit scrolls a line step per discrete delta,
    // positive delta_y is down
    const QPoint a = e->angleDelta();
    WPEEvent *ev = wpe_event_scroll_new(m_view, WPE_INPUT_SOURCE_MOUSE, now_ms(), mods(e->modifiers()),
                                        -a.x() / 120.0, -a.y() / 120.0, FALSE, FALSE,
                                        e->position().x(), e->position().y());
    wpe_view_event(m_view, ev);
    wpe_event_unref(ev);
}

static guint keyval_for(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Return: case Qt::Key_Enter: return WPE_KEY_Return;
    case Qt::Key_Backspace: return WPE_KEY_BackSpace;
    case Qt::Key_Tab: case Qt::Key_Backtab: return WPE_KEY_Tab;
    case Qt::Key_Escape: return WPE_KEY_Escape;
    case Qt::Key_Delete: return WPE_KEY_Delete;
    case Qt::Key_Insert: return WPE_KEY_Insert;
    case Qt::Key_Home: return WPE_KEY_Home;
    case Qt::Key_End: return WPE_KEY_End;
    case Qt::Key_Left: return WPE_KEY_Left;
    case Qt::Key_Right: return WPE_KEY_Right;
    case Qt::Key_Up: return WPE_KEY_Up;
    case Qt::Key_Down: return WPE_KEY_Down;
    case Qt::Key_PageUp: return WPE_KEY_Page_Up;
    case Qt::Key_PageDown: return WPE_KEY_Page_Down;
    case Qt::Key_Shift: return WPE_KEY_Shift_L;
    case Qt::Key_Control: return WPE_KEY_Control_L;
    case Qt::Key_Alt: return WPE_KEY_Alt_L;
    case Qt::Key_Meta: return WPE_KEY_Super_L;
    case Qt::Key_Space: return ' ';
    default: break;
    }
    if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F12) return WPE_KEY_F1 + guint(e->key() - Qt::Key_F1);
    const QString t = e->text();
    if (!t.isEmpty() && t.at(0).unicode() >= 0x20) return wpe_unicode_to_keyval(t.toUcs4().value(0));
    // Ctrl+letter: the letter (text is a control character then)
    if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z)
        return wpe_unicode_to_keyval(guint32((e->modifiers() & Qt::ShiftModifier) ? e->key() : e->key() + 32));
    return 0;
}

void WebView::key(QKeyEvent *e, bool down)
{
    const guint kv = keyval_for(e);
    if (!kv) { e->ignore(); return; }
    WPEEvent *ev = wpe_event_keyboard_new(down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
                                          m_view, WPE_INPUT_SOURCE_KEYBOARD, now_ms(), mods(e->modifiers()),
                                          e->nativeScanCode(), kv);
    wpe_view_event(m_view, ev);
    wpe_event_unref(ev);
}

void WebView::keyPressEvent(QKeyEvent *e) { key(e, true); }
void WebView::keyReleaseEvent(QKeyEvent *e) { key(e, false); }
void WebView::focusInEvent(QFocusEvent *e) { QQuickPaintedItem::focusInEvent(e); wpe_view_focus_in(m_view); }
void WebView::focusOutEvent(QFocusEvent *e) { QQuickPaintedItem::focusOutEvent(e); wpe_view_focus_out(m_view); }

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
    zerp_qpa_capture_args(argc, argv);
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    if (!qEnvironmentVariableIsSet("FONTCONFIG_FILE"))
        qputenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf");
    if (!qEnvironmentVariableIsSet("HOME"))
        qputenv("HOME", "/home/user");
    // WebKit's processes inherit these: no bubblewrap here (no namespaces
    // yet), JavaScriptCore's JIT allowed, pages drawn on the CPU into
    // shared memory (apps/web/wynwpe.c)
    qputenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1");
    qputenv("WYNLAND_ALLOW_JIT", "1");
    qputenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
    if (!qEnvironmentVariableIsSet("LD_LIBRARY_PATH")) qputenv("LD_LIBRARY_PATH", "/lib64");
    if (!qEnvironmentVariableIsSet("GST_DEBUG")) qputenv("GST_DEBUG", "1");

    QGuiApplication app(argc, argv);

    // WebKit's main-thread setup before any other WebKit/WPE object
    webkit_web_context_get_default();
    const QSize scr = app.primaryScreen() ? app.primaryScreen()->size() : QSize(1600, 900);
    g_display = wyn_wpe_display_new(scr.width(), scr.height());

    qmlRegisterType<WebView>("Wynland.Web", 1, 0, "WebView");
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("startUrl"),
        argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("https://www.youtube.com/"));
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &ws) {
        for (const QQmlError &w : ws) fprintf(stderr, "[web] %s\n", qPrintable(w.toString()));
    });
    const QString qml = QStringLiteral("/usr/share/zerp/qml/web/Main.qml");
    engine.load(QUrl::fromLocalFile(qml));
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "[web] FAIL: could not load %s\n", qPrintable(qml));
        return 1;
    }
    fprintf(stderr, "[web] up\n");
    return app.exec();
}
