// WynlandOS - qmldemo: a Qt Quick (QML) app as a Zerp client.
//
// First step towards Zerp 2.0 on Qt Quick. Runs Ubuntu's prebuilt Qt 6
// (glibc, see tools/stage_qt6.sh) with the qwynlandfb platform plugin
// (qt_qpa/) compiled straight into this binary, and Qt Quick's software
// scene graph: no GPU needed yet, it draws into the Zerp tile like the
// Qt Widgets rofi does.
//
// Zerp spawns clients as `app <c2s fd> <s2c fd> <shm fd>`; an optional
// 4th argument names the .qml file to load.

#include <QtCore/QUrl>
#include <QtCore/QtPlugin>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>
#include <cstdio>

#include "../../qt_qpa/qwynlandfb_zerpargs.h"

Q_IMPORT_PLUGIN(QWynlandFbIntegrationPlugin)

int main(int argc, char **argv)
{
    zerp_qpa_capture_args(argc, argv);
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    qputenv("QT_QUICK_BACKEND", "software");   // no GL in the plugin yet
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    if (!qEnvironmentVariableIsSet("FONTCONFIG_FILE"))
        qputenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf");
    if (!qEnvironmentVariableIsSet("HOME"))
        qputenv("HOME", "/tmp");

    QGuiApplication app(argc, argv);

    const QString file = argc > 4 ? QString::fromLocal8Bit(argv[4])
                                  : QStringLiteral("/usr/share/zerp/qml/demo.qml");
    QQmlApplicationEngine engine;
    /* Ubuntu's QML module tree. Qt derives it from its install prefix,
       which it can't work out here (Qt lives in /lib64, and qt.conf next to
       the binary needs /proc/self/exe), so name it outright. */
    engine.addImportPath(QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &ws) {
        for (const QQmlError &w : ws)
            fprintf(stderr, "[qmldemo] %s\n", qPrintable(w.toString()));
    });
    engine.load(QUrl::fromLocalFile(file));
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "[qmldemo] FAIL: could not load %s\n", qPrintable(file));
        return 1;
    }
    fprintf(stderr, "[qmldemo] loaded %s\n", qPrintable(file));
    if (auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        QObject::connect(w, &QQuickWindow::afterRendering, w, [w] {
            static int n;
            if (n++ == 0) fprintf(stderr, "[qmldemo] first frame %dx%d\n", w->width(), w->height());
        }, Qt::DirectConnection);
    }
    return app.exec();
}
