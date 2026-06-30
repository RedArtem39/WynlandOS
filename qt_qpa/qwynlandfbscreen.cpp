#include "qwynlandfbscreen.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

QT_BEGIN_NAMESPACE

#include <QtGui/QPainter>
#include <QtGui/QCursor>
#include <QtGui/QPainterPath>

QWynlandFbScreen::QWynlandFbScreen()
    : m_depth(32),
      m_format(QImage::Format_ARGB32_Premultiplied),
      m_fbFd(-1),
      m_mmapAddr(nullptr),
      m_mmapSize(0),
      m_backingStore(nullptr),
      m_lastCursorPos(960, 540)
{
    // Default fallback geometry
    m_geometry = QRect(0, 0, 1920, 1080);
}

QWynlandFbScreen::~QWynlandFbScreen()
{
    if (m_mmapAddr && m_mmapAddr != MAP_FAILED) {
        munmap(m_mmapAddr, m_mmapSize);
    }
    if (m_fbFd >= 0) {
        close(m_fbFd);
    }
}

bool QWynlandFbScreen::initialize()
{
    // 1. Open the virtual frame buffer device
    m_fbFd = open("/dev/fb0", O_RDWR);
    if (m_fbFd < 0) {
        qWarning("QWynlandFbScreen: Failed to open /dev/fb0, using dummy screen.");
        return false;
    }

    // 2. Compute size and map the memory
    m_mmapSize = m_geometry.width() * m_geometry.height() * 4;
    m_mmapAddr = (uchar *)mmap(nullptr, m_mmapSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_fbFd, 0);
    if (m_mmapAddr == MAP_FAILED) {
        qWarning("QWynlandFbScreen: Failed to mmap /dev/fb0!");
        close(m_fbFd);
        m_fbFd = -1;
        return false;
    }

    // 3. Create the QImage wrapper directly on the mapped framebuffer
    m_screenImage = QImage(m_mmapAddr, m_geometry.width(), m_geometry.height(), m_geometry.width() * 4, m_format);
    
    return true;
}

void QWynlandFbScreen::updateCursor()
{
    QPoint cursorPos = QCursor::pos();
    if (m_screenImage.isNull()) return;

    QPainter painter(&m_screenImage);
    
    // 1. Restore background at last cursor position from backing store
    if (m_backingStore && !m_backingStore->isNull()) {
        QRect oldRect(m_lastCursorPos, QSize(32, 32));
        painter.drawImage(oldRect, *m_backingStore, oldRect);
    }
    
    // 2. Draw the beautiful macOS white arrow with a black outline
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    
    QPainterPath cursorPath;
    cursorPath.moveTo(cursorPos.x(), cursorPos.y());
    cursorPath.lineTo(cursorPos.x() + 15, cursorPos.y() + 15);
    cursorPath.lineTo(cursorPos.x() + 8, cursorPos.y() + 15);
    cursorPath.lineTo(cursorPos.x() + 12, cursorPos.y() + 24);
    cursorPath.lineTo(cursorPos.x() + 9, cursorPos.y() + 25);
    cursorPath.lineTo(cursorPos.x() + 5, cursorPos.y() + 16);
    cursorPath.lineTo(cursorPos.x(), cursorPos.y() + 19);
    cursorPath.closeSubpath();
    
    painter.fillPath(cursorPath, Qt::white);
    painter.strokePath(cursorPath, QPen(Qt::black, 2));
    
    painter.restore();
    
    m_lastCursorPos = cursorPos;
}

QT_END_NAMESPACE
