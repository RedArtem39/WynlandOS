// WynlandOS - Zerp 2.0: the pointer for games -- relative motion, locks,
// confinement, no cursor.
//
// A game's mouse look (Doom, anything where you look around) wants the
// mouse's own motion, not where the pointer is: at the screen's edge the
// pointer stops but the hand goes on. Wayland's answer is two protocols
// (wayland-protocols, unstable):
//
//  - zwp_relative_pointer_manager_v1: each packet's motion (dx, dy) to the
//    client the pointer is over, beside the ordinary wl_pointer motion;
//  - zwp_pointer_constraints_v1: the client's surface may lock the pointer
//    (it stays put -- only the motion is told) or confine it to the
//    surface, while that surface has the pointer and the keyboard. Taking
//    the keyboard elsewhere (a Super binding, the session lock) lets go.
//
// The kernel does the pointer itself (drivers/input/mouse.c): the events
// of SYS_mouse_events carry dx/dy, and SYS_pointer_ctl locks, confines,
// moves and hides it. And a client asking for no cursor (wl_pointer.
// set_cursor with no surface: SDL's relative mode, a video player) gets
// the cursor plane off, until the pointer goes to another surface.
#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QRect>

#include "qwayland-server-relative-pointer-unstable-v1.h"
#include "qwayland-server-pointer-constraints-unstable-v1.h"

class QWaylandCompositor;
class QWaylandSeat;
class QWaylandSurface;
class QWaylandView;
class ZPointer;

class ZRelativePointer : public QtWaylandServer::zwp_relative_pointer_v1
{
public:
    ZRelativePointer(ZPointer *z, wl_client *client, uint32_t id, int version, struct ::wl_resource *pointer);
    ~ZRelativePointer();
    wl_client *client() const { return m_client; }
    // the motion, then its wl_pointer's frame (SDL 3.4 hands the motion on
    // at the frame: without one, a game's view never turns)
    void motion(uint32_t hi, uint32_t lo, int dx, int dy);

protected:
    void zwp_relative_pointer_v1_destroy(Resource *resource) override;
    void zwp_relative_pointer_v1_destroy_resource(Resource *resource) override;

private:
    ZPointer *m_z;
    wl_client *m_client;
    struct ::wl_resource *m_pointer;   // (null once the client destroyed it)
    struct Gone { struct ::wl_listener l; ZRelativePointer *self; } m_gone;
};

// a lock or a confinement (the two are alike but for what they do)
class ZConstraint
{
public:
    virtual ~ZConstraint() = default;
    bool lock = false;
    bool persistent = false;   // (else oneshot: dead after its first release)
    bool active = false;
    bool dead = false;
    QPointer<QWaylandSurface> surface;
    QPointF hint;              // the lock's cursor position hint (surface-local)
    bool hasHint = false;
    virtual void sendOn() = 0;
    virtual void sendOff() = 0;
};

class ZLockedPointer : public QtWaylandServer::zwp_locked_pointer_v1, public ZConstraint
{
public:
    ZLockedPointer(ZPointer *z, wl_client *client, uint32_t id, int version);
    void sendOn() override { send_locked(); }
    void sendOff() override { send_unlocked(); }

protected:
    void zwp_locked_pointer_v1_destroy(Resource *resource) override;
    void zwp_locked_pointer_v1_destroy_resource(Resource *resource) override;
    void zwp_locked_pointer_v1_set_cursor_position_hint(Resource *, wl_fixed_t x, wl_fixed_t y) override;
    void zwp_locked_pointer_v1_set_region(Resource *, struct ::wl_resource *) override {}

private:
    ZPointer *m_z;
};

class ZConfinedPointer : public QtWaylandServer::zwp_confined_pointer_v1, public ZConstraint
{
public:
    ZConfinedPointer(ZPointer *z, wl_client *client, uint32_t id, int version);
    void sendOn() override { send_confined(); }
    void sendOff() override { send_unconfined(); }

protected:
    void zwp_confined_pointer_v1_destroy(Resource *resource) override;
    void zwp_confined_pointer_v1_destroy_resource(Resource *resource) override;
    void zwp_confined_pointer_v1_set_region(Resource *, struct ::wl_resource *) override {}

private:
    ZPointer *m_z;
};

class ZPointer : public QObject,
                 public QtWaylandServer::zwp_relative_pointer_manager_v1,
                 public QtWaylandServer::zwp_pointer_constraints_v1
{
    Q_OBJECT
public:
    explicit ZPointer(QWaylandCompositor *compositor);
    static ZPointer *instance() { return s_instance; }

    // each mouse event (InputPump): its motion to the client under the
    // pointer, the constraints looked at again. true: the pointer is
    // locked (no ordinary motion goes out)
    bool motion(int dx, int dy);
    bool locked() const { return m_on && m_on->active && m_on->lock; }
    // look at the constraints again (focus changed, a window moved)
    void update();

    void forget(ZRelativePointer *r) { m_rel.removeAll(r); }
    void forget(ZConstraint *c);

protected:
    void zwp_relative_pointer_manager_v1_destroy(QtWaylandServer::zwp_relative_pointer_manager_v1::Resource *r) override;
    void zwp_relative_pointer_manager_v1_get_relative_pointer(
        QtWaylandServer::zwp_relative_pointer_manager_v1::Resource *r, uint32_t id, struct ::wl_resource *pointer) override;
    void zwp_pointer_constraints_v1_destroy(QtWaylandServer::zwp_pointer_constraints_v1::Resource *r) override;
    void zwp_pointer_constraints_v1_lock_pointer(QtWaylandServer::zwp_pointer_constraints_v1::Resource *r, uint32_t id,
                                                 struct ::wl_resource *surface, struct ::wl_resource *pointer,
                                                 struct ::wl_resource *region, uint32_t lifetime) override;
    void zwp_pointer_constraints_v1_confine_pointer(QtWaylandServer::zwp_pointer_constraints_v1::Resource *r, uint32_t id,
                                                    struct ::wl_resource *surface, struct ::wl_resource *pointer,
                                                    struct ::wl_resource *region, uint32_t lifetime) override;

private:
    void add(ZConstraint *c, QtWaylandServer::zwp_pointer_constraints_v1::Resource *r, struct ::wl_resource *surface,
             uint32_t lifetime);
    void on(ZConstraint *c);
    void off(ZConstraint *c);
    QRect sceneRect(QWaylandSurface *s) const;   // where the surface is on the screen
    QWaylandSurface *pointerSurface() const;

    static ZPointer *s_instance;
    QWaylandCompositor *m_comp;
    QWaylandSeat *m_seat = nullptr;
    QList<ZRelativePointer *> m_rel;
    QList<ZConstraint *> m_cons;
    ZConstraint *m_on = nullptr;      // the active one (one at a time: one seat)
    QRect m_confined;                 // what the kernel was told
    QPointer<QWaylandSurface> m_cursorHiddenFor;
};
