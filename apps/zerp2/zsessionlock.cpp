// WynlandOS - Zerp 2.0's session lock (see zsessionlock.h)
#include "zsessionlock.h"

#include <QtCore/QProcess>
#include <QtGui/QKeyEvent>
#include <QtWaylandCompositor/QWaylandCompositor>
#include <QtWaylandCompositor/QWaylandSeat>
#include <QtWaylandCompositor/QWaylandSurfaceRole>

#include <cstdio>

static QWaylandSurfaceRole s_role("ext_session_lock_surface_v1");
static ZSessionLock *s_instance = nullptr;

bool zerpSessionLocked() { return s_instance && s_instance->locked(); }

// ---------------------------------------------------------------- surface

ZLockSurface::ZLockSurface(ZSessionLock *mgr, QWaylandSurface *surface, wl_client *client, uint32_t id, int version)
    : QtWaylandServer::ext_session_lock_surface_v1(client, int(id), version), m_mgr(mgr), m_surface(surface)
{
    connect(surface, &QWaylandSurface::redraw, this, &ZLockSurface::committed);
    connect(surface, &QWaylandSurface::hasContentChanged, this, &ZLockSurface::committed);
}

ZLockSurface::~ZLockSurface()
{
    m_mgr->surfaceGone(this);
}

void ZLockSurface::configure(QSize size)
{
    if (!m_surface) return;
    m_serial = m_surface->compositor()->nextSerial();
    send_configure(m_serial, uint32_t(size.width()), uint32_t(size.height()));
}

void ZLockSurface::committed()
{
    const bool on = m_surface && m_acked && m_surface->hasContent();
    if (on != m_mapped) { m_mapped = on; emit changed(); }
}

void ZLockSurface::ext_session_lock_surface_v1_ack_configure(Resource *, uint32_t serial)
{
    if (serial == m_serial) m_acked = true;
}

void ZLockSurface::ext_session_lock_surface_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void ZLockSurface::ext_session_lock_surface_v1_destroy_resource(Resource *)
{
    m_mapped = false;
    emit changed();
    deleteLater();
}

// ---------------------------------------------------------------- one lock

ZLock::ZLock(ZSessionLock *mgr, wl_client *client, uint32_t id, int version, bool granted)
    : QtWaylandServer::ext_session_lock_v1(client, int(id), version), m_mgr(mgr), m_granted(granted)
{
}

ZLock::~ZLock() = default;

void ZLock::ext_session_lock_v1_get_lock_surface(Resource *resource, uint32_t id, struct ::wl_resource *surfaceRes,
                                                 struct ::wl_resource *)
{
    QWaylandSurface *surface = QWaylandSurface::fromResource(surfaceRes);
    if (!surface) return;
    if (m_surface) {
        wl_resource_post_error(resource->handle, 3, "one output: a second lock surface");   // duplicate_output
        return;
    }
    if (!surface->setRole(&s_role, resource->handle, 2)) return;                          // role
    auto *s = new ZLockSurface(m_mgr, surface, resource->client(), id, wl_resource_get_version(resource->handle));
    m_surface = s;
    m_mgr->surfaceMade(this, s);
}

void ZLock::ext_session_lock_v1_unlock_and_destroy(Resource *resource)
{
    if (!m_granted) {
        wl_resource_post_error(resource->handle, 1, "unlock of a lock that was never granted");   // invalid_unlock
        return;
    }
    m_unlocked = true;
    m_mgr->unlock(this);
    wl_resource_destroy(resource->handle);
}

void ZLock::ext_session_lock_v1_destroy(Resource *resource)
{
    // only a refused lock may simply go: a granted one unlocks or stays
    if (m_granted && !m_unlocked) {
        wl_resource_post_error(resource->handle, 0, "destroy of a granted lock: unlock_and_destroy");   // invalid_destroy
        return;
    }
    wl_resource_destroy(resource->handle);
}

void ZLock::ext_session_lock_v1_destroy_resource(Resource *)
{
    if (m_granted && !m_unlocked) m_mgr->lockGone(this);
    deleteLater();
}

// ---------------------------------------------------------------- manager

ZSessionLock::ZSessionLock() : QWaylandCompositorExtensionTemplate<ZSessionLock>() { s_instance = this; }
ZSessionLock::ZSessionLock(QWaylandCompositor *compositor)
    : QWaylandCompositorExtensionTemplate<ZSessionLock>(compositor) { s_instance = this; }

ZSessionLock *ZSessionLock::instance() { return s_instance; }

