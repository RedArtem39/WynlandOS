#!/usr/bin/env python3
# WynlandOS - Qt's linux-dmabuf client buffer plugin for Zerp 2.0, made
# to speak version 4 of the protocol (tools/stage_qt6.sh builds it; Ubuntu
# does not ship the plugin).
#
# Qt 6.10's plugin announces zwp_linux_dmabuf_v1 version 3: formats and
# modifiers as events. Mesa's Wayland EGL learns the compositor's GPU from
# version 4's feedback (main_device) -- or from wl_drm, which Ubuntu's Mesa
# no longer serves -- so with version 3 every GL client fell back to
# software. Added: get_default_feedback / get_surface_feedback, answered
# with the format table (a memfd of format + modifier pairs), the render
# node's device number and one tranche of every format.
#
# usage: apply.py DIR      (DIR: the plugin's sources, copied out of qtwayland)
import os
import sys

d = sys.argv[1]


def edit(rel, old, new):
    path = os.path.join(d, rel)
    s = open(path, encoding="utf-8").read()
    if s.count(old) != 1:
        sys.exit("zerp2-dmabuf: %s: expected one %r, found %d" % (rel, old, s.count(old)))
    open(path, "w", encoding="utf-8").write(s.replace(old, new))


edit("linuxdmabuf.h", "    void zwp_linux_dmabuf_v1_create_params(Resource *resource, uint32_t params_id) override;\n",
     "    void zwp_linux_dmabuf_v1_create_params(Resource *resource, uint32_t params_id) override;\n"
     "    void zwp_linux_dmabuf_v1_get_default_feedback(Resource *resource, uint32_t id) override;\n"
     "    void zwp_linux_dmabuf_v1_get_surface_feedback(Resource *resource, uint32_t id, struct ::wl_resource *surface) override;\n"
     "\nprivate:\n"
     "    void sendFeedback(wl_client *client, uint32_t id, int version);\n"
     "    int m_formatTable = -1;           // memfd: {u32 format, u32 pad, u64 modifier}[]\n"
     "    uint32_t m_formatTableSize = 0;\n"
     "    QByteArray m_mainDevice;          // dev_t of the render node\n"
     "    QByteArray m_indices;             // u16[]: every table entry, one tranche\n")

edit("linuxdmabuf.cpp", "    : zwp_linux_dmabuf_v1(display, 3 /*version*/)", "    : zwp_linux_dmabuf_v1(display, 4 /*version*/)")

# the format table and the device, made once (the modifiers are set
# before any client binds)
edit("linuxdmabuf.cpp", "    Q_ASSERT(resourceMap().isEmpty());\n    m_modifiers = modifiers;\n",
     """    Q_ASSERT(resourceMap().isEmpty());
    m_modifiers = modifiers;

    struct Entry { uint32_t format, pad; uint64_t modifier; };
    QList<Entry> table;
    for (auto it = m_modifiers.constBegin(); it != m_modifiers.constEnd(); ++it) {
        const QList<uint64_t> mods = it.value().isEmpty() ? QList<uint64_t>{ DRM_FORMAT_MOD_INVALID } : it.value();
        for (uint64_t m : mods) table.append({ it.key(), 0, m });
    }
    m_formatTableSize = uint32_t(table.size() * sizeof(Entry));
    m_formatTable = memfd_create("dmabuf-format-table", MFD_CLOEXEC);
    if (m_formatTable >= 0 && write(m_formatTable, table.constData(), m_formatTableSize) != ssize_t(m_formatTableSize)) {
        close(m_formatTable);
        m_formatTable = -1;
    }
    m_indices.clear();
    for (int i = 0; i < table.size(); ++i) {
        const uint16_t idx = uint16_t(i);
        m_indices.append(reinterpret_cast<const char *>(&idx), sizeof(idx));
    }
    struct stat st;
    const char *node = qEnvironmentVariableIsSet("ZERP2_RENDER_NODE") ? getenv("ZERP2_RENDER_NODE") : "/dev/dri/renderD128";
    if (stat(node, &st) == 0) {
        const dev_t dev = st.st_rdev;
        m_mainDevice = QByteArray(reinterpret_cast<const char *>(&dev), sizeof(dev));
    }
    qCDebug(qLcWaylandCompositorHardwareIntegration) << "dmabuf feedback:" << table.size() << "formats, device" << node;
""")

# version 4 clients take the formats from the feedback only
edit("linuxdmabuf.cpp", "void LinuxDmabuf::zwp_linux_dmabuf_v1_bind_resource(Resource *resource)\n{\n",
     "void LinuxDmabuf::zwp_linux_dmabuf_v1_bind_resource(Resource *resource)\n{\n"
     "    if (resource->version() >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION)\n"
     "        return;\n")

edit("linuxdmabuf.cpp", "LinuxDmabufParams::LinuxDmabufParams(",
     """namespace {
// one feedback object; it never changes (one GPU, no scanout tranches)
class LinuxDmabufFeedback : public QtWaylandServer::zwp_linux_dmabuf_feedback_v1
{
public:
    LinuxDmabufFeedback(wl_client *client, uint32_t id, int version)
        : zwp_linux_dmabuf_feedback_v1(client, id, version) {}
protected:
    void zwp_linux_dmabuf_feedback_v1_destroy(Resource *resource) override { wl_resource_destroy(resource->handle); }
    void zwp_linux_dmabuf_feedback_v1_destroy_resource(Resource *) override { delete this; }
};
}

void LinuxDmabuf::sendFeedback(wl_client *client, uint32_t id, int version)
{
    auto *fb = new LinuxDmabufFeedback(client, id, version);   // deleted with its resource
    wl_resource *r = fb->resource()->handle;
    if (m_formatTable >= 0)
        fb->send_format_table(r, m_formatTable, m_formatTableSize);
    fb->send_main_device(r, m_mainDevice);
    fb->send_tranche_target_device(r, m_mainDevice);
    fb->send_tranche_flags(r, 0);
    fb->send_tranche_formats(r, m_indices);
    fb->send_tranche_done(r);
    fb->send_done(r);
}

void LinuxDmabuf::zwp_linux_dmabuf_v1_get_default_feedback(Resource *resource, uint32_t id)
{
    sendFeedback(resource->client(), id, resource->version());
}

void LinuxDmabuf::zwp_linux_dmabuf_v1_get_surface_feedback(Resource *resource, uint32_t id, struct ::wl_resource *surface)
{
    Q_UNUSED(surface);
    sendFeedback(resource->client(), id, resource->version());
}

LinuxDmabufParams::LinuxDmabufParams(""")

edit("linuxdmabuf.cpp", "#include <unistd.h>\n",
     "#include <unistd.h>\n#include <sys/mman.h>\n#include <sys/stat.h>\n")

print("  ZERP2      linux-dmabuf v4 feedback patched in")
