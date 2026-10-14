// WynlandOS - Zerp 2.0's Hyprland IPC (see hypripc.h)
#include "hypripc.h"
#include "zerp2.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <algorithm>
#include <cstdio>
#include <unistd.h>

QPoint HyprIpc::s_cursor;

HyprIpc::HyprIpc(ZServer *server, const QString &monitorName, QSize monitorSize, QObject *parent)
    : QObject(parent), m_srv(server), m_monitor(monitorName), m_size(monitorSize)
{
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) runtime = QStringLiteral("/tmp/runtime-%1").arg(getuid());
    const QString sig = QStringLiteral("zerp_%1").arg(QCoreApplication::applicationPid());
    m_dir = runtime + QStringLiteral("/hypr/") + sig;
    QDir().mkpath(m_dir);
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    m_req = new QLocalServer(this);
    m_evt = new QLocalServer(this);
    m_req->setSocketOptions(QLocalServer::UserAccessOption);
    m_evt->setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(m_dir + QStringLiteral("/.socket.sock"));
    QLocalServer::removeServer(m_dir + QStringLiteral("/.socket2.sock"));
    if (!m_req->listen(m_dir + QStringLiteral("/.socket.sock")) ||
        !m_evt->listen(m_dir + QStringLiteral("/.socket2.sock"))) {
        fprintf(stderr, "[zerp2] hyprland ipc: %s\n", qPrintable(m_req->errorString()));
        return;
    }
    // the session's programs find it (they inherit our environment)
    qputenv("HYPRLAND_INSTANCE_SIGNATURE", sig.toUtf8());
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    if (qEnvironmentVariableIsEmpty("XDG_CURRENT_DESKTOP")) qputenv("XDG_CURRENT_DESKTOP", "Hyprland");

    connect(m_req, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_req->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                // one request per connection; the client writes it whole
                const QByteArray req = s->readAll();
                s->write(request(req));
                s->flush();
                s->disconnectFromServer();
            });
        }
    });
    connect(m_evt, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *s = m_evt->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            m_listeners.append(s);
        }
    });

    connect(m_srv, &ZServer::clientsChanged, this, &HyprIpc::scheduleSync);
    connect(m_srv, &ZServer::focusChanged, this, &HyprIpc::scheduleSync);
    connect(m_srv, &ZServer::workspaceChanged, this, &HyprIpc::scheduleSync);
    connect(m_srv, &ZServer::layoutChanged, this, &HyprIpc::scheduleSync);
    m_seenActive = m_srv->workspace();
    m_seenWs = workspaces();
    fprintf(stderr, "[zerp2] hyprland ipc: %s\n", qPrintable(m_dir));
}

HyprIpc::~HyprIpc()
{
    QDir(m_dir).removeRecursively();
}

bool HyprIpc::listening() const { return m_req && m_req->isListening(); }

// ---------------------------------------------------------------- state

QString HyprIpc::address(const ZClient *c) { return QStringLiteral("0x%1").arg(0x5a7e0000 + c->id(), 0, 16); }
QByteArray HyprIpc::addr(const ZClient *c) { return QByteArray::number(0x5a7e0000 + c->id(), 16); }

QList<int> HyprIpc::workspaces() const
{
    // like Hyprland: the active one and every one with windows
    QList<int> ws{ m_srv->workspace() };
    for (QObject *o : m_srv->clients())
        if (auto *c = qobject_cast<ZClient *>(o); c && !ws.contains(c->workspace())) ws.append(c->workspace());
    std::sort(ws.begin(), ws.end());
    return ws;
}

