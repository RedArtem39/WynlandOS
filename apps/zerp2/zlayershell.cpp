// WynlandOS - Zerp 2.0's wlr-layer-shell (see zlayershell.h)
#include "zlayershell.h"

#include <QtWaylandCompositor/QWaylandCompositor>
#include <QtWaylandCompositor/QWaylandResource>
#include <QtWaylandCompositor/QWaylandSurfaceRole>

#include <cstdio>

enum { ANCHOR_TOP = 1, ANCHOR_BOTTOM = 2, ANCHOR_LEFT = 4, ANCHOR_RIGHT = 8 };

static QWaylandSurfaceRole s_role("zwlr_layer_surface_v1");

// ---------------------------------------------------------------- surface

ZLayerSurface::ZLayerSurface(ZLayerShell *shell, QWaylandSurface *surface, wl_client *client, uint32_t id,
                             int version, uint32_t layer, const QString &scope)
    : QtWaylandServer::zwlr_layer_surface_v1(client, int(id), version),
      m_shell(shell), m_surface(surface), m_scope(scope)
{
    m_pend.layer = layer <= 3 ? int(layer) : 0;
    m_cur = m_pend;
    // the state is double-buffered: it applies at the surface's commit
    connect(surface, &QWaylandSurface::redraw, this, &ZLayerSurface::committed);
    connect(surface, &QWaylandSurface::hasContentChanged, this, &ZLayerSurface::committed);
    connect(surface, &QObject::destroyed, this, [this] { emit gone(); });
}

ZLayerSurface::~ZLayerSurface()
{
    m_shell->forget(this);
}

int ZLayerSurface::zoneEdge() const
{
    if (m_cur.zone <= 0) return 0;
    if (m_cur.edge) return m_cur.edge;
    // anchored to one edge, or to one edge and both of its neighbours
    const int a = m_cur.anchors;
    for (int e : { ANCHOR_TOP, ANCHOR_BOTTOM, ANCHOR_LEFT, ANCHOR_RIGHT }) {
        const int across = (e == ANCHOR_TOP || e == ANCHOR_BOTTOM) ? (ANCHOR_LEFT | ANCHOR_RIGHT) : (ANCHOR_TOP | ANCHOR_BOTTOM);
        if (a == e || a == (e | across)) return e;
    }
    return 0;
}

QSize ZLayerSurface::sizeFor(const State &s) const
{
    const QSize out = m_shell->outputSize();
    int w = s.size.width(), h = s.size.height();
    // 0 along an axis anchored at both ends: the output's, less the margins
    if (w == 0 && (s.anchors & (ANCHOR_LEFT | ANCHOR_RIGHT)) == (ANCHOR_LEFT | ANCHOR_RIGHT))
        w = out.width() - s.margins.left() - s.margins.right();
    if (h == 0 && (s.anchors & (ANCHOR_TOP | ANCHOR_BOTTOM)) == (ANCHOR_TOP | ANCHOR_BOTTOM))
        h = out.height() - s.margins.top() - s.margins.bottom();
    return QSize(qMax(w, 0), qMax(h, 0));
}

void ZLayerSurface::committed()
{
    if (!m_surface) return;
    const bool layoutChanged = m_pend.layer != m_cur.layer || m_pend.anchors != m_cur.anchors ||
                               m_pend.zone != m_cur.zone || m_pend.margins != m_cur.margins ||
                               m_pend.keyboard != m_cur.keyboard || m_pend.edge != m_cur.edge;
    const QSize want = sizeFor(m_pend);
    m_cur = m_pend;
    const bool hasContent = m_surface->hasContent();
    if (!hasContent) {
        // no buffer: the initial commit (or an unmap) -- tell it its size
        if (m_mapped) { m_mapped = false; emit changed(); }
        if (!m_configured || want != m_size) {
            m_size = want;
            m_serial = m_surface->compositor()->nextSerial();
            send_configure(m_serial, uint32_t(m_size.width()), uint32_t(m_size.height()));
            m_configured = true;
        }
    } else {
        if (want != m_size && m_configured) {
            m_size = want;
            m_serial = m_surface->compositor()->nextSerial();
            send_configure(m_serial, uint32_t(m_size.width()), uint32_t(m_size.height()));
        }
        // a size of 0 the client chose itself: the buffer's
        if (m_size.width() == 0 || m_size.height() == 0) {
            const QSize b = m_surface->destinationSize();
            if (m_size.width() == 0) m_size.setWidth(b.width());
            if (m_size.height() == 0) m_size.setHeight(b.height());
        }
        m_mapped = true;
    }
    if (layoutChanged) m_shell->updateReserved();
    emit changed();
}

