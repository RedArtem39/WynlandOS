#ifndef QWYNLANDFBSCREEN_H
#define QWYNLANDFBSCREEN_H

#include <qpa/qplatformscreen.h>
#include <QtGui/QImage>

QT_BEGIN_NAMESPACE

class QWynlandFbScreen : public QPlatformScreen
{
public:
    QWynlandFbScreen();
    ~QWynlandFbScreen();

    bool initialize();

    QRect geometry() const override { return m_geometry; }
    int depth() const override { return m_depth; }
    QImage::Format format() const override { return m_format; }
    QImage *image() { return &m_screenImage; }

    // Physical screen dimensions in millimeters
    QDpi logicalDpi() const override { return QDpi(96, 96); }

    void setBackingStore(QImage *img) { m_backingStore = img; }
    void updateCursor();

private:
    QRect m_geometry;
    int m_depth;
    QImage::Format m_format;
    int m_fbFd;
    uchar *m_mmapAddr;
    ulong m_mmapSize;
    QImage m_screenImage;
    QImage *m_backingStore;
    QPoint m_lastCursorPos;
};

QT_END_NAMESPACE

#endif // QWYNLANDFBSCREEN_H
