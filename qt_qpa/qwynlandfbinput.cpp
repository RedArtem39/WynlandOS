#include "qwynlandfbinput.h"
#include "qwynlandfbscreen.h"
#include "../zerp_protocol.h"
#include "../zerp_layouts.h"
#include <unistd.h>
#include <qpa/qwindowsysteminterface.h>

QT_BEGIN_NAMESPACE

/* PS/2 Set 1 make-code -> Qt::Key, same 59-entry range and scancode
   convention as zerp_term.c's SCANCODE_TO_ASCII (kept in sync with it
   deliberately -- both are decoding the exact same wire format Zerp
   forwards raw from drivers/input's IRQ handler). 0 = no mapping. */
static const int SCANCODE_TO_QTKEY[59] = {
    0, Qt::Key_Escape, Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_5,
    Qt::Key_6, Qt::Key_7, Qt::Key_8, Qt::Key_9, Qt::Key_0, Qt::Key_Minus,
    Qt::Key_Equal, Qt::Key_Backspace,
    Qt::Key_Tab, Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R, Qt::Key_T,
    Qt::Key_Y, Qt::Key_U, Qt::Key_I, Qt::Key_O, Qt::Key_P,
    Qt::Key_BracketLeft, Qt::Key_BracketRight, Qt::Key_Return,
    Qt::Key_Control, Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F, Qt::Key_G,
    Qt::Key_H, Qt::Key_J, Qt::Key_K, Qt::Key_L, Qt::Key_Semicolon,
    Qt::Key_Apostrophe, Qt::Key_QuoteLeft, Qt::Key_Shift,
    Qt::Key_Backslash, Qt::Key_Z, Qt::Key_X, Qt::Key_C, Qt::Key_V, Qt::Key_B,
    Qt::Key_N, Qt::Key_M, Qt::Key_Comma, Qt::Key_Period, Qt::Key_Slash,
    Qt::Key_Shift, Qt::Key_Asterisk, Qt::Key_Alt, Qt::Key_Space
};

/* The `text` of printable keys (QLineEdit etc. need it, not just the
   Qt::Key code) comes from the layout Zerp says is on (ZERP_MSG_LAYOUT):
   zerp_layouts.h. The Qt::Key stays the US one -- shortcuts. */

/* keys past the 59-entry table: F1-F10 (0x3B-0x44), F11/F12, and the
   0xE0-prefixed navigation block */
static int extendedKey(bool e0, uint8_t code)
{
    if (e0) {
        switch (code) {
        case 0x48: return Qt::Key_Up;
        case 0x50: return Qt::Key_Down;
        case 0x4B: return Qt::Key_Left;
        case 0x4D: return Qt::Key_Right;
        case 0x47: return Qt::Key_Home;
        case 0x4F: return Qt::Key_End;
        case 0x49: return Qt::Key_PageUp;
        case 0x51: return Qt::Key_PageDown;
        case 0x52: return Qt::Key_Insert;
        case 0x53: return Qt::Key_Delete;
        case 0x1C: return Qt::Key_Enter;
        case 0x1D: return Qt::Key_Control;
        case 0x38: return Qt::Key_Alt;
        case 0x5D: return Qt::Key_Menu;
        }
        return 0;
    }
    if (code >= 0x3B && code <= 0x44) return Qt::Key_F1 + (code - 0x3B);
    if (code == 0x57) return Qt::Key_F11;
    if (code == 0x58) return Qt::Key_F12;
    return 0;
}

QWynlandFbInputReader::QWynlandFbInputReader()
    : m_s2cFd(-1), m_running(false), m_screen(nullptr), m_modifiers(Qt::NoModifier)
{
}

QWynlandFbInputReader::~QWynlandFbInputReader()
{
    m_running = false;
}

bool QWynlandFbInputReader::initialize(int s2cFd)
{
    if (s2cFd < 0) {
        qWarning("QWynlandFbInputReader: no s2c fd from Zerp");
        return false;
    }
    m_s2cFd = s2cFd;
    return true;
}

