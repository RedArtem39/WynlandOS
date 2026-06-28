#include "qwynlandfbscreen.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

QT_BEGIN_NAMESPACE

QWynlandFbScreen::QWynlandFbScreen()
    : m_depth(32),
      m_format(QImage::Format_ARGB32_Premultiplied),
      m_fbFd(-1),
      m_mmapAddr(nullptr),
      m_mmapSize(0)
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

QT_END_NAMESPACE
