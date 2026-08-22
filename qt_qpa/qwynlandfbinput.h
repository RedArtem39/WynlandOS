#ifndef QWYNLANDFBINPUT_H
#define QWYNLANDFBINPUT_H

#include <QtCore/QThread>
#include <QtCore/QPoint>

QT_BEGIN_NAMESPACE

class QWynlandFbScreen;

/* Reads ZerpMsg records off the client's s2c pipe (mouse/key input, plus
   TILE_RECT resize notifications) and translates them into real Qt
   events. Zerp's pipes are non-blocking ring buffers (see zerp_client.h),
   so this is a poll-with-short-sleep loop, same pattern the original
   /dev/input/mice reader here already used. */
class QWynlandFbInputReader : public QThread
{
    Q_OBJECT
public:
    QWynlandFbInputReader();
    ~QWynlandFbInputReader();

    bool initialize(int s2cFd);
    void setScreen(QWynlandFbScreen *screen) { m_screen = screen; }
    void stop() { m_running = false; }

protected:
    void run() override;

private:
    int m_s2cFd;
    bool m_running;
    QWynlandFbScreen *m_screen;
    Qt::KeyboardModifiers m_modifiers;
};

QT_END_NAMESPACE

#endif // QWYNLANDFBINPUT_H
