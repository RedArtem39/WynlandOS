// WynlandOS - Zerp 2.0: client hosting for the Qt Quick WM.
//
// Two kinds of clients, tiled alike:
//  - Zerp clients (zerp_term, the Qt apps through the qwynlandfb plugin):
//    spawned with a c2s pipe, an s2c pipe and a shared-memory pixel buffer
//    (argv[1..3]), they render into the buffer at the size of their
//    TILE_RECT and report DAMAGE; a ZSurface item shows the buffer.
//  - Wayland clients (any program speaking xdg-shell; Shell.qml runs the
//    compositor): a ShellSurfaceItem shows the surface, the size goes out
//    as an xdg_toplevel configure.
// QML owns the layout (tiling, workspaces, animations) and tells each
// client its size.
#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtCore/QVector>
#include <QtQuick/QQuickItem>
#include <cstdint>

class QWaylandXdgToplevel;

extern "C" {
#include "../../zerp_protocol.h"
}

class ZClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int id READ id CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString appId READ appId NOTIFY titleChanged)
    /* a Wayland client: shellSurface is its QWaylandXdgSurface */
    Q_PROPERTY(bool wayland READ wayland CONSTANT)
    Q_PROPERTY(QObject *shellSurface READ shellSurface CONSTANT)
    Q_PROPERTY(int workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)
    Q_PROPERTY(bool focused READ focused NOTIFY focusedChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged)
    /* the client draws with alpha (ZERP_MSG_ALPHA): blended, glass behind */
    Q_PROPERTY(bool alpha READ alpha NOTIFY alphaChanged)
public:
    ZClient(int id, const QString &path, QObject *parent);
    ZClient(int id, QObject *xdgSurface, QObject *parent);   // a Wayland toplevel
    ~ZClient() override;

    bool start(uint32_t shmBytes);
    int id() const { return m_id; }
    QString title() const { return m_title; }
    QString appId() const { return m_appId; }
    int workspace() const { return m_workspace; }
    void setWorkspace(int w) { if (w != m_workspace) { m_workspace = w; emit workspaceChanged(); } }
    bool focused() const { return m_focused; }
    void setFocused(bool f);
    bool fullscreen() const { return m_fullscreen; }
    bool alpha() const { return m_alpha; }
    void setFullscreen(bool f);
    bool wayland() const { return m_wayland; }
    QObject *shellSurface() const { return m_xdg; }

    // QML: the size the client should render at (the layout's target,
    // not the animated in-between sizes)
    Q_INVOKABLE void configure(int x, int y, int w, int h);
    Q_INVOKABLE void close();

    void sendMouse(int x, int y, uint32_t buttons, bool motion);
    void wlKeyboardFocus(bool on);
    // a raw scancode (release bit 7); mods: the modifiers held, for
    // Wayland clients (their xkb state follows them)
    void sendKey(bool e0, uint8_t scancode, Qt::KeyboardModifiers mods = Qt::NoModifier);
    // drains c2s; returns false once the client is gone
    bool pump(QVector<QString> *spawnRequests);
    bool alive() const;

    // pixels, for ZSurface (render thread, GUI thread blocked)
    const uint32_t *pixels() const { return m_shm; }
    uint32_t bufferBytes() const { return m_shmBytes; }
    int bufWidth() const { return m_bufW; }
    int bufHeight() const { return m_bufH; }
    bool takeDirty() { bool d = m_dirty; m_dirty = false; return d; }
    // union of DAMAGE since the last take, in buffer coordinates (the
    // whole buffer after a resize)
    QRect takeDamage() { QRect r = m_damage; m_damage = QRect(); return r; }

signals:
    void titleChanged();
    void workspaceChanged();
    void focusedChanged();
    void fullscreenChanged();
    void alphaChanged();
    void damaged();
    void fullscreenRequested(ZClient *self, bool on);   // a Wayland client asked

