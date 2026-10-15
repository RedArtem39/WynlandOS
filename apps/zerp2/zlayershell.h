// WynlandOS - Zerp 2.0: wlr-layer-shell (zwlr_layer_shell_v1, version 4).
//
// Desktop shells (Quickshell's PanelWindow: Caelestia's bar, wallpaper,
// notifications, launcher) are layer surfaces: anchored to screen edges,
// stacked in four layers (background, bottom, top, overlay) around the
// windows, and an exclusive zone keeps the windows out of a panel's strip.
// The protocol is here (C++); Shell.qml draws the surfaces and lays the
// windows out in what is left.
//
// Popups of layer surfaces (get_popup) are not supported yet: xdg-popup's
// parent must be a toplevel here.
#pragma once

#include <QtCore/QList>
#include <QtCore/QMargins>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QSize>
#include <QtWaylandCompositor/QWaylandCompositorExtension>
#include <QtWaylandCompositor/QWaylandQuickExtension>
#include <QtWaylandCompositor/QWaylandSurface>

#include "qwayland-server-wlr-layer-shell-unstable-v1.h"

class ZLayerShell;

class ZLayerSurface : public QObject, public QtWaylandServer::zwlr_layer_surface_v1
{
    Q_OBJECT
    Q_PROPERTY(QWaylandSurface *surface READ surface CONSTANT)
    Q_PROPERTY(QString scope READ scope CONSTANT)
    Q_PROPERTY(int layer READ layer NOTIFY changed)
    Q_PROPERTY(int anchors READ anchors NOTIFY changed)              // 1 top, 2 bottom, 4 left, 8 right
    Q_PROPERTY(int exclusiveZone READ exclusiveZone NOTIFY changed)
    Q_PROPERTY(QMargins margins READ margins NOTIFY changed)
    Q_PROPERTY(int keyboard READ keyboard NOTIFY changed)            // 0 none, 1 exclusive, 2 on demand
    Q_PROPERTY(int width READ width NOTIFY changed)                  // as configured
    Q_PROPERTY(int height READ height NOTIFY changed)
    Q_PROPERTY(bool mapped READ mapped NOTIFY changed)
public:
    ZLayerSurface(ZLayerShell *shell, QWaylandSurface *surface, wl_client *client, uint32_t id, int version,
                  uint32_t layer, const QString &scope);
    ~ZLayerSurface() override;

    QWaylandSurface *surface() const { return m_surface; }
    QString scope() const { return m_scope; }
    int layer() const { return m_cur.layer; }
    int anchors() const { return m_cur.anchors; }
    int exclusiveZone() const { return m_cur.zone; }
    QMargins margins() const { return m_cur.margins; }
    int keyboard() const { return m_cur.keyboard; }
    int width() const { return m_size.width(); }
    int height() const { return m_size.height(); }
    bool mapped() const { return m_mapped; }
    // the edge an exclusive zone is on: 1/2/4/8, 0 for none
    int zoneEdge() const;
    void reconfigure();   // the output changed: a new size if it follows it

signals:
    void changed();
    void gone();

protected:
    void zwlr_layer_surface_v1_set_size(Resource *, uint32_t w, uint32_t h) override { m_pend.size = QSize(int(w), int(h)); }
    void zwlr_layer_surface_v1_set_anchor(Resource *, uint32_t a) override { m_pend.anchors = int(a & 15); }
    void zwlr_layer_surface_v1_set_exclusive_zone(Resource *, int32_t z) override { m_pend.zone = z; }
    void zwlr_layer_surface_v1_set_margin(Resource *, int32_t t, int32_t r, int32_t b, int32_t l) override { m_pend.margins = QMargins(l, t, r, b); }
    void zwlr_layer_surface_v1_set_keyboard_interactivity(Resource *, uint32_t k) override { m_pend.keyboard = int(k); }
    void zwlr_layer_surface_v1_set_layer(Resource *, uint32_t l) override { if (l <= 3) m_pend.layer = int(l); }
    void zwlr_layer_surface_v1_set_exclusive_edge(Resource *, uint32_t e) override { m_pend.edge = int(e); }
    void zwlr_layer_surface_v1_get_popup(Resource *, struct ::wl_resource *) override {}
    void zwlr_layer_surface_v1_ack_configure(Resource *, uint32_t serial) override;
    void zwlr_layer_surface_v1_destroy(Resource *resource) override;
    void zwlr_layer_surface_v1_destroy_resource(Resource *resource) override;

private:
    struct State {
        int layer = 0, anchors = 0, zone = 0, keyboard = 0, edge = 0;
        QSize size;
        QMargins margins;
    };
    void committed();
    QSize sizeFor(const State &s) const;

    ZLayerShell *m_shell;
    QPointer<QWaylandSurface> m_surface;
    QString m_scope;
    State m_pend, m_cur;
    QSize m_size;                 // last configured
    uint32_t m_serial = 0, m_acked = 0;
    bool m_configured = false, m_mapped = false;
};

class ZLayerShell : public QWaylandCompositorExtensionTemplate<ZLayerShell>,
                    public QtWaylandServer::zwlr_layer_shell_v1
{
    Q_OBJECT
    Q_PROPERTY(QSize outputSize READ outputSize WRITE setOutputSize NOTIFY outputSizeChanged)
    Q_PROPERTY(QList<QObject *> surfaces READ surfaces NOTIFY surfacesChanged)
    // what panels keep for themselves (exclusive zones), per edge
    Q_PROPERTY(QMargins reserved READ reserved NOTIFY reservedChanged)
public:
    ZLayerShell();
    explicit ZLayerShell(QWaylandCompositor *compositor);
    void initialize() override;

    QSize outputSize() const { return m_output; }
    void setOutputSize(QSize s);
    QList<QObject *> surfaces() const;
    QMargins reserved() const { return m_reserved; }
    void updateReserved();
    void forget(ZLayerSurface *s);

signals:
    void outputSizeChanged();
    void surfacesChanged();
    void reservedChanged();

protected:
    void zwlr_layer_shell_v1_get_layer_surface(Resource *resource, uint32_t id, struct ::wl_resource *surface,
                                               struct ::wl_resource *output, uint32_t layer,
                                               const QString &scope) override;
    void zwlr_layer_shell_v1_destroy(Resource *resource) override;

private:
    QSize m_output{ 1280, 720 };
    QList<ZLayerSurface *> m_surfaces;
    QMargins m_reserved;
};

Q_COMPOSITOR_DECLARE_QUICK_EXTENSION_CLASS(ZLayerShell)