QJsonObject HyprIpc::clientJson(ZClient *c) const
{
    const QRect g = c->geometry();
    const int focusRank = c == m_srv->focused() ? 0 : 1;
    return QJsonObject{
        { "address", address(c) }, { "mapped", true }, { "hidden", c->workspace() != m_srv->workspace() },
        { "at", QJsonArray{ g.x(), g.y() } }, { "size", QJsonArray{ g.width(), g.height() } },
        { "workspace", QJsonObject{ { "id", c->workspace() }, { "name", QString::number(c->workspace()) } } },
        { "floating", false }, { "pseudo", false }, { "monitor", 0 },
        { "class", c->appId() }, { "title", c->title() },
        { "initialClass", c->appId() }, { "initialTitle", c->title() },
        { "pid", (qint64)c->pid() }, { "xwayland", false }, { "pinned", false },
        { "fullscreen", c->fullscreen() ? 2 : 0 }, { "fullscreenClient", c->fullscreen() ? 2 : 0 },
        { "grouped", QJsonArray() }, { "tags", QJsonArray() }, { "swallowing", "0x0" },
        { "focusHistoryID", focusRank }, { "inhibitingIdle", false },
        { "xdgTag", "" }, { "xdgDescription", "" }, { "contentType", "none" },
    };
}

QJsonObject HyprIpc::workspaceJson(int ws) const
{
    int windows = 0;
    bool full = false;
    ZClient *last = nullptr;
    for (QObject *o : m_srv->clients()) {
        auto *c = qobject_cast<ZClient *>(o);
        if (!c || c->workspace() != ws) continue;
        windows++;
        full = full || c->fullscreen();
        last = c;
    }
    if (m_srv->focused() && m_srv->focused()->workspace() == ws) last = m_srv->focused();
    return QJsonObject{
        { "id", ws }, { "name", QString::number(ws) }, { "monitor", m_monitor }, { "monitorID", 0 },
        { "windows", windows }, { "hasfullscreen", full },
        { "lastwindow", last ? address(last) : QStringLiteral("0x0") },
        { "lastwindowtitle", last ? last->title() : QString() }, { "ispersistent", false },
    };
}

QJsonObject HyprIpc::monitorJson() const
{
    const int ws = m_srv->workspace();
    return QJsonObject{
        { "id", 0 }, { "name", m_monitor }, { "description", "WynlandOS " + m_monitor },
        { "make", "WynlandOS" }, { "model", m_monitor }, { "serial", "" },
        { "width", m_size.width() }, { "height", m_size.height() }, { "refreshRate", 60.0 },
        { "x", 0 }, { "y", 0 },
        { "activeWorkspace", QJsonObject{ { "id", ws }, { "name", QString::number(ws) } } },
        { "specialWorkspace", QJsonObject{ { "id", 0 }, { "name", "" } } },
        { "reserved", QJsonArray{ 0, 0, 0, 0 } }, { "scale", 1.0 }, { "transform", 0 },
        { "focused", true }, { "dpmsStatus", true }, { "vrr", false }, { "solitary", "0" },
        { "activelyTearing", false }, { "disabled", false }, { "currentFormat", "XRGB8888" },
        { "mirrorOf", "none" }, { "availableModes", QJsonArray{ QStringLiteral("%1x%2@60.00Hz").arg(m_size.width()).arg(m_size.height()) } },
    };
}

ZClient *HyprIpc::byAddress(const QByteArray &spec) const
{
    // "address:0x5a7e0003" (Caelestia writes "0x" + an address that has one: tolerated)
    QByteArray a = spec.trimmed();
    if (a.startsWith("address:")) a = a.mid(8);
    while (a.startsWith("0x")) a = a.mid(2);
    bool ok = false;
    const qulonglong v = a.toULongLong(&ok, 16);
    if (!ok) return nullptr;
    for (QObject *o : m_srv->clients())
        if (auto *c = qobject_cast<ZClient *>(o); c && (qulonglong)(0x5a7e0000 + c->id()) == v) return c;
    return nullptr;
}

// ---------------------------------------------------------------- requests

