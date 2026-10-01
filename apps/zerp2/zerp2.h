// WynlandOS - Zerp 2.0: client hosting (Zerp protocol) for the Qt Quick WM.
//
// A client is an ordinary Zerp client process (zerp_term, zerp_files, the
// Qt apps through the qwynlandfb plugin): spawned with a c2s pipe, an s2c
// pipe and a shared-memory pixel buffer (argv[1..3]), it renders into the
// buffer at the size of its TILE_RECT and reports DAMAGE. Here every
// client is a QML item (ZSurface) showing that buffer as a texture; QML
// owns the layout (tiling, workspaces, animations) and tells each client
// its size.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QVector>
#include <QtQuick/QQuickItem>
#include <cstdint>

extern "C" {
#include "../../zerp_protocol.h"
}

class ZClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int id READ id CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString appId READ appId CONSTANT)
    Q_PROPERTY(int workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)
    Q_PROPERTY(bool focused READ focused NOTIFY focusedChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged)
public:
    ZClient(int id, const QString &path, QObject *parent);
    ~ZClient() override;

    bool start(uint32_t shmBytes);
    int id() const { return m_id; }
    QString title() const { return m_title; }
    QString appId() const { return m_appId; }
    int workspace() const { return m_workspace; }
    void setWorkspace(int w) { if (w != m_workspace) { m_workspace = w; emit workspaceChanged(); } }
    bool focused() const { return m_focused; }
    void setFocused(bool f) { if (f != m_focused) { m_focused = f; emit focusedChanged(); } }
    bool fullscreen() const { return m_fullscreen; }
    void setFullscreen(bool f) { if (f != m_fullscreen) { m_fullscreen = f; emit fullscreenChanged(); } }

    // QML: the size the client should render at (the layout's target,
    // not the animated in-between sizes)
    Q_INVOKABLE void configure(int x, int y, int w, int h);
    Q_INVOKABLE void close();

    void sendMouse(int x, int y, uint32_t buttons, bool motion);
    void sendKey(bool e0, uint8_t scancode);
    // drains c2s; returns false once the client is gone
    bool pump(QVector<QString> *spawnRequests);
    bool alive() const;

    // pixels, for ZSurface (render thread, GUI thread blocked)
    const uint32_t *pixels() const { return m_shm; }
    int bufWidth() const { return m_bufW; }
    int bufHeight() const { return m_bufH; }
    bool takeDirty() { bool d = m_dirty; m_dirty = false; return d; }

signals:
    void workspaceChanged();
    void focusedChanged();
    void fullscreenChanged();
    void damaged();

private:
    void send(const ZerpMsg &m, bool motion = false);
    void flush();

    int m_id;
    QString m_path, m_title, m_appId;
    int m_workspace = 1;
    bool m_focused = false, m_fullscreen = false;
    int m_c2s = -1, m_s2c = -1;
    long m_pid = -1;
    uint32_t *m_shm = nullptr;
    uint32_t m_shmBytes = 0;
    int m_bufW = 0, m_bufH = 0;      // size the client renders at
    bool m_dirty = false;
    QByteArray m_rx;
    QVector<ZerpMsg> m_out;          // waiting for room in the s2c pipe
    bool m_outTailMotion = false;
};

class ZServer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<QObject *> clients READ clients NOTIFY clientsChanged)
    Q_PROPERTY(QObject *focusedClient READ focusedClient NOTIFY focusChanged)
    Q_PROPERTY(int workspace READ workspace WRITE setWorkspace NOTIFY workspaceChanged)
public:
    explicit ZServer(uint32_t shmBytes, QObject *parent = nullptr);

    QList<QObject *> clients() const;
    QObject *focusedClient() const { return m_focused; }
    ZClient *focused() const { return m_focused; }
    int workspace() const { return m_workspace; }
    void setWorkspace(int w);

    Q_INVOKABLE int spawn(const QString &path);
    Q_INVOKABLE void focus(QObject *client);
    Q_INVOKABLE void closeFocused();
    Q_INVOKABLE void moveFocusedTo(int workspace);
    Q_INVOKABLE void toggleFullscreen();
    Q_INVOKABLE void focusDirection(int dx, int dy);   // QML layout picks; see Shell.qml
    Q_INVOKABLE int countOn(int workspace) const;

signals:
    void clientsChanged();
    void focusChanged();
    void workspaceChanged();
    void focusDirectionRequested(int dx, int dy);

private:
    void pumpAll();
    void reap();
    void refocus();

    uint32_t m_shmBytes;
    int m_nextId = 1;
    int m_workspace = 1;
    QVector<ZClient *> m_clients;
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

private:
    void forward(const QPointF &p, Qt::MouseButtons b, bool motion);
    ZClient *m_client = nullptr;
    QSize m_texSize;
};