void ZSessionLock::initialize()
{
    QWaylandCompositorExtensionTemplate::initialize();
    auto *compositor = static_cast<QWaylandCompositor *>(extensionContainer());
    if (!compositor) {
        fprintf(stderr, "[zerp2] session lock: no compositor\n");
        return;
    }
    init(compositor->display(), 1);
    // nobody else gets the keyboard while locked
    if (QWaylandSeat *seat = compositor->defaultSeat())
        connect(seat, &QWaylandSeat::keyboardFocusChanged, this, [this] { keepFocus(); });
}

void ZSessionLock::keepFocus()
{
    if (!m_locked) return;
    auto *compositor = static_cast<QWaylandCompositor *>(extensionContainer());
    QWaylandSeat *seat = compositor ? compositor->defaultSeat() : nullptr;
    if (!seat) return;
    QWaylandSurface *want = m_surface && m_surface->mapped() ? m_surface->surface() : nullptr;
    if (seat->keyboardFocus() != want) seat->setKeyboardFocus(want);
}

void ZSessionLock::setLocked(bool on)
{
    if (m_locked == on) { emit lockedChanged(); return; }
    m_locked = on;
    fprintf(stderr, "[zerp2] session %s\n", on ? "locked" : "unlocked");
    emit lockedChanged();
    keepFocus();
}

void ZSessionLock::setOutputSize(QSize s)
{
    if (s == m_output || s.isEmpty()) return;
    m_output = s;
    emit outputSizeChanged();
    if (m_surface) m_surface->configure(m_output);
}

void ZSessionLock::ext_session_lock_manager_v1_lock(Resource *resource, uint32_t id)
{
    // a live lock screen holds it: the newcomer is refused (finished);
    // unlocked, or its lock screen gone: the newcomer has it
    const bool granted = !m_owner;
    auto *l = new ZLock(this, resource->client(), id, wl_resource_get_version(resource->handle), granted);
    if (!granted) { l->send_finished(); return; }
    m_owner = l;
    setLocked(true);
    l->send_locked();
}

void ZSessionLock::ext_session_lock_manager_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void ZSessionLock::surfaceMade(ZLock *lock, ZLockSurface *s)
{
    if (lock != m_owner) return;
    m_surface = s;
    connect(s, &ZLockSurface::changed, this, [this] { keepFocus(); });
    s->configure(m_output);
    emit surfaceChanged();
}

void ZSessionLock::surfaceGone(ZLockSurface *s)
{
    if (m_surface == s || !m_surface) { m_surface = nullptr; emit surfaceChanged(); keepFocus(); }
}

void ZSessionLock::unlock(ZLock *lock)
{
    if (lock != m_owner) return;
    m_owner = nullptr;
    m_surface = nullptr;
    emit surfaceChanged();
    setLocked(false);
}

void ZSessionLock::lockGone(ZLock *lock)
{
    // the lock screen died (or was killed) locked: the session stays so
    if (lock != m_owner) return;
    fprintf(stderr, "[zerp2] session lock: the lock screen is gone -- still locked\n");
    m_owner = nullptr;
    m_surface = nullptr;
    emit surfaceChanged();
    setLocked(true);
}

int zerpEvdevKey(bool e0, int code);   // zerp2.cpp

bool ZSessionLock::sendKey(bool e0, uint8_t scancode, Qt::KeyboardModifiers mods)
{
    if (!m_locked || !m_surface || !m_surface->mapped() || !m_surface->surface()) return false;
    QWaylandSurface *surf = m_surface->surface();
    QWaylandSeat *seat = surf->compositor() ? surf->compositor()->defaultSeat() : nullptr;
    const int key = zerpEvdevKey(e0, scancode & 0x7F);
    if (!seat || !key) return true;
    if (seat->keyboardFocus() != surf) seat->setKeyboardFocus(surf);
    // xkb codes are Linux's + 8; a full event brings the modifiers along
    QKeyEvent ke((scancode & 0x80) ? QEvent::KeyRelease : QEvent::KeyPress, Qt::Key_unknown, mods, quint32(key + 8), 0, 0);
    seat->sendFullKeyEvent(&ke);
    return true;
}

void ZSessionLock::tryPassword(const QString &password)
{
    if (!abandoned() || m_checking) return;
    m_checking = true;
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        m_checking = false;
        p->deleteLater();
        const bool ok = st == QProcess::NormalExit && code == 0;
        emit passwordChecked(ok);
        if (ok && abandoned()) setLocked(false);
    });
    p->start(QStringLiteral("/usr/lib/wynland/chkpwd"), { qEnvironmentVariable("USER") });
    p->write(password.toUtf8() + '\n');
    p->closeWriteChannel();
}
