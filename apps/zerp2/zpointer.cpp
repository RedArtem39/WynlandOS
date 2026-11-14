// WynlandOS - Zerp 2.0's pointer for games (see zpointer.h)
#include "zpointer.h"

#include <QtQuick/QQuickItem>
#include <QtWaylandCompositor/QWaylandClient>
#include <QtWaylandCompositor/QWaylandCompositor>
#include <QtWaylandCompositor/QWaylandSeat>
#include <QtWaylandCompositor/QWaylandSurface>
#include <QtWaylandCompositor/QWaylandView>

#include <cstdio>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

// SYS_pointer_ctl (kernel/syscall.c): what the kernel's pointer does
static void pointerCtl(long op, long a, long b) { syscall(416, op, a, b); }
enum { PtrLock = 1, PtrConfine = 2, PtrWarp = 3, PtrHide = 4 };

ZPointer *ZPointer::s_instance = nullptr;

// ---------------------------------------------------------------- relative

ZRelativePointer::ZRelativePointer(ZPointer *z, wl_client *client, uint32_t id, int version,
                                   struct ::wl_resource *pointer)
    : QtWaylandServer::zwp_relative_pointer_v1(client, int(id), version), m_z(z), m_client(client), m_pointer(pointer)
{
    m_gone.self = this;
    m_gone.l.notify = [](struct ::wl_listener *l, void *) {
        Gone *g = wl_container_of(l, g, l);
        g->self->m_pointer = nullptr;
        wl_list_remove(&g->l.link);
    };
    if (m_pointer) wl_resource_add_destroy_listener(m_pointer, &m_gone.l);
}

ZRelativePointer::~ZRelativePointer()
{
    if (m_pointer) wl_list_remove(&m_gone.l.link);
}

void ZRelativePointer::motion(uint32_t hi, uint32_t lo, int dx, int dy)
{
    send_relative_motion(hi, lo, wl_fixed_from_int(dx), wl_fixed_from_int(dy), wl_fixed_from_int(dx), wl_fixed_from_int(dy));
    if (m_pointer && wl_resource_get_version(m_pointer) >= 5)
        wl_resource_post_event_array(m_pointer, 5, nullptr);   // wl_pointer.frame (as wlcompat.c sends it)
}

void ZRelativePointer::zwp_relative_pointer_v1_destroy(Resource *resource) { wl_resource_destroy(resource->handle); }

void ZRelativePointer::zwp_relative_pointer_v1_destroy_resource(Resource *)
{
    m_z->forget(this);
    delete this;
}

// ---------------------------------------------------------------- the constraints

ZLockedPointer::ZLockedPointer(ZPointer *z, wl_client *client, uint32_t id, int version)
    : QtWaylandServer::zwp_locked_pointer_v1(client, int(id), version), m_z(z)
{
    lock = true;
}

void ZLockedPointer::zwp_locked_pointer_v1_destroy(Resource *resource) { wl_resource_destroy(resource->handle); }

void ZLockedPointer::zwp_locked_pointer_v1_destroy_resource(Resource *)
{
    m_z->forget(this);
    delete this;
}

void ZLockedPointer::zwp_locked_pointer_v1_set_cursor_position_hint(Resource *, wl_fixed_t x, wl_fixed_t y)
{
    hint = QPointF(wl_fixed_to_double(x), wl_fixed_to_double(y));
    hasHint = true;
}

ZConfinedPointer::ZConfinedPointer(ZPointer *z, wl_client *client, uint32_t id, int version)
    : QtWaylandServer::zwp_confined_pointer_v1(client, int(id), version), m_z(z)
{
}

void ZConfinedPointer::zwp_confined_pointer_v1_destroy(Resource *resource) { wl_resource_destroy(resource->handle); }

void ZConfinedPointer::zwp_confined_pointer_v1_destroy_resource(Resource *)
{
    m_z->forget(this);
    delete this;
}

// ---------------------------------------------------------------- the two managers