private:
    void send(const ZerpMsg &m, bool motion = false);
    void flush();

    int m_id;
    QString m_path, m_title, m_appId;
    int m_workspace = 1;
    bool m_focused = false, m_fullscreen = false, m_alpha = false;
    int m_c2s = -1, m_s2c = -1;
    long m_pid = -1;
    uint32_t *m_shm = nullptr;
    uint32_t m_shmBytes = 0;
    int m_bufW = 0, m_bufH = 0;      // size the client renders at
    // asked for, not yet drawn: the SHM rows are still the old width until
    // the client's first full frame at the new size (or a timeout)
    int m_pendW = 0, m_pendH = 0;
    qint64 m_pendSince = 0;
    void adoptPending();
    bool m_dirty = false;
    QRect m_damage;
    // Wayland
    bool m_wayland = false;
    QPointer<QObject> m_xdg;         // QWaylandXdgSurface; gone with the client
    QPointer<QObject> m_top;         // its QWaylandXdgToplevel: a client may destroy it first
    QWaylandXdgToplevel *toplevel() const;
    QSize m_wlSize;
    void wlConfigure();
    QByteArray m_rx;
    QVector<ZerpMsg> m_out;          // waiting for room in the s2c pipe
    bool m_outTailMotion = false;
};

// The windows as a list model: adding or removing one inserts/removes a
// row, so QML creates/destroys just that tile (a plain list property made
// the Repeater rebuild every tile on every change).
class ZClientModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum { ClientRole = Qt::UserRole + 1 };
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &p = QModelIndex()) const override { return p.isValid() ? 0 : m_list.size(); }
    QVariant data(const QModelIndex &i, int role) const override;
    QHash<int, QByteArray> roleNames() const override { return { { ClientRole, "client" } }; }
    void add(ZClient *c);
    void remove(ZClient *c);
private:
    QVector<ZClient *> m_list;
};

class ZServer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<QObject *> clients READ clients NOTIFY clientsChanged)
    Q_PROPERTY(QObject *model READ model CONSTANT)
    Q_PROPERTY(QObject *focusedClient READ focusedClient NOTIFY focusChanged)
    Q_PROPERTY(int workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)
    Q_PROPERTY(bool showFps READ showFps WRITE setShowFps NOTIFY showFpsChanged)
public:
    explicit ZServer(uint32_t shmBytes, QObject *parent = nullptr);

    QList<QObject *> clients() const;
    QObject *model() { return &m_model; }
    QObject *focusedClient() const { return m_focused; }
    ZClient *focused() const { return m_focused; }
    int workspace() const { return m_workspace; }
    void setWorkspace(int w);

    Q_INVOKABLE int spawn(const QString &path);
    // Shell.qml's xdg-shell: a new toplevel becomes a window
    Q_INVOKABLE void addWayland(QObject *xdgSurface);
    // start a program that is not a Zerp client (a Wayland one): a shell
    // command line, the session's environment
    Q_INVOKABLE bool run(const QString &command);
    Q_INVOKABLE void logout() { QCoreApplication::quit(); }   // the session ends: wynlogin shows the login screen
    Q_INVOKABLE void focus(QObject *client);
    Q_INVOKABLE void closeFocused();
    Q_INVOKABLE void moveFocusedTo(int workspace);
    Q_INVOKABLE void toggleFullscreen();
    Q_INVOKABLE void focusDirection(int dx, int dy);   // QML layout picks; see Shell.qml
    Q_INVOKABLE int countOn(int workspace) const;
    bool showFps() const { return m_showFps; }
    void setShowFps(bool s) { if (s != m_showFps) { m_showFps = s; emit showFpsChanged(); } }

signals:
    void clientsChanged();
    void layoutChanged();   // a window went full screen or back: tiles move
    void focusChanged();
    void workspaceChanged();
    void focusDirectionRequested(int dx, int dy);
    void showFpsChanged();

private:
    void pumpAll();
    void reap();
    void refocus();
    void remove(const QVector<ZClient *> &gone, const char *why);

    uint32_t m_shmBytes;
    int m_nextId = 1;
    int m_workspace = 1;
    bool m_showFps = false;
    QVector<ZClient *> m_clients;
    ZClientModel m_model;
    ZClient *m_focused = nullptr;
    QTimer m_pump, m_reaper;
};

// One client's pixels as a QML item; forwards the pointer to the client.
class ZSurface : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *client READ client WRITE setClient NOTIFY clientChanged)

public:
    ZSurface();
    QObject *client() const { return m_client; }
    void setClient(QObject *c);

signals:
    void clientChanged();
    void pressed();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void hoverMoveEvent(QHoverEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    void forward(const QPointF &p, Qt::MouseButtons b, bool motion);
    ZClient *m_client = nullptr;
};
