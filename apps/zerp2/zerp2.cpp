// WynlandOS - Zerp 2.0 client hosting, see zerp2.h.
#include "zerp2.h"

#include <QtCore/QFileInfo>
#include <QtGui/QImage>
#include <QtGui/QScreen>
#include <QtGui/QWheelEvent>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGSimpleTextureNode>
#include <QtQuick/QSGTexture>
#include <rhi/qrhi.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// WynlandOS syscalls (kernel/syscall.c)
static long sys_shm_create(long size) { return syscall(407, size, 0, 0); }
static long sys_spawn_argv(const char *path, const char **argv) { return syscall(408, path, argv, 0); }
static long sys_process_alive(long pid) { return syscall(411, pid, 0, 0); }

static const int kOutboxMax = 128;

// ---------------------------------------------------------------- ZClient

ZClient::ZClient(int id, const QString &path, QObject *parent)
    : QObject(parent), m_id(id), m_path(path)
{
    const QString base = QFileInfo(path).completeBaseName();
    m_appId = base;
    if (base == QLatin1String("zerp_term")) m_title = QStringLiteral("Terminal");
    else if (base == QLatin1String("zerp_files") || base == QLatin1String("files")) m_title = QStringLiteral("Files");
    else if (base == QLatin1String("zerp_rofi")) m_title = QStringLiteral("Launcher");
    else if (base == QLatin1String("qmldemo")) m_title = QStringLiteral("Qt Quick demo");
    else m_title = base;
}

ZClient::~ZClient()
{
    if (m_c2s >= 0) ::close(m_c2s);
    if (m_s2c >= 0) ::close(m_s2c);
    if (m_shm) munmap(m_shm, m_shmBytes);
}

bool ZClient::start(uint32_t shmBytes)
{
    int c2s[2], s2c[2];
    if (pipe(c2s) < 0) return false;
    if (pipe(s2c) < 0) { ::close(c2s[0]); ::close(c2s[1]); return false; }
    long shm = sys_shm_create(shmBytes);
    if (shm < 0) { ::close(c2s[0]); ::close(c2s[1]); ::close(s2c[0]); ::close(s2c[1]); return false; }

    // ours: never inherited by later spawns, never block the WM
    fcntl(c2s[0], F_SETFD, FD_CLOEXEC);
    fcntl(s2c[1], F_SETFD, FD_CLOEXEC);
    fcntl(c2s[0], F_SETFL, O_NONBLOCK);
    fcntl(s2c[1], F_SETFL, O_NONBLOCK);

    const QByteArray p = m_path.toLocal8Bit();
    char a1[16], a2[16], a3[16];
    snprintf(a1, sizeof a1, "%d", c2s[1]);
    snprintf(a2, sizeof a2, "%d", s2c[0]);
    snprintf(a3, sizeof a3, "%ld", shm);
    const char *argv[] = { p.constData(), a1, a2, a3, nullptr };
    m_pid = sys_spawn_argv(p.constData(), argv);

    ::close(c2s[1]);
    ::close(s2c[0]);
    if (m_pid < 0) { ::close(c2s[0]); ::close(s2c[1]); ::close((int)shm); return false; }

    m_c2s = c2s[0];
    m_s2c = s2c[1];
    m_shmBytes = shmBytes;
    void *map = mmap(nullptr, shmBytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)shm, 0);
    ::close((int)shm);
    m_shm = (map == MAP_FAILED) ? nullptr : (uint32_t *)map;
    fprintf(stderr, "[zerp2] client %d: %s pid %ld (c2s %d/%d s2c %d/%d shm %ld)\n",
            m_id, p.constData(), m_pid, c2s[0], c2s[1], s2c[0], s2c[1], shm);
    return m_shm != nullptr;
}

