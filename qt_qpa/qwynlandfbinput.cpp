#include "qwynlandfbinput.h"
#include "qwynlandfbscreen.h"
#include <fcntl.h>
#include <unistd.h>
#include <qpa/qwindowsysteminterface.h>

QT_BEGIN_NAMESPACE

QWynlandFbInputReader::QWynlandFbInputReader()
    : m_mouseFd(-1),
      m_kbdFd(-1),
      m_cursorPos(960, 540),
      m_running(false),
      m_screen(nullptr)
{
}

QWynlandFbInputReader::~QWynlandFbInputReader()
{
    m_running = false;
    if (m_mouseFd >= 0) {
        close(m_mouseFd);
    }
}

bool QWynlandFbInputReader::initialize()
{
    m_mouseFd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
    if (m_mouseFd < 0) {
        qWarning("QWynlandFbInputReader: Failed to open /dev/input/mice");
        return false;
    }
    return true;
}

void QWynlandFbInputReader::run()
{
    m_running = true;
    uint8_t packet[3];
    Qt::MouseButtons buttons = Qt::NoButton;

    // Set initial cursor position to center of screen
    if (m_screen) {
        m_cursorPos = QPoint(m_screen->geometry().width() / 2, m_screen->geometry().height() / 2);
    }

    while (m_running) {
        int bytes = read(m_mouseFd, packet, 3);
        if (bytes == 3) {
            // 1. Decode relative coordinates from PS/2 mouse packet
            int8_t rel_x = (int8_t)packet[1];
            int8_t rel_y = (int8_t)packet[2];

            int maxX = 1919;
            int maxY = 1079;
            if (m_screen) {
                QRect geom = m_screen->geometry();
                maxX = geom.width() - 1;
                maxY = geom.height() - 1;
            }

            m_cursorPos.setX(qBound(0, m_cursorPos.x() + rel_x, maxX));
            m_cursorPos.setY(qBound(0, m_cursorPos.y() - rel_y, maxY)); // Invert Y for screen coordinates

            // 2. Decode button states
            Qt::MouseButtons newButtons = Qt::NoButton;
            if (packet[0] & 1) newButtons |= Qt::LeftButton;
            if (packet[0] & 2) newButtons |= Qt::RightButton;
            if (packet[0] & 4) newButtons |= Qt::MiddleButton;

            // 3. Determine changed buttons, event type, and target button
            Qt::MouseButton button = Qt::NoButton;
            QEvent::Type type = QEvent::MouseMove;

            if (newButtons != buttons) {
                Qt::MouseButtons changed = newButtons ^ buttons;
                if (changed & Qt::LeftButton) button = Qt::LeftButton;
                else if (changed & Qt::RightButton) button = Qt::RightButton;
                else if (changed & Qt::MiddleButton) button = Qt::MiddleButton;

                if (newButtons & button) {
                    type = QEvent::MouseButtonPress;
                } else {
                    type = QEvent::MouseButtonRelease;
                }
            }

            // 4. Dispatch mouse event to Qt Window System Interface
            QWindowSystemInterface::handleMouseEvent(nullptr, m_cursorPos, m_cursorPos, newButtons, button, type);
            buttons = newButtons;
        } else {
            // Read error, EOF, or partial read: yield thread to prevent 100% CPU spinning
            msleep(10);
        }
    }
}

QT_END_NAMESPACE
