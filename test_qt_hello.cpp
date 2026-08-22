// Phase 7 checkpoint: a real Qt6 Widgets app running as a genuine Zerp
// client -- spawned by zerp.c's own spawn_client() (argv[1..3] = c2s/s2c/
// shm fd numbers), drawing into its assigned SHM tile via the rewritten
// qwynlandfb QPA plugin, statically linked in (no runtime plugin
// directory scan -- see qt_qpa/CMakeLists.txt's STATIC option).
#include <QtCore/qcoreapplication.h>
#include <QtCore/QtPlugin>
#include "qt_qpa/qwynlandfb_zerpargs.h"
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qwidget.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qboxlayout.h>

Q_IMPORT_PLUGIN(QWynlandFbIntegrationPlugin)

int main(int argc, char **argv)
{
    // Must happen before QApplication's constructor -- that's where
    // QPlatformIntegration::initialize() runs and needs these fds.
    zerp_qpa_capture_args(argc, argv);
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");

    QApplication app(argc, argv);

    QWidget window;
    window.resize(400, 200);

    QVBoxLayout *layout = new QVBoxLayout(&window);
    QLabel *label = new QLabel("Hello from real Qt6 on WynlandOS!");
    QPushButton *button = new QPushButton("Click me");
    layout->addWidget(label);
    layout->addWidget(button);

    QObject::connect(button, &QPushButton::clicked, [label]() {
        label->setText("Button was clicked!");
    });

    window.show();
    return app.exec();
}