void ZClient::configure(int x, int y, int w, int h)
{
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if ((uint64_t)w * (uint64_t)h * 4 > m_shmBytes) return;   // can't exceed the buffer
    if (w == m_bufW && h == m_bufH) return;
    fprintf(stderr, "[zerp2] client %d configure %dx%d\n", m_id, w, h);
    m_bufW = w;
    m_bufH = h;
    m_dirty = true;
    m_damage = QRect(0, 0, w, h);
    ZerpMsg m = { ZERP_MSG_TILE_RECT, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    send(m);
    emit damaged();
}

void ZClient::close()
{
    if (m_pid > 0) ::kill((pid_t)m_pid, SIGTERM);
}

bool ZClient::alive() const
{
    return m_pid > 0 && sys_process_alive(m_pid) == 1;
}

void ZClient::sendMouse(int x, int y, uint32_t buttons, bool motion)
{
    if (m_bufW <= 0) return;
    x = qBound(0, x, m_bufW - 1);
    y = qBound(0, y, m_bufH - 1);
    ZerpMsg m = { ZERP_MSG_INPUT_MOUSE, (uint32_t)x, (uint32_t)y, buttons, 0 };
    send(m, motion);
}

void ZClient::sendKey(bool e0, uint8_t scancode)
{
    if (e0) { ZerpMsg p = { ZERP_MSG_INPUT_KEY, 0xE0, 0, 0, 0 }; m_out.append(p); }
    ZerpMsg k = { ZERP_MSG_INPUT_KEY, scancode, 0, 0, 0 };
    m_out.append(k);
    m_outTailMotion = false;
    flush();
}

void ZClient::send(const ZerpMsg &m, bool motion)
{
    // pure motion replaces queued pure motion: only the last position counts
    if (motion && !m_out.isEmpty() && m_outTailMotion) {
        m_out.last() = m;
        flush();
        return;
    }
    if (m_out.size() >= kOutboxMax) {
        // the client stopped reading: drop motion, keep everything else
        if (motion) return;
        for (int i = 0; i < m_out.size(); i++)
            if (m_out[i].type == ZERP_MSG_INPUT_MOUSE) { m_out.remove(i); break; }
    }
    m_out.append(m);
    m_outTailMotion = motion;
    flush();
}

void ZClient::flush()
{
    while (!m_out.isEmpty()) {
        if (::write(m_s2c, &m_out.first(), sizeof(ZerpMsg)) != (ssize_t)sizeof(ZerpMsg)) return;
        m_out.removeFirst();
    }
    m_outTailMotion = false;
}

bool ZClient::pump(QVector<QString> *spawnRequests)
{
    if (m_c2s < 0) return false;
    flush();
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(m_c2s, buf, sizeof buf);
        if (n == 0) return false;          // every writer gone: the client exited
        if (n < 0) break;                  // -EAGAIN: nothing more now
        m_rx.append(buf, (int)n);
    }
    int off = 0;
    bool closed = false;
    while (m_rx.size() - off >= 4) {
        uint32_t type;
        memcpy(&type, m_rx.constData() + off, 4);
        const int size = (type == ZERP_MSG_SPAWN) ? (int)sizeof(ZerpSpawnMsg) : (int)sizeof(ZerpMsg);
        if (m_rx.size() - off < size) break;
        if (type == ZERP_MSG_SPAWN) {
            ZerpSpawnMsg sm;
            memcpy(&sm, m_rx.constData() + off, sizeof sm);
            sm.path[sizeof(sm.path) - 1] = 0;
            spawnRequests->append(QString::fromLocal8Bit(sm.path));
        } else {
            ZerpMsg m;
            memcpy(&m, m_rx.constData() + off, sizeof m);
            if (m.type == ZERP_MSG_DAMAGE) {
                static int logged[64];
                if (m_id < 64 && !logged[m_id]++) fprintf(stderr, "[zerp2] client %d first damage %ux%u\n", m_id, m.w, m.h);
                m_damage |= QRect(int(m.x), int(m.y), int(m.w), int(m.h)) & QRect(0, 0, m_bufW, m_bufH);
                m_dirty = true; emit damaged();
            }
            else if (m.type == ZERP_MSG_CLOSE) closed = true;
        }
        off += size;
    }
    m_rx.remove(0, off);
    return !closed;
}

// ---------------------------------------------------------------- ZClientModel

QVariant ZClientModel::data(const QModelIndex &i, int role) const
{
    if (role != ClientRole || i.row() < 0 || i.row() >= m_list.size()) return {};
    return QVariant::fromValue<QObject *>(m_list[i.row()]);
}

void ZClientModel::add(ZClient *c)
{
    beginInsertRows(QModelIndex(), m_list.size(), m_list.size());
    m_list.append(c);
    endInsertRows();
}

void ZClientModel::remove(ZClient *c)
{
    const int r = m_list.indexOf(c);
    if (r < 0) return;
    beginRemoveRows(QModelIndex(), r, r);
    m_list.removeAt(r);
    endRemoveRows();
}

// ---------------------------------------------------------------- ZServer

ZServer::ZServer(uint32_t shmBytes, QObject *parent) : QObject(parent), m_shmBytes(shmBytes)
{
    connect(&m_pump, &QTimer::timeout, this, &ZServer::pumpAll);
    m_pump.start(4);
    connect(&m_reaper, &QTimer::timeout, this, &ZServer::reap);
    m_reaper.start(500);
}

QList<QObject *> ZServer::clients() const
{
    QList<QObject *> l;
    for (ZClient *c : m_clients) l.append(c);
    return l;
}

