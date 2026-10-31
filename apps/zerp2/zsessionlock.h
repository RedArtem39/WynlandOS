// WynlandOS - Zerp 2.0: the session lock (ext_session_lock_manager_v1).
//
// A lock screen (xvlet's, swaylock, hyprlock, Quickshell's WlSessionLock)
// asks the compositor to lock the session; from then on the compositor,
// not the lock screen, keeps it locked:
//   - nothing is drawn but the lock surface, over black;
//   - the keys and the pointer go to the lock surface only: no other
//     window gets the focus, Zerp's own key bindings and global shortcuts
//     do nothing;
//   - when the lock screen dies without unlocking (a crash, a kill), the
//     session STAYS locked: black, and Zerp's own password field, checked
//     by /usr/lib/wynland/chkpwd; a new lock screen may take over.
// Only unlock_and_destroy from the live lock screen unlocks.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSize>
#include <QtWaylandCompositor/QWaylandCompositorExtension>
#include <QtWaylandCompositor/QWaylandQuickExtension>
#include <QtWaylandCompositor/QWaylandSurface>

#include "qwayland-server-ext-session-lock-v1.h"

class ZSessionLock;
class ZLock;

// the lock screen's surface (one output here)
class ZLockSurface : public QObject, public QtWaylandServer::ext_session_lock_surface_v1
{
    Q_OBJECT
    Q_PROPERTY(QWaylandSurface *surface READ surface CONSTANT)
    Q_PROPERTY(bool mapped READ mapped NOTIFY changed)
public:
    ZLockSurface(ZSessionLock *mgr, QWaylandSurface *surface, wl_client *client, uint32_t id, int version);
    ~ZLockSurface() override;
    QWaylandSurface *surface() const { return m_surface; }
    bool mapped() const { return m_mapped; }
    void configure(QSize size);

signals:
    void changed();

protected:
    void ext_session_lock_surface_v1_ack_configure(Resource *, uint32_t serial) override;
    void ext_session_lock_surface_v1_destroy(Resource *resource) override;
    void ext_session_lock_surface_v1_destroy_resource(Resource *) override;

private:
    void committed();
    ZSessionLock *m_mgr;
    QPointer<QWaylandSurface> m_surface;
    uint32_t m_serial = 0;
    bool m_acked = false, m_mapped = false;
};

// one lock request (ext_session_lock_v1)
class ZLock : public QObject, public QtWaylandServer::ext_session_lock_v1
{
    Q_OBJECT
public:
    ZLock(ZSessionLock *mgr, wl_client *client, uint32_t id, int version, bool granted);
    ~ZLock() override;
    bool granted() const { return m_granted; }

protected:
    void ext_session_lock_v1_get_lock_surface(Resource *resource, uint32_t id, struct ::wl_resource *surface,
                                              struct ::wl_resource *output) override;
    void ext_session_lock_v1_unlock_and_destroy(Resource *resource) override;
    void ext_session_lock_v1_destroy(Resource *resource) override;
    void ext_session_lock_v1_destroy_resource(Resource *) override;

private:
    ZSessionLock *m_mgr;
    bool m_granted, m_unlocked = false;
    QPointer<ZLockSurface> m_surface;
};

class ZSessionLock : public QWaylandCompositorExtensionTemplate<ZSessionLock>,
                     public QtWaylandServer::ext_session_lock_manager_v1
{
    Q_OBJECT
    Q_PROPERTY(bool locked READ locked NOTIFY lockedChanged)
    // the lock screen died with the session locked: Zerp's own way back in
    Q_PROPERTY(bool abandoned READ abandoned NOTIFY lockedChanged)
    Q_PROPERTY(QObject *surface READ surfaceObject NOTIFY surfaceChanged)
    Q_PROPERTY(QSize outputSize READ outputSize WRITE setOutputSize NOTIFY outputSizeChanged)
public:
    ZSessionLock();
    explicit ZSessionLock(QWaylandCompositor *compositor);
    void initialize() override;
    static ZSessionLock *instance();

    bool locked() const { return m_locked; }
    bool abandoned() const { return m_locked && !m_owner; }
    QObject *surfaceObject() const { return m_surface; }
    ZLockSurface *lockSurface() const { return m_surface; }
    QSize outputSize() const { return m_output; }
    void setOutputSize(QSize s);

    // a key for the lock screen, raw (as Zerp gives windows theirs):
    // false when there is no lock screen to take it
    bool sendKey(bool e0, uint8_t scancode, Qt::KeyboardModifiers mods);
    // the abandoned lock: the user's own password (chkpwd) unlocks
    Q_INVOKABLE void tryPassword(const QString &password);

    // for ZLock / ZLockSurface
    void surfaceMade(ZLock *lock, ZLockSurface *s);
    void surfaceGone(ZLockSurface *s);
    void unlock(ZLock *lock);
    void lockGone(ZLock *lock);

signals:
    void lockedChanged();
    void surfaceChanged();
    void outputSizeChanged();
    void passwordChecked(bool ok);

protected:
    void ext_session_lock_manager_v1_lock(Resource *resource, uint32_t id) override;
    void ext_session_lock_manager_v1_destroy(Resource *resource) override;

private:
    void setLocked(bool on);
    void keepFocus();
    bool m_locked = false;
    QPointer<ZLock> m_owner;            // the live lock screen
    QPointer<ZLockSurface> m_surface;
    QSize m_output{ 1280, 720 };
    bool m_checking = false;
};

Q_COMPOSITOR_DECLARE_QUICK_EXTENSION_CLASS(ZSessionLock)

// for the input pump: no bindings, no client keys while locked
bool zerpSessionLocked();
