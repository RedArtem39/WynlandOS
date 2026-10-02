#include "qwynlandfbintegration.h"
#include "qwynlandfbscreen.h"
#include "qwynlandfbinput.h"
#include "qwynlandfb_zerpargs.h"
#include "../zerp_protocol.h"
#include <unistd.h>
#include <cstdio>
#include <QtGui/QPainter>

#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qgenericunixfontdatabase_p.h>

QT_BEGIN_NAMESPACE

class QWynlandFbBackingStore : public QPlatformBackingStore
{
public:
    QWynlandFbBackingStore(QWindow *window, QWynlandFbScreen *screen, int c2sFd)
        : QPlatformBackingStore(window), m_screen(screen), m_c2sFd(c2sFd) {}

    QPaintDevice *paintDevice() override { return &m_image; }

    void beginPaint(const QRegion &region) override {
        Q_UNUSED(region);
    }

    void endPaint() override {
    }

    void flush(QWindow *window, const QRegion &region, const QPoint &offset) override {
        Q_UNUSED(window);
        Q_UNUSED(offset);

        // Copy backing store contents directly into the client's SHM tile.
        QImage *tileImage = m_screen->image();
        if (!tileImage || tileImage->isNull()) return;

        QPainter painter(tileImage);
        for (const QRect &rect : region) {
            painter.drawImage(rect, m_image, rect);
        }
        painter.end();

        // Tell Zerp this region needs blitting to the real display.
        QRect dirty = region.boundingRect();
        ZerpMsg msg = { ZERP_MSG_DAMAGE, (quint32)dirty.x(), (quint32)dirty.y(),
                         (quint32)dirty.width(), (quint32)dirty.height() };
        if (write(m_c2sFd, &msg, sizeof(msg)) < 0) { /* Zerp gone: nothing to tell */ }
    }

    void resize(const QSize &size, const QRegion &staticContents) override {
        Q_UNUSED(staticContents);
        m_image = QImage(size, QImage::Format_ARGB32_Premultiplied);
    }

private:
    QImage m_image;
    QWynlandFbScreen *m_screen;
    int m_c2sFd;
};

QWynlandFbIntegration::QWynlandFbIntegration(const QStringList &paramList)
    : m_screen(nullptr), m_inputReader(nullptr), m_parameters(paramList),
      m_fontDb(new QGenericUnixFontDatabase), m_c2sFd(-1),
      m_native(new QPlatformNativeInterface)
{
}

QWynlandFbIntegration::~QWynlandFbIntegration()
{
    if (m_inputReader) {
        m_inputReader->stop();
        m_inputReader->wait();
        delete m_inputReader;
    }
    delete m_screen;
    delete m_fontDb;
    delete m_native;
}

bool QWynlandFbIntegration::hasCapability(QPlatformIntegration::Capability cap) const
{
    switch (cap) {
    case ThreadedPixmaps: return true;
    case MultipleWindows: return false;
    default: return QPlatformIntegration::hasCapability(cap);
    }
}

void QWynlandFbIntegration::initialize()
{
    m_c2sFd = zerp_qpa_c2s_fd();
    int s2cFd = zerp_qpa_s2c_fd();
    int shmFd = zerp_qpa_shm_fd();

    if (m_c2sFd < 0 || s2cFd < 0 || shmFd < 0) {
        qWarning("QWynlandFbIntegration: missing Zerp fds -- did the app call "
                 "zerp_qpa_capture_args(argc, argv) before constructing QApplication?");
        return;
    }

    m_screen = new QWynlandFbScreen();
    if (m_screen->initialize(shmFd, s2cFd)) {
        QWindowSystemInterface::handleScreenAdded(m_screen);
    }

    m_inputReader = new QWynlandFbInputReader();
    m_inputReader->setScreen(m_screen);
    if (m_inputReader->initialize(s2cFd)) {
        m_inputReader->start();
    }
}

/* The window always fills the tile: whatever size the app asks for (a
   QML Window sized from Screen.width before the first retile), it gets
   the tile, and Qt hears about it -- the base class's setGeometry() only
   records the rectangle, so the window never learnt it had been resized. */
class QWynlandFbWindow : public QPlatformWindow
{
public:
    QWynlandFbWindow(QWindow *window, QWynlandFbScreen *screen) : QPlatformWindow(window), m_screen(screen) {}

    void setGeometry(const QRect &) override {
        const QRect tile = m_screen ? m_screen->geometry() : QRect();
        QPlatformWindow::setGeometry(tile);
        QWindowSystemInterface::handleGeometryChange(window(), tile);
    }
    void setVisible(bool visible) override {
        QPlatformWindow::setVisible(visible);
        if (visible) {
            setGeometry(QRect());
            QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(0, 0), geometry().size()));
        }
    }

private:
    QWynlandFbScreen *m_screen;
};

QPlatformWindow *QWynlandFbIntegration::createPlatformWindow(QWindow *window) const
{
    QPlatformWindow *w = new QWynlandFbWindow(window, m_screen);
    w->requestActivateWindow();
    return w;
}

QPlatformBackingStore *QWynlandFbIntegration::createPlatformBackingStore(QWindow *window) const
{
    return new QWynlandFbBackingStore(window, m_screen, m_c2sFd);
}

QAbstractEventDispatcher *QWynlandFbIntegration::createEventDispatcher() const
{
    return createUnixEventDispatcher();
}

QPlatformFontDatabase *QWynlandFbIntegration::fontDatabase() const
{
    return m_fontDb;
}


QT_END_NAMESPACE