void QWynlandFbInputReader::run()
{
    m_running = true;
    Qt::MouseButtons buttons = Qt::NoButton;
    bool e0 = false;
    int layout = ZERP_LAYOUT_US;
    bool caps = false;

    while (m_running) {
        ZerpMsg msg;
        long n = read(m_s2cFd, &msg, sizeof(msg));
        if (n != (long)sizeof(msg)) {
            msleep(10);
            continue;
        }

        switch (msg.type) {
        case ZERP_MSG_TILE_RECT:
            if (m_screen) m_screen->applyTileRect(msg.x, msg.y, msg.w, msg.h);
            break;

        case ZERP_MSG_INPUT_MOUSE: {
            QPoint pos((int)msg.x, (int)msg.y);
            Qt::MouseButtons newButtons = Qt::NoButton;
            if (msg.w & 1) newButtons |= Qt::LeftButton;
            if (msg.w & 2) newButtons |= Qt::RightButton;
            if (msg.w & 4) newButtons |= Qt::MiddleButton;
            /* bits 8-15: wheel steps, positive = scroll down (zerp2) */
            const int wheel = (int8_t)((msg.w >> 8) & 0xFF);
            if (wheel) {
                QWindowSystemInterface::handleWheelEvent(nullptr, pos, pos, QPoint(),
                                                         QPoint(0, -wheel * 120), m_modifiers);
                break;
            }

            Qt::MouseButton changedButton = Qt::NoButton;
            QEvent::Type type = QEvent::MouseMove;
            if (newButtons != buttons) {
                Qt::MouseButtons changed = newButtons ^ buttons;
                if (changed & Qt::LeftButton) changedButton = Qt::LeftButton;
                else if (changed & Qt::RightButton) changedButton = Qt::RightButton;
                else if (changed & Qt::MiddleButton) changedButton = Qt::MiddleButton;
                type = (newButtons & changedButton) ? QEvent::MouseButtonPress
                                                     : QEvent::MouseButtonRelease;
            }
            QWindowSystemInterface::handleMouseEvent(nullptr, pos, pos, newButtons,
                                                       changedButton, type, m_modifiers);
            buttons = newButtons;
            break;
        }

        case ZERP_MSG_INPUT_KEY: {
            uint8_t scancode = (uint8_t)msg.x;
            if (scancode == 0xE0) { e0 = true; break; }
            bool release = (scancode & 0x80) != 0;
            uint8_t code = scancode & 0x7F;
            const bool ext = e0;
            e0 = false;

            int qtKey = ext ? 0 : (code < 59 ? SCANCODE_TO_QTKEY[code] : 0);
            if (!qtKey) qtKey = extendedKey(ext, code);
            if (qtKey == 0) break;

            if (qtKey == Qt::Key_Shift) {
                m_modifiers.setFlag(Qt::ShiftModifier, !release);
            } else if (qtKey == Qt::Key_Control) {
                m_modifiers.setFlag(Qt::ControlModifier, !release);
            } else if (qtKey == Qt::Key_Alt) {
                m_modifiers.setFlag(Qt::AltModifier, !release);
            }

            if (!ext && code == 0x3A && !release) caps = !caps;   /* Caps Lock */

            uint16_t ch = 0;
            if (!ext && code < 59 && !(m_modifiers & (Qt::ControlModifier | Qt::AltModifier)))
                ch = zerp_key_char(layout, code, (m_modifiers & Qt::ShiftModifier) != 0, caps);
            QString text = (ch != 0) ? QString(QChar(ch)) : QString();

            QWindowSystemInterface::handleKeyEvent(
                nullptr, release ? QEvent::KeyRelease : QEvent::KeyPress,
                qtKey, m_modifiers, text);
            break;
        }

        case ZERP_MSG_LAYOUT:
            if (msg.x < ZERP_LAYOUT_COUNT) layout = (int)msg.x;
            break;

        default:
            break;
        }
    }
}

QT_END_NAMESPACE