QByteArray HyprIpc::request(const QByteArray &raw)
{
    QByteArray req = raw.trimmed();
    if (req.startsWith("[[BATCH]]")) {
        QByteArray out;
        for (const QByteArray &one : req.mid(9).split(';')) {
            if (one.trimmed().isEmpty()) continue;
            out += request(one);
            out += "\n\n";
        }
        return out;
    }
    // flags before the slash: j (json), a, r ... ("j/clients")
    bool json = false;
    if (req.startsWith('/')) req = req.mid(1);
    else if (const int slash = req.indexOf('/'); slash > 0 && slash < 4 && !req.left(slash).contains(' ')) {
        json = req.left(slash).contains('j');
        req = req.mid(slash + 1);
    }
    return command(req, json);
}

QByteArray HyprIpc::command(QByteArray cmd, bool json)
{
    const int sp = cmd.indexOf(' ');
    const QByteArray name = sp < 0 ? cmd : cmd.left(sp);
    const QByteArray args = sp < 0 ? QByteArray() : cmd.mid(sp + 1).trimmed();
    auto out = [](const QJsonDocument &d) { return d.toJson(QJsonDocument::Compact); };

    if (name == "dispatch") return dispatch(args);
    if (name == "keyword" || name == "reload" || name == "switchxkblayout" || name == "setcursor" ||
        name == "notify" || name == "dismissnotify" || name == "seterror" || name == "eval")
        return "ok";

    if (name == "clients") {
        QJsonArray a;
        for (QObject *o : m_srv->clients()) if (auto *c = qobject_cast<ZClient *>(o)) a.append(clientJson(c));
        if (json) return out(QJsonDocument(a));
        QByteArray t;
        for (const auto &v : a) {
            const QJsonObject c = v.toObject();
            t += "Window " + c["address"].toString().mid(2).toUtf8() + " -> " + c["title"].toString().toUtf8() + ":\n";
            t += "\tworkspace: " + QByteArray::number(c["workspace"].toObject()["id"].toInt()) + "\n\tclass: " +
                 c["class"].toString().toUtf8() + "\n\ttitle: " + c["title"].toString().toUtf8() + "\n\n";
        }
        return t;
    }
    if (name == "activewindow") {
        ZClient *c = m_srv->focused();
        if (json) return out(QJsonDocument(c ? clientJson(c) : QJsonObject()));
        return c ? "Window " + addr(c) + " -> " + c->title().toUtf8() + "\n" : "Invalid\n";
    }
    if (name == "workspaces") {
        QJsonArray a;
        for (int w : workspaces()) a.append(workspaceJson(w));
        if (json) return out(QJsonDocument(a));
        QByteArray t;
        for (int w : workspaces()) t += "workspace ID " + QByteArray::number(w) + " (" + QByteArray::number(w) + ") on monitor " + m_monitor.toUtf8() + ":\n\n";
        return t;
    }
    if (name == "activeworkspace") {
        const QJsonObject w = workspaceJson(m_srv->workspace());
        if (json) return out(QJsonDocument(w));
        return "workspace ID " + QByteArray::number(m_srv->workspace()) + "\n";
    }
    if (name == "monitors") {
        if (json) return out(QJsonDocument(QJsonArray{ monitorJson() }));
        return "Monitor " + m_monitor.toUtf8() + " (ID 0):\n\t" + QByteArray::number(m_size.width()) + "x" +
               QByteArray::number(m_size.height()) + "@60.00000 at 0x0\n";
    }
    if (name == "devices") {
        const QJsonObject kb{ { "address", "0x1" }, { "name", "at-translated-set-2-keyboard" }, { "rules", "" },
                              { "model", "" }, { "layout", "us" }, { "variant", "" }, { "options", "" },
                              { "active_keymap", "English (US)" }, { "capsLock", false }, { "numLock", false },
                              { "main", true } };
        const QJsonObject mouse{ { "address", "0x2" }, { "name", "ps2-mouse" }, { "defaultSpeed", 0.0 } };
        return out(QJsonDocument(QJsonObject{ { "mice", QJsonArray{ mouse } }, { "keyboards", QJsonArray{ kb } },
                                              { "tablets", QJsonArray() }, { "touch", QJsonArray() },
                                              { "switches", QJsonArray() } }));
    }
    if (name == "cursorpos") {
        if (json) return out(QJsonDocument(QJsonObject{ { "x", s_cursor.x() }, { "y", s_cursor.y() } }));
        return QByteArray::number(s_cursor.x()) + ", " + QByteArray::number(s_cursor.y()) + "\n";
    }
    if (name == "getoption") {
        QJsonObject o{ { "option", QString::fromUtf8(args) }, { "set", false } };
        if (args == "input:kb_layout") { o["str"] = "us"; o["set"] = true; }
        else o["int"] = 0;
        return out(QJsonDocument(o));
    }
    if (name == "version") {
        return json ? out(QJsonDocument(QJsonObject{ { "branch", "zerp" }, { "commit", "zerp2" }, { "version", "0.50.0" },
                                                     { "tag", "v0.50.0-zerp" }, { "dirty", false }, { "flags", QJsonArray() } }))
                    : QByteArray("Hyprland IPC of Zerp 2.0 (WynlandOS)\n");
    }
    if (name == "status") return out(QJsonDocument(QJsonObject{ { "configProvider", "hyprlang" } }));
    if (name == "layers" || name == "binds" || name == "workspacerules" || name == "descriptions" ||
        name == "globalshortcuts" || name == "animations" || name == "layouts")
        return json ? "[]" : "";
    if (name == "splash") return "Zerp\n";
    return "unknown request";
}

