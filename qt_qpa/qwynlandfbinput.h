#ifndef QWYNLANDFBINPUT_H
#define QWYNLANDFBINPUT_H

#include <QtCore/QThread>
#include <QtCore/QPoint>

QT_BEGIN_NAMESPACE

class QWynlandFbScreen;

class QWynlandFbInputReader : public QThread
{
    Q_OBJECT
public:
    QWynlandFbInputReader();
    ~QWynlandFbInputReader();

    bool initialize();
    void setScreen(QWynlandFbScreen *screen) { m_screen = screen; }

protected:
    void run() override;

private:
    int m_mouseFd;
    int m_kbdFd;
    QPoint m_cursorPos;
    bool m_running;
    QWynlandFbScreen *m_screen;
};

QT_END_NAMESPACE

#endif // QWYNLANDFBINPUT_H
