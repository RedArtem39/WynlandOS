#include "qwynlandfbintegration.h"
#include "qwynlandfbscreen.h"
#include "qwynlandfbinput.h"
#include <QtGui/QPainter>

#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>
#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qgenericunixfontdatabase_p.h>
#include <QtCore/QTimer>



QT_BEGIN_NAMESPACE

class QWynlandFbBackingStore : public QPlatformBackingStore
{
public:
    QWynlandFbBackingStore(QWindow *window, QWynlandFbScreen *screen)
        : QPlatformBackingStore(window), m_screen(screen) {}

    QPaintDevice *paintDevice() override { return &m_image; }

    void beginPaint(const QRegion &region) override {
        Q_UNUSED(region);
    }

    void endPaint() override {
    }

    void flush(QWindow *window, const QRegion &region, const QPoint &offset) override {
        Q_UNUSED(window);
        Q_UNUSED(offset);

        // Copy backing store contents directly to mapped framebuffer
        QImage *fbImage = m_screen->image();
        if (fbImage && !fbImage->isNull()) {
            QPainter painter(fbImage);
            for (const QRect &rect : region) {
                painter.drawImage(rect, m_image, rect);
            }
        }
    }

    void resize(const QSize &size, const QRegion &staticContents) override {
        Q_UNUSED(staticContents);
        m_image = QImage(size, QImage::Format_ARGB32_Premultiplied);
        m_screen->setBackingStore(&m_image);
    }

private:
    QImage m_image;
    QWynlandFbScreen *m_screen;
};

QWynlandFbIntegration::QWynlandFbIntegration(const QStringList &paramList)
    : m_screen(nullptr), m_inputReader(nullptr), m_parameters(paramList), m_fontDb(new QGenericUnixFontDatabase), m_cursorTimer(nullptr)
{
}

QWynlandFbIntegration::~QWynlandFbIntegration()
{
    if (m_inputReader) {
        m_inputReader->wait();
        delete m_inputReader;
    }
    delete m_screen;
    delete m_fontDb;
    delete m_cursorTimer;
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
    m_screen = new QWynlandFbScreen();
    if (m_screen->initialize()) {
        QWindowSystemInterface::handleScreenAdded(m_screen);
    }

    m_inputReader = new QWynlandFbInputReader();
    if (m_inputReader->initialize()) {
        m_inputReader->start();
    }

    // Set up a thread-safe QTimer in the main GUI thread to update the software cursor
    m_cursorTimer = new QTimer();
    m_cursorTimer->setInterval(16); // ~60 FPS
    QObject::connect(m_cursorTimer, &QTimer::timeout, [=]() {
        if (m_screen) m_screen->updateCursor();
    });
    m_cursorTimer->start();
}

QPlatformWindow *QWynlandFbIntegration::createPlatformWindow(QWindow *window) const
{
    QPlatformWindow *w = new QPlatformWindow(window);
    w->requestActivateWindow();
    return w;
}

QPlatformBackingStore *QWynlandFbIntegration::createPlatformBackingStore(QWindow *window) const
{
    return new QWynlandFbBackingStore(window, m_screen);
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