QByteArray HyprIpc::dispatch(const QByteArray &line)
{
    const int sp = line.indexOf(' ');
    const QByteArray d = sp < 0 ? line : line.left(sp);
    const QByteArray arg = sp < 0 ? QByteArray() : line.mid(sp + 1).trimmed();

    // a workspace spec: N, +1/-1, r+1/r-1, e+1/e-1, name:N, previous
    auto target = [this](QByteArray s) -> int {
        const int cur = m_srv->workspace();
        if (s.startsWith("name:")) s = s.mid(5);
        if (s.startsWith("special")) return 0;
        if (s.startsWith("r") || s.startsWith("e") || s.startsWith("m")) s = s.mid(1);
        bool ok = false;
        int n = s.toInt(&ok);
        if (!ok) return 0;
        if (s.startsWith('+') || s.startsWith('-')) n = cur + n;
        if (n < 1) n = 9 + (n % 9 == 0 ? 0 : n % 9);
        if (n > 9) n = (n - 1) % 9 + 1;
        return n;
    };

    if (d == "workspace") {
        if (const int w = target(arg)) m_srv->setWorkspace(w);
        return "ok";
    }
    if (d == "movetoworkspace" || d == "movetoworkspacesilent") {
        const QList<QByteArray> parts = arg.split(',');
        const int w = target(parts.value(0));
        ZClient *c = parts.size() > 1 ? byAddress(parts[1]) : m_srv->focused();
        if (!w || !c) return "ok";
        if (c->workspace() != w) {
            c->setWorkspace(w);
            emit m_srv->clientsChanged();
        }
        if (d == "movetoworkspace") { m_srv->setWorkspace(w); m_srv->focus(c); }
        else if (c == m_srv->focused()) m_srv->focus(nullptr);
        return "ok";
    }
    if (d == "killactive" || d == "closewindow" || d == "killwindow" || d == "forcekillactive") {
        ZClient *c = arg.isEmpty() ? m_srv->focused() : byAddress(arg);
        if (c) c->close();
        return "ok";
    }
    if (d == "focuswindow") {
        if (ZClient *c = byAddress(arg)) { m_srv->setWorkspace(c->workspace()); m_srv->focus(c); }
        return "ok";
    }
    if (d == "fullscreen") { m_srv->toggleFullscreen(); return "ok"; }
    if (d == "movefocus") {
        const char dir = arg.isEmpty() ? 'r' : arg[0];
        m_srv->focusDirection(dir == 'l' ? -1 : dir == 'r' ? 1 : 0, dir == 'u' ? -1 : dir == 'd' ? 1 : 0);
        return "ok";
    }
    if (d == "exec") { m_srv->run(QString::fromUtf8(arg)); return "ok"; }
    if (d == "exit") { m_srv->logout(); return "ok"; }
    // no counterpart in Zerp: special workspaces, floating, pinning, dpms, global shortcuts
    return "ok";
}