ZPointer::ZPointer(QWaylandCompositor *compositor)
    : QtWaylandServer::zwp_relative_pointer_manager_v1(compositor->display(), 1),
      QtWaylandServer::zwp_pointer_constraints_v1(compositor->display(), 1),
      m_comp(compositor)
{
    s_instance = this;
    m_seat = compositor->defaultSeat();
    if (!m_seat) return;
    // focus moved: the constraints again, and the cursor back for a surface
    // that did not ask it away
    connect(m_seat, &QWaylandSeat::mouseFocusChanged, this, [this](QWaylandView *v, QWaylandView *) {
        QWaylandSurface *s = v ? v->surface() : nullptr;
        if (m_cursorHiddenFor && m_cursorHiddenFor != s) {
            m_cursorHiddenFor = nullptr;
            if (!locked()) pointerCtl(PtrHide, 0, 0);
        }
        update();
    });
    connect(m_seat, &QWaylandSeat::keyboardFocusChanged, this, [this] { update(); });
    // wl_pointer.set_cursor: no surface = no cursor (SDL's relative mode,
    // a video player's still mouse); any picture = ours, shown
    connect(m_seat, &QWaylandSeat::cursorSurfaceRequested, this,
            [this](QWaylandSurface *surface, int, int, QWaylandClient *client) {
                QWaylandSurface *ps = pointerSurface();
                if (!ps || !client || ps->client() != client) return;   // (not the one under the pointer)
                m_cursorHiddenFor = surface ? nullptr : ps;
                if (!locked()) pointerCtl(PtrHide, surface ? 0 : 1, 0);
            });
}

QWaylandSurface *ZPointer::pointerSurface() const
{
    QWaylandView *v = m_seat ? m_seat->mouseFocus() : nullptr;
    return v ? v->surface() : nullptr;
}

QRect ZPointer::sceneRect(QWaylandSurface *s) const
{
    QWaylandView *v = s ? s->primaryView() : nullptr;
    auto *item = v ? qobject_cast<QQuickItem *>(v->renderObject()) : nullptr;
    if (!item) return QRect();
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).toAlignedRect();
}

bool ZPointer::motion(int dx, int dy)
{
    update();
    if (dx || dy) {
        QWaylandSurface *s = pointerSurface();
        wl_client *c = s && s->client() ? s->client()->client() : nullptr;
        if (c) {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const uint64_t us = uint64_t(ts.tv_sec) * 1000000u + uint64_t(ts.tv_nsec) / 1000u;
            for (ZRelativePointer *r : m_rel)
                if (r->client() == c) r->motion(uint32_t(us >> 32), uint32_t(us), dx, dy);
        }
    }
    return locked();
}

// a constraint is on while its surface has the pointer and the keyboard
void ZPointer::update()
{
    if (!m_seat) return;
    QWaylandSurface *ps = pointerSurface(), *ks = m_seat->keyboardFocus();
    if (m_on && (!m_on->surface || m_on->surface != ps || m_on->surface != ks)) off(m_on);
    if (!m_on && ps && ps == ks)
        for (ZConstraint *c : m_cons)
            if (!c->dead && c->surface == ps) { on(c); break; }
    // a confinement follows its window (moved, resized, the canvas zoomed)
    if (m_on && !m_on->lock) {
        const QRect r = sceneRect(m_on->surface);
        if (r.isValid() && r != m_confined) {
            m_confined = r;
            pointerCtl(PtrConfine, long(uint32_t(r.left())) | (long(uint32_t(r.top())) << 32),
                       long(uint32_t(r.right())) | (long(uint32_t(r.bottom())) << 32));
        }
    }
}

void ZPointer::on(ZConstraint *c)
{
    m_on = c;
    c->active = true;
    if (c->lock) {
        pointerCtl(PtrLock, 1, 0);
        pointerCtl(PtrHide, 1, 0);   // (a locked pointer goes nowhere: nothing to show)
    } else {
        m_confined = QRect();      // (update() tells the kernel the rectangle)
    }
    c->sendOn();
    fprintf(stderr, "[zerp2] pointer %s\n", c->lock ? "locked" : "confined");
}

