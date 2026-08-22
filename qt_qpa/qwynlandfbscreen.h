#ifndef QWYNLANDFBSCREEN_H
#define QWYNLANDFBSCREEN_H

#include <qpa/qplatformscreen.h>
#include <QtGui/QImage>

QT_BEGIN_NAMESPACE

/* Zerp gives each client its own private tile -- this "screen" is really
   just that client's SHM buffer, sized to its currently assigned tile,
   not the real display. See qwynlandfb_zerpargs.h for how the SHM fd is
   obtained. */
class QWynlandFbScreen : public QPlatformScreen
{
public:
    QWynlandFbScreen();
    ~QWynlandFbScreen();

    bool initialize(int shmFd, int s2cFd);

    QRect geometry() const override { return m_geometry; }
    int depth() const override { return m_depth; }
    QImage::Format format() const override { return m_format; }
    QImage *image() { return &m_screenImage; }

    QDpi logicalDpi() const override { return QDpi(96, 96); }

    /* Applies a new ZERP_MSG_TILE_RECT (the tile was resized/moved by a
       retile()) -- rebuilds the QImage wrapper over the same SHM pointer
       with the new dimensions and notifies Qt of the geometry change. */
    void applyTileRect(quint32 x, quint32 y, quint32 w, quint32 h);

private:
    QRect m_geometry;
    int m_depth;
    QImage::Format m_format;
    uint32_t *m_shm;
    quint32 m_shmCapacityPixels;
    QImage m_screenImage;
};

QT_END_NAMESPACE

#endif // QWYNLANDFBSCREEN_H
