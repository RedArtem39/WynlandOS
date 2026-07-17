#include <qpa/qplatformintegrationplugin.h>
#include "qwynlandfbintegration.h"

QT_BEGIN_NAMESPACE

class QWynlandFbIntegrationPlugin : public QPlatformIntegrationPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QPlatformIntegrationFactoryInterface_iid FILE "qwynlandfb.json")
public:
    QPlatformIntegration *create(const QString&, const QStringList&) override;
};

QPlatformIntegration *QWynlandFbIntegrationPlugin::create(const QString& system, const QStringList& paramList)
{
    if (system.toLower() == QLatin1String("qwynlandfb")) {
        return new QWynlandFbIntegration(paramList);
    }
    return nullptr;
}

QT_END_NAMESPACE

#include "qwynlandfbmain.moc"