void ZLayerSurface::reconfigure()
{
    if (!m_configured) return;
    const QSize want = sizeFor(m_cur);
    if (want == m_size || (want.width() == 0 && want.height() == 0)) return;
    m_size = want;
    m_serial = m_surface->compositor()->nextSerial();
    send_configure(m_serial, uint32_t(m_size.width()), uint32_t(m_size.height()));
    emit changed();
}

void ZLayerSurface::zwlr_layer_surface_v1_ack_configure(Resource *, uint32_t serial)
{
    m_acked = serial;
}

void ZLayerSurface::zwlr_layer_surface_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void ZLayerSurface::zwlr_layer_surface_v1_destroy_resource(Resource *)
{
    emit gone();
    deleteLater();
}

// ---------------------------------------------------------------- shell

ZLayerShell::ZLayerShell() : QWaylandCompositorExtensionTemplate<ZLayerShell>() {}
ZLayerShell::ZLayerShell(QWaylandCompositor *compositor) : QWaylandCompositorExtensionTemplate<ZLayerShell>(compositor) {}

void ZLayerShell::initialize()
{
    QWaylandCompositorExtensionTemplate::initialize();
    auto *compositor = static_cast<QWaylandCompositor *>(extensionContainer());
    if (!compositor) {
        fprintf(stderr, "[zerp2] layer shell: no compositor\n");
        return;
    }
    init(compositor->display(), 4);
}

void ZLayerShell::setOutputSize(QSize s)
{
    if (s == m_output || s.isEmpty()) return;
    m_output = s;
    emit outputSizeChanged();
    for (ZLayerSurface *l : m_surfaces) l->reconfigure();
}

QList<QObject *> ZLayerShell::surfaces() const
{
    QList<QObject *> out;
    for (ZLayerSurface *l : m_surfaces) out.append(l);
    return out;
}

void ZLayerShell::updateReserved()
{
    QMargins r;
    for (ZLayerSurface *l : m_surfaces) {
        const int z = l->exclusiveZone();
        const QMargins m = l->margins();
        switch (l->zoneEdge()) {
        case ANCHOR_TOP: r.setTop(qMax(r.top(), z + m.top())); break;
        case ANCHOR_BOTTOM: r.setBottom(qMax(r.bottom(), z + m.bottom())); break;
        case ANCHOR_LEFT: r.setLeft(qMax(r.left(), z + m.left())); break;
        case ANCHOR_RIGHT: r.setRight(qMax(r.right(), z + m.right())); break;
        default: break;
        }
    }
    if (r != m_reserved) {
        m_reserved = r;
        emit reservedChanged();
    }
}

void ZLayerShell::forget(ZLayerSurface *s)
{
    if (m_surfaces.removeOne(s)) {
        emit surfacesChanged();
        updateReserved();
    }
}

void ZLayerShell::zwlr_layer_shell_v1_get_layer_surface(Resource *resource, uint32_t id, struct ::wl_resource *surfaceRes,
                                                        struct ::wl_resource *, uint32_t layer, const QString &scope)
{
    QWaylandSurface *surface = QWaylandSurface::fromResource(surfaceRes);
    if (!surface) return;
    if (!surface->setRole(&s_role, resource->handle, ZWLR_LAYER_SHELL_V1_ERROR_ROLE)) return;
    auto *l = new ZLayerSurface(this, surface, resource->client(), id, wl_resource_get_version(resource->handle),
                                layer, scope);
    connect(l, &ZLayerSurface::gone, this, [this, l] { forget(l); });
    connect(l, &ZLayerSurface::changed, this, [this] { updateReserved(); });
    m_surfaces.append(l);
    fprintf(stderr, "[zerp2] layer surface \"%s\" (layer %u)\n", qPrintable(scope), layer);
    emit surfacesChanged();
}

void ZLayerShell::zwlr_layer_shell_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}
