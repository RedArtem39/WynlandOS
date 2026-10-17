// WynlandOS - Zerp 2.0: Hyprland's global shortcuts (hyprland_global_shortcuts_v1).
//
// A desktop shell registers named actions -- Caelestia's "caelestia:launcher",
// "caelestia:session", ... (Quickshell's GlobalShortcut) -- without knowing
// which keys trigger them; the compositor's key bindings do (apps/zerp2/
// keybinds.h: a `global, caelestia:launcher` binding). Firing one sends the
// shortcut's pressed and released events to the client that registered it.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtWaylandCompositor/QWaylandCompositorExtension>
#include <QtWaylandCompositor/QWaylandQuickExtension>

#include "qwayland-server-hyprland-global-shortcuts-v1.h"

class ZGlobalShortcuts;

class ZGlobalShortcut : public QtWaylandServer::hyprland_global_shortcut_v1
{
public:
    ZGlobalShortcut(ZGlobalShortcuts *mgr, wl_client *client, uint32_t id, int version, const QString &key,
                    const QString &description);
    void send(bool pressed);
    QString key() const { return m_key; }
    QString description() const { return m_desc; }

protected:
    void hyprland_global_shortcut_v1_destroy(Resource *resource) override;
    void hyprland_global_shortcut_v1_destroy_resource(Resource *resource) override;

private:
    ZGlobalShortcuts *m_mgr;
    QString m_key, m_desc;
};

class ZGlobalShortcuts : public QWaylandCompositorExtensionTemplate<ZGlobalShortcuts>,
                         public QtWaylandServer::hyprland_global_shortcuts_manager_v1
{
    Q_OBJECT
public:
    ZGlobalShortcuts();
    explicit ZGlobalShortcuts(QWaylandCompositor *compositor);
    void initialize() override;

    // the one in the running compositor (the key bindings fire through it)
    static ZGlobalShortcuts *instance() { return s_instance; }
    // "appid:id": pressed or released; false if nobody registered it
    bool fire(const QString &key, bool pressed);
    // what is registered, one "appid:id - description" a line (hyprctl globalshortcuts)
    QString list() const;
    void forget(ZGlobalShortcut *s);

protected:
    void hyprland_global_shortcuts_manager_v1_register_shortcut(Resource *resource, uint32_t shortcut,
                                                                const QString &id, const QString &app_id,
                                                                const QString &description,
                                                                const QString &trigger_description) override;
    void hyprland_global_shortcuts_manager_v1_destroy(Resource *resource) override;

private:
    static ZGlobalShortcuts *s_instance;
    QHash<QString, ZGlobalShortcut *> m_shortcuts;
};

Q_COMPOSITOR_DECLARE_QUICK_EXTENSION_CLASS(ZGlobalShortcuts)