// ---------------------------------------------------------------- events

void HyprIpc::event(const QByteArray &name, const QByteArray &data)
{
    const QByteArray line = name + ">>" + data + "\n";
    for (int i = m_listeners.size() - 1; i >= 0; i--) {
        QLocalSocket *s = m_listeners[i];
        if (!s || s->state() != QLocalSocket::ConnectedState) { m_listeners.removeAt(i); continue; }
        s->write(line);
        s->flush();
    }
}

// Tell the listeners what changed since the last time, in Hyprland's order:
// workspaces created, windows opened/moved/retitled, the active workspace
// and window, windows closed, workspaces destroyed.
void HyprIpc::sync()
{
    m_syncPending = false;
    QList<ZClient *> now;
    for (QObject *o : m_srv->clients()) if (auto *c = qobject_cast<ZClient *>(o)) now.append(c);
    for (ZClient *c : now)
        if (!m_hooked.contains(c)) {
            m_hooked.insert(c);
            connect(c, &ZClient::titleChanged, this, &HyprIpc::scheduleSync);
            connect(c, &ZClient::workspaceChanged, this, &HyprIpc::scheduleSync);
            connect(c, &ZClient::fullscreenChanged, this, &HyprIpc::scheduleSync);
            connect(c, &QObject::destroyed, this, [this, c] { m_hooked.remove(c); });
        }

    const QList<int> ws = workspaces();
    for (int w : ws)
        if (!m_seenWs.contains(w)) {
            event("createworkspace", QByteArray::number(w));
            event("createworkspacev2", QByteArray::number(w) + "," + QByteArray::number(w));
        }

    for (ZClient *c : now) {
        const QByteArray a = addr(c), w = QByteArray::number(c->workspace());
        auto it = m_seen.find(c);
        if (it == m_seen.end()) {
            event("openwindow", a + "," + w + "," + c->appId().toUtf8() + "," + c->title().toUtf8());
            m_seen.insert(c, { c->workspace(), c->title(), c->fullscreen(), a });
            continue;
        }
        if (it->ws != c->workspace()) {
            event("movewindow", a + "," + w);
            event("movewindowv2", a + "," + w + "," + w);
            it->ws = c->workspace();
        }
        if (it->title != c->title()) {
            event("windowtitle", a);
            event("windowtitlev2", a + "," + c->title().toUtf8());
            it->title = c->title();
        }
        if (it->fullscreen != c->fullscreen()) {
            event("fullscreen", c->fullscreen() ? "1" : "0");
            it->fullscreen = c->fullscreen();
        }
    }

    const int active = m_srv->workspace();
    if (active != m_seenActive) {
        const QByteArray w = QByteArray::number(active);
        event("workspace", w);
        event("workspacev2", w + "," + w);
        event("focusedmon", m_monitor.toUtf8() + "," + w);
        event("focusedmonv2", m_monitor.toUtf8() + "," + w);
        m_seenActive = active;
    }

    ZClient *f = m_srv->focused();
    if (!m_seenFocusValid || f != m_seenFocus) {
        event("activewindow", f ? f->appId().toUtf8() + "," + f->title().toUtf8() : QByteArray(","));
        event("activewindowv2", f ? addr(f) : QByteArray());
        m_seenFocus = f;
        m_seenFocusValid = true;
    }

    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (!now.contains(it.key())) {
            event("closewindow", it->addr);
            it = m_seen.erase(it);
        } else {
            ++it;
        }
    }

    for (int w : m_seenWs)
        if (!ws.contains(w)) {
            event("destroyworkspace", QByteArray::number(w));
            event("destroyworkspacev2", QByteArray::number(w) + "," + QByteArray::number(w));
        }
    m_seenWs = ws;
}
