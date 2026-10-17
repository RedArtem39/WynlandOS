// WynlandOS - Zerp 2.0's Hyprland global shortcuts (see zglobalshortcuts.h)
#include "zglobalshortcuts.h"

#include <QtWaylandCompositor/QWaylandCompositor>

#include <cstdio>
#include <ctime>

ZGlobalShortcuts *ZGlobalShortcuts::s_instance = nullptr;

// ---------------------------------------------------------------- shortcut

ZGlobalShortcut::ZGlobalShortcut(ZGlobalShortcuts *mgr, wl_client *client, uint32_t id, int version,
                                 const QString &key, const QString &description)
    : QtWaylandServer::hyprland_global_shortcut_v1(client, int(id), version),
      m_mgr(mgr), m_key(key), m_desc(description)
{
}

void ZGlobalShortcut::send(bool pressed)
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t sec = uint64_t(ts.tv_sec);
    if (pressed) send_pressed(uint32_t(sec >> 32), uint32_t(sec), uint32_t(ts.tv_nsec));
    else send_released(uint32_t(sec >> 32), uint32_t(sec), uint32_t(ts.tv_nsec));
}

void ZGlobalShortcut::hyprland_global_shortcut_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void ZGlobalShortcut::hyprland_global_shortcut_v1_destroy_resource(Resource *)
{
    m_mgr->forget(this);
    delete this;
}

// ---------------------------------------------------------------- manager

ZGlobalShortcuts::ZGlobalShortcuts() : QWaylandCompositorExtensionTemplate<ZGlobalShortcuts>() {}
ZGlobalShortcuts::ZGlobalShortcuts(QWaylandCompositor *compositor)
    : QWaylandCompositorExtensionTemplate<ZGlobalShortcuts>(compositor) {}

void ZGlobalShortcuts::initialize()
{
    QWaylandCompositorExtensionTemplate::initialize();
    auto *compositor = static_cast<QWaylandCompositor *>(extensionContainer());
    if (!compositor) {
        fprintf(stderr, "[zerp2] global shortcuts: no compositor\n");
        return;
    }
    init(compositor->display(), 1);
    s_instance = this;
}

bool ZGlobalShortcuts::fire(const QString &key, bool pressed)
{
    ZGlobalShortcut *s = m_shortcuts.value(key);
    if (!s) {
        if (pressed) fprintf(stderr, "[zerp2] global shortcut %s: nobody registered it\n", qPrintable(key));
        return false;
    }
    if (pressed) fprintf(stderr, "[zerp2] global shortcut %s\n", qPrintable(key));
    s->send(pressed);
    return true;
}

QString ZGlobalShortcuts::list() const
{
    QString out;
    for (auto it = m_shortcuts.cbegin(); it != m_shortcuts.cend(); ++it)
        out += it.key() + QStringLiteral(" - ") + it.value()->description() + QLatin1Char('\n');
    return out;
}

void ZGlobalShortcuts::forget(ZGlobalShortcut *s)
{
    if (m_shortcuts.value(s->key()) == s) m_shortcuts.remove(s->key());
}

void ZGlobalShortcuts::hyprland_global_shortcuts_manager_v1_register_shortcut(Resource *resource, uint32_t shortcut,
                                                                              const QString &id, const QString &app_id,
                                                                              const QString &description,
                                                                              const QString &)
{
    const QString key = app_id + QLatin1Char(':') + id;
    if (m_shortcuts.contains(key)) {
        wl_resource_post_error(resource->handle, error_already_taken,
                               "the app_id + id combination has already been registered");
        return;
    }
    auto *s = new ZGlobalShortcut(this, resource->client(), shortcut, wl_resource_get_version(resource->handle),
                                  key, description);
    m_shortcuts.insert(key, s);
    fprintf(stderr, "[zerp2] global shortcut registered: %s\n", qPrintable(key));
}

void ZGlobalShortcuts::hyprland_global_shortcuts_manager_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}
