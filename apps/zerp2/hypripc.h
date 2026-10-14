// WynlandOS - Zerp 2.0 speaks Hyprland's IPC, so shells written for
// Hyprland (Quickshell's Quickshell.Hyprland, Caelestia, hyprctl) see our
// windows and workspaces.
//
// Two Unix sockets in $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/:
//  - .socket.sock: one request per connection ("j/clients", "dispatch
//    workspace 2", "[[BATCH]]a;b"), the answer, then the close;
//  - .socket2.sock: events, "name>>data\n", to every listener.
// What it reports follows Hyprland's JSON closely enough for those; what
// Zerp has no counterpart for (special workspaces, floating, pinning,
// layouts) is accepted and does nothing.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QTimer>

class QLocalServer;
class QLocalSocket;
class ZServer;
class ZClient;

class HyprIpc : public QObject
{
    Q_OBJECT
public:
    HyprIpc(ZServer *server, const QString &monitorName, QSize monitorSize, QObject *parent = nullptr);
    ~HyprIpc() override;
    bool listening() const;
    static void setCursor(QPoint p) { s_cursor = p; }

private:
    QByteArray request(const QByteArray &req);
    QByteArray command(QByteArray cmd, bool json);
    QByteArray dispatch(const QByteArray &args);

    QJsonObject clientJson(ZClient *c) const;
    QJsonObject workspaceJson(int ws) const;
    QJsonObject monitorJson() const;
    QList<int> workspaces() const;
    ZClient *byAddress(const QByteArray &spec) const;
    static QString address(const ZClient *c);   // "0x..." as in JSON
    static QByteArray addr(const ZClient *c);   // without 0x, as in events

    void event(const QByteArray &name, const QByteArray &data);
    void scheduleSync() { if (!m_syncPending) { m_syncPending = true; QTimer::singleShot(0, this, &HyprIpc::sync); } }
    void sync();

    ZServer *m_srv;
    QString m_monitor;
    QSize m_size;
    QString m_dir;
    QLocalServer *m_req = nullptr, *m_evt = nullptr;
    QList<QPointer<QLocalSocket>> m_listeners;
    bool m_syncPending = false;

    // what the listeners were last told
    struct Seen { int ws; QString title; bool fullscreen; QByteArray addr; };   // addr: the client may be gone
    QHash<ZClient *, Seen> m_seen;
    QSet<ZClient *> m_hooked;
    QList<int> m_seenWs;
    int m_seenActive = 0;
    ZClient *m_seenFocus = nullptr;
    bool m_seenFocusValid = false;

    static QPoint s_cursor;
};