void ZPointer::off(ZConstraint *c)
{
    if (c->lock) {
        pointerCtl(PtrLock, 0, 0);
        // the client's hint: where its own cursor was drawn
        if (c->hasHint && c->surface) {
            const QRect r = sceneRect(c->surface);
            if (r.isValid()) pointerCtl(PtrWarp, long(r.left() + c->hint.x()), long(r.top() + c->hint.y()));
        }
        pointerCtl(PtrHide, m_cursorHiddenFor ? 1 : 0, 0);
    } else {
        pointerCtl(PtrConfine, -1, -1);
        m_confined = QRect();
    }
    c->active = false;
    if (!c->persistent) c->dead = true;
    m_on = nullptr;
    c->sendOff();
    fprintf(stderr, "[zerp2] pointer %s\n", c->lock ? "unlocked" : "unconfined");
}

void ZPointer::forget(ZConstraint *c)
{
    if (m_on == c) {
        // destroyed while on: the kernel lets go (no event: it is gone)
        if (c->lock) {
            pointerCtl(PtrLock, 0, 0);
            pointerCtl(PtrHide, m_cursorHiddenFor ? 1 : 0, 0);
        } else {
            pointerCtl(PtrConfine, -1, -1);
            m_confined = QRect();
        }
        m_on = nullptr;
        fprintf(stderr, "[zerp2] pointer let go\n");
    }
    m_cons.removeAll(c);
}

void ZPointer::zwp_relative_pointer_manager_v1_destroy(QtWaylandServer::zwp_relative_pointer_manager_v1::Resource *r)
{
    wl_resource_destroy(r->handle);
}

void ZPointer::zwp_relative_pointer_manager_v1_get_relative_pointer(
    QtWaylandServer::zwp_relative_pointer_manager_v1::Resource *r, uint32_t id, struct ::wl_resource *pointer)
{
    m_rel.append(new ZRelativePointer(this, r->client(), id, wl_resource_get_version(r->handle), pointer));
}

void ZPointer::zwp_pointer_constraints_v1_destroy(QtWaylandServer::zwp_pointer_constraints_v1::Resource *r)
{
    wl_resource_destroy(r->handle);
}

void ZPointer::add(ZConstraint *c, QtWaylandServer::zwp_pointer_constraints_v1::Resource *r,
                   struct ::wl_resource *surface, uint32_t lifetime)
{
    c->surface = QWaylandSurface::fromResource(surface);
    c->persistent = lifetime == QtWaylandServer::zwp_pointer_constraints_v1::lifetime_persistent;
    for (ZConstraint *o : m_cons)
        if (o->surface == c->surface && !o->dead) {
            wl_resource_post_error(r->handle, QtWaylandServer::zwp_pointer_constraints_v1::error_already_constrained,
                                   "the surface already has a pointer constraint");
            return;
        }
    m_cons.append(c);
    update();
}

void ZPointer::zwp_pointer_constraints_v1_lock_pointer(QtWaylandServer::zwp_pointer_constraints_v1::Resource *r,
                                                       uint32_t id, struct ::wl_resource *surface,
                                                       struct ::wl_resource *, struct ::wl_resource *, uint32_t lifetime)
{
    add(new ZLockedPointer(this, r->client(), id, wl_resource_get_version(r->handle)), r, surface, lifetime);
}

void ZPointer::zwp_pointer_constraints_v1_confine_pointer(QtWaylandServer::zwp_pointer_constraints_v1::Resource *r,
                                                          uint32_t id, struct ::wl_resource *surface,
                                                          struct ::wl_resource *, struct ::wl_resource *,
                                                          uint32_t lifetime)
{
    add(new ZConfinedPointer(this, r->client(), id, wl_resource_get_version(r->handle)), r, surface, lifetime);
}
