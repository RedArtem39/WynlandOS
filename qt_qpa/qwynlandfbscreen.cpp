#include "qwynlandfbscreen.h"
#include "../zerp_protocol.h"
#include <unistd.h>
#include <cstdio>
#include <sys/types.h>
#include <qpa/qwindowsysteminterface.h>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <qpa/qplatformwindow.h>

/* Deliberately NOT #include <sys/mman.h> -- this OS's own include/sys/mman.h
   (used by this same build for the freestanding Zerp/test binaries, and
   picked up here too since the toolchain prepends -Iinclude) declares
   mmap() as an inline stub that always returns MAP_FAILED. Real musl's
   mmap() is a normal exported libc symbol; declare it directly to bypass
   the stub header entirely. */
#ifdef __GLIBC__
/* glibc build (Ubuntu's Qt, tools/stage_qt6.sh): no stub header in the
   way, the real one has the noexcept declarations */
#include <sys/mman.h>
#else
extern "C" void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
extern "C" int munmap(void *addr, size_t length);
#endif
#define WYN_PROT_READ  1
#define WYN_PROT_WRITE 2
#define WYN_MAP_SHARED 0x01
#define WYN_MAP_FAILED ((void *)-1)

QT_BEGIN_NAMESPACE

/* Generous fixed capacity for the SHM segment -- matches zerp.c's own
   shm_bytes = screen_w * screen_h * 4 convention (a client's tile can
   never exceed the real screen size). */
static const quint32 kShmCapacityPixels = 1920u * 1080u;

QWynlandFbScreen::QWynlandFbScreen()
    : m_depth(32),
      m_format(QImage::Format_ARGB32_Premultiplied),
      m_shm(nullptr),
      m_shmCapacityPixels(0)
{
    m_geometry = QRect(0, 0, 1, 1);
}

QWynlandFbScreen::~QWynlandFbScreen()
{
    if (m_shm) {
        munmap(m_shm, (size_t)m_shmCapacityPixels * 4);
    }
}

bool QWynlandFbScreen::initialize(int shmFd, int s2cFd)
{
    if (shmFd < 0) {
        qWarning("QWynlandFbScreen: no SHM fd from Zerp (was zerp_qpa_capture_args() called?)");
        return false;
    }

    m_shm = (uint32_t *)mmap(nullptr, (size_t)kShmCapacityPixels * 4,
                              WYN_PROT_READ | WYN_PROT_WRITE, WYN_MAP_SHARED, shmFd, 0);
    if (m_shm == WYN_MAP_FAILED) {
        qWarning("QWynlandFbScreen: mmap of SHM segment failed");
        m_shm = nullptr;
        return false;
    }
    m_shmCapacityPixels = kShmCapacityPixels;

    /* Zerp always sends a TILE_RECT immediately after spawning a client --
       block (short poll loop) until it arrives so the screen has real
       geometry before Qt starts asking for it. */
    for (int tries = 0; tries < 2000; tries++) {
        ZerpMsg msg;
        long n = read(s2cFd, &msg, sizeof(msg));
        if (n == (long)sizeof(msg) && msg.type == ZERP_MSG_TILE_RECT) {
            applyTileRect(msg.x, msg.y, msg.w, msg.h);
            return true;
        }
        usleep(1000);
    }

    qWarning("QWynlandFbScreen: timed out waiting for initial TILE_RECT from Zerp");
    return false;
}

void QWynlandFbScreen::applyTileRect(quint32 x, quint32 y, quint32 w, quint32 h)
{
    Q_UNUSED(x);
    Q_UNUSED(y);
    if (w == 0 || h == 0 || !m_shm) return;
    if ((quint64)w * h > m_shmCapacityPixels) {
        /* Clamp -- should never happen (tile can't exceed screen size,
           which is <= the SHM capacity above), but never wrap a QImage
           around a stride that walks past the mapped SHM region. */
        h = m_shmCapacityPixels / w;
    }
    /* Called on the input thread (retiles) -- the GUI thread may be
       painting into m_screenImage right now, so the swap happens there.
       The first tile (initialize(), GUI thread, no event loop yet) is
       applied directly. */
    if (!qApp || (QThread::currentThread() == qApp->thread() && m_geometry.width() <= 1)) {
        resizeTo(QSize((int)w, (int)h));
        return;
    }
    const QSize size((int)w, (int)h);
    QMetaObject::invokeMethod(qApp, [this, size] { resizeTo(size); }, Qt::QueuedConnection);
}

/* GUI thread. The SHM's row stride is the tile width, so the window must
   be the same size before it paints again: tell Qt the window was resized
   (QPlatformWindow::setGeometry() alone sends no event -- the window kept
   its old size and painted with the old stride, which Zerp showed as
   smeared stripes) and have it repaint everything. */
void QWynlandFbScreen::resizeTo(const QSize &size)
{
    fprintf(stderr, "[qpa] tile %dx%d (was %dx%d)\n", size.width(), size.height(), m_geometry.width(), m_geometry.height());
    if (m_geometry.size() == size) return;
    m_geometry = QRect(QPoint(0, 0), size);
    m_screenImage = QImage((uchar *)m_shm, size.width(), size.height(), size.width() * 4, m_format);
    m_screenImage.fill(QColor(0x11, 0x14, 0x1c));   /* no stale rows while it repaints */

    QWindowSystemInterface::handleScreenGeometryChange(screen(), m_geometry, m_geometry);
    for (QWindow *w : QGuiApplication::topLevelWindows()) {
        if (QPlatformWindow *pw = w->handle()) pw->QPlatformWindow::setGeometry(m_geometry);
        QWindowSystemInterface::handleGeometryChange(w, m_geometry);
        if (w->isVisible())
            QWindowSystemInterface::handleExposeEvent(w, QRect(QPoint(0, 0), size));
    }
}

QT_END_NAMESPACE