int ZServer::spawn(const QString &path)
{
    auto *c = new ZClient(m_nextId++, path, this);
    c->setWorkspace(m_workspace);
    if (!c->start(m_shmBytes)) {
        fprintf(stderr, "[zerp2] could not start %s\n", qPrintable(path));
        delete c;
        return -1;
    }
    m_clients.append(c);
    m_model.add(c);
    emit clientsChanged();
    focus(c);
    return c->id();
}

void ZServer::focus(QObject *o)
{
    auto *c = qobject_cast<ZClient *>(o);
    if (c == m_focused) return;
    if (m_focused) m_focused->setFocused(false);
    m_focused = c;
    if (c) c->setFocused(true);
    emit focusChanged();
}

void ZServer::refocus()
{
    // newest window on the current workspace
    for (int i = m_clients.size() - 1; i >= 0; i--)
        if (m_clients[i]->workspace() == m_workspace) { focus(m_clients[i]); return; }
    focus(nullptr);
}

void ZServer::setWorkspace(int w)
{
    if (w == m_workspace || w < 1 || w > 9) return;
    m_workspace = w;
    emit workspaceChanged();
    refocus();
}

void ZServer::closeFocused()
{
    if (m_focused) m_focused->close();
}

void ZServer::moveFocusedTo(int w)
{
    if (!m_focused || w < 1 || w > 9 || w == m_workspace) return;
    m_focused->setWorkspace(w);
    refocus();
}

void ZServer::toggleFullscreen()
{
    if (m_focused) m_focused->setFullscreen(!m_focused->fullscreen());
}

void ZServer::focusDirection(int dx, int dy)
{
    emit focusDirectionRequested(dx, dy);
}

int ZServer::countOn(int w) const
{
    int n = 0;
    for (ZClient *c : m_clients) if (c->workspace() == w) n++;
    return n;
}

void ZServer::pumpAll()
{
    QVector<QString> spawns;
    QVector<ZClient *> gone;
    for (ZClient *c : m_clients)
        if (!c->pump(&spawns)) gone.append(c);
    for (ZClient *c : gone) {
        fprintf(stderr, "[zerp2] client %d closed\n", c->id());
        m_clients.removeOne(c);
        m_model.remove(c);
        if (c == m_focused) m_focused = nullptr;
        c->deleteLater();
    }
    if (!gone.isEmpty()) { emit clientsChanged(); if (!m_focused) refocus(); }
    for (const QString &p : spawns) spawn(p);
}

void ZServer::reap()
{
    QVector<ZClient *> dead;
    for (ZClient *c : m_clients) if (!c->alive()) dead.append(c);
    if (dead.isEmpty()) return;
    for (ZClient *c : dead) {
        fprintf(stderr, "[zerp2] client %d exited\n", c->id());
        m_clients.removeOne(c);
        m_model.remove(c);
        if (c == m_focused) m_focused = nullptr;
        c->deleteLater();
    }
    emit clientsChanged();
    if (!m_focused) refocus();
}

// ---------------------------------------------------------------- ZTexture
//
// One GPU texture per client, updated in place with only the damaged
// rectangle, straight from the shared buffer. (Re-creating the texture
// from the whole buffer on every DAMAGE cost ~100 ms for a full-screen
// terminal and made every cursor blink a dropped frame.)
class ZTexture : public QSGTexture
{
public:
    ZTexture() : m_key(++s_keys) {}
    ~ZTexture() override { delete m_tex; }
    // unique per instance, never an address: a new texture allocated where
    // a deleted one was must not look like it to the renderer's caches
    qint64 comparisonKey() const override { return m_key; }
    QRhiTexture *rhiTexture() const override { return m_tex; }
    QSize textureSize() const override { return m_size; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }

    // GUI side (render thread, GUI blocked): what to upload next.
    // `capacity` is the texture's fixed size (the whole shared buffer as
    // a screen-sized image); `size` the part the client draws now.
    void update(const uint32_t *pixels, QSize capacity, QSize size, QRect damage)
    {
        m_pixels = pixels;
        m_size = capacity;
        if (size != m_view) { m_view = size; damage = QRect(QPoint(0, 0), size); }
        m_pending |= damage & QRect(QPoint(0, 0), size);
    }
    QSize view() const { return m_view; }

