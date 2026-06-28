#ifndef QWYNLANDFBINTEGRATION_H
#define QWYNLANDFBINTEGRATION_H

#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <QtCore/QThread>
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

private:
    QWynlandFbScreen *m_screen;
    QWynlandFbInputReader *m_inputReader;
    QStringList m_parameters;
};

QT_END_NAMESPACE

#endif // QWYNLANDFBINTEGRATION_H
