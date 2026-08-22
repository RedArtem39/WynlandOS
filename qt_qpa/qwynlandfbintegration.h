#ifndef QWYNLANDFBINTEGRATION_H
#define QWYNLANDFBINTEGRATION_H

#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <QtCore/QFile>

QT_BEGIN_NAMESPACE

class QWynlandFbScreen;
class QWynlandFbInputReader;

class QWynlandFbIntegration : public QPlatformIntegration
{
public:
    QWynlandFbIntegration(const QStringList &paramList);
    ~QWynlandFbIntegration();

    bool hasCapability(QPlatformIntegration::Capability cap) const override;
    void initialize() override;

    QPlatformWindow *createPlatformWindow(QWindow *window) const override;
    QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override;
    QAbstractEventDispatcher *createEventDispatcher() const override;
    QPlatformFontDatabase *fontDatabase() const override;

    /* Client's own c2s pipe fd, used by the backingstore to send
       ZERP_MSG_DAMAGE after each flush -- see qwynlandfbintegration.cpp. */
    int c2sFd() const { return m_c2sFd; }

private:
    QWynlandFbScreen *m_screen;
    QWynlandFbInputReader *m_inputReader;
    QStringList m_parameters;
    QPlatformFontDatabase *m_fontDb;
    int m_c2sFd;
};

QT_END_NAMESPACE

#endif // QWYNLANDFBINTEGRATION_H