    void commitTextureOperations(QRhi *rhi, QRhiResourceUpdateBatch *u) override
    {
        if (!m_pixels || m_size.isEmpty()) return;
        if (!m_tex || m_tex->pixelSize() != m_size) {
            delete m_tex;
            m_bgra = rhi->isTextureFormatSupported(QRhiTexture::BGRA8);
            m_tex = rhi->newTexture(m_bgra ? QRhiTexture::BGRA8 : QRhiTexture::RGBA8, m_size);
            m_tex->create();
            m_pending = QRect(QPoint(0, 0), m_size);
        }
        if (m_pending.isEmpty()) return;
        // the client's buffer as an image (0xAARRGGBB words = BGRA bytes),
        // laid out at its current width
        QImage whole(reinterpret_cast<const uchar *>(m_pixels), m_view.width(), m_view.height(),
                     m_view.width() * 4, QImage::Format_RGB32);
        QRhiTextureSubresourceUploadDescription d;
        if (m_bgra) {
            d = QRhiTextureSubresourceUploadDescription(whole);
            d.setSourceTopLeft(m_pending.topLeft());
            d.setSourceSize(m_pending.size());
        } else {
            // no BGRA textures: swizzle just the damaged part
            d = QRhiTextureSubresourceUploadDescription(
                whole.copy(m_pending).convertToFormat(QImage::Format_RGBX8888));
        }
        d.setDestinationTopLeft(m_pending.topLeft());
        u->uploadTexture(m_tex, QRhiTextureUploadDescription({ 0, 0, d }));
        m_pending = QRect();
    }

private:
    QRhiTexture *m_tex = nullptr;
    QSize m_size, m_view;
    const uint32_t *m_pixels = nullptr;
    QRect m_pending;
    bool m_bgra = true;
    qint64 m_key;
    static inline qint64 s_keys = 0;
};

// ---------------------------------------------------------------- ZSurface

ZSurface::ZSurface()
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
}

void ZSurface::setClient(QObject *o)
{
    auto *c = qobject_cast<ZClient *>(o);
    if (c == m_client) return;
    if (m_client) disconnect(m_client, nullptr, this, nullptr);
    m_client = c;
    if (c) connect(c, &ZClient::damaged, this, &QQuickItem::update);
    emit clientChanged();
    update();
}

QSGNode *ZSurface::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(old);
    if (!m_client || !m_client->pixels() || m_client->bufWidth() <= 0) {
        static int warned;
        if (warned++ < 8) fprintf(stderr, "[zerp2] surface without pixels: client=%p pixels=%p w=%d\n",
                                  (void *)m_client, m_client ? (const void *)m_client->pixels() : nullptr,
                                  m_client ? m_client->bufWidth() : -1);
        delete node;
        return nullptr;
    }
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        node->setTexture(new ZTexture);
    }
    auto *tex = static_cast<ZTexture *>(node->texture());
    const QSize bufSize(m_client->bufWidth(), m_client->bufHeight());
    if (m_client->takeDirty() || tex->view() != bufSize) {
        // the texture never changes size (the renderer caches it); a
        // resized client is a different region of it
        const QScreen *scr = window()->screen();
        QSize cap = scr ? scr->size() : QSize(1920, 1080);
        if (uint64_t(cap.width()) * cap.height() * 4 > m_client->bufferBytes())
            cap = bufSize;
        tex->update(m_client->pixels(), cap, bufSize, m_client->takeDamage());
        node->markDirty(QSGNode::DirtyMaterial);
    }
    node->setSourceRect(QRectF(0, 0, bufSize.width(), bufSize.height()));
    node->setRect(boundingRect());
    return node;
}

void ZSurface::forward(const QPointF &p, Qt::MouseButtons b, bool motion)
{
    if (!m_client || width() <= 0 || height() <= 0) return;
    // item size may still be animating towards the client's size: scale
    const int x = int(p.x() * m_client->bufWidth() / width());
    const int y = int(p.y() * m_client->bufHeight() / height());
    uint32_t mask = 0;
    if (b & Qt::LeftButton) mask |= 1;
    if (b & Qt::RightButton) mask |= 2;
    if (b & Qt::MiddleButton) mask |= 4;
    m_client->sendMouse(x, y, mask, motion);
}

void ZSurface::mousePressEvent(QMouseEvent *e)
{
    emit pressed();
    forward(e->position(), e->buttons(), false);
    e->accept();
}

void ZSurface::mouseReleaseEvent(QMouseEvent *e) { forward(e->position(), e->buttons(), false); e->accept(); }
void ZSurface::mouseMoveEvent(QMouseEvent *e) { forward(e->position(), e->buttons(), true); e->accept(); }
void ZSurface::hoverMoveEvent(QHoverEvent *e) { forward(e->position(), Qt::NoButton, true); e->accept(); }

void ZSurface::wheelEvent(QWheelEvent *e)
{
    if (!m_client || width() <= 0 || height() <= 0) return;
    const int steps = -e->angleDelta().y() / 120;   // + = down, as the PS/2 wheel
    if (!steps) return;
    const int x = int(e->position().x() * m_client->bufWidth() / width());
    const int y = int(e->position().y() * m_client->bufHeight() / height());
    uint32_t mask = 0;
    if (e->buttons() & Qt::LeftButton) mask |= 1;
    mask |= uint32_t(uint8_t(int8_t(qBound(-127, steps, 127)))) << 8;
    m_client->sendMouse(x, y, mask, false);
    e->accept();
}
