// Phase 9: rofi-clone -- a real Qt6 Widgets Zerp client (built on Phase 7's
// static-plugin qwynlandfb), not a raw-C freestanding one. Reads
// /etc/rofi.conf ("name=path" lines), lets the user type-to-filter, and on
// Enter sends Phase 8's ZERP_MSG_SPAWN for the selected entry's path over
// its own c2s pipe (rofi IS an ordinary Zerp client, same fd convention as
// every freestanding one), then closes itself (launch-then-dismiss).
//
// Build:
//   x86_64-linux-musl-g++ -O2 -DQT_NO_DEBUG -fPIE -pie \
//     -Ibuild_qt_headers/include -Iinclude -I. \
//     -c zerp_rofi.cpp -o build/zerp_rofi.o
//   x86_64-linux-musl-g++ -O2 -fPIE -pie \
//     build/zerp_rofi.o build_qt/plugins/platforms/libqwynlandfb.a \
//     -Lbuild/lib -lQt6Widgets -lQt6Gui -lQt6Core -Wl,-rpath,/lib \
//     -o zerp_rofi.elf
#include <QtCore/qcoreapplication.h>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QStringList>
#include <QtCore/QtPlugin>
#include "qt_qpa/qwynlandfb_zerpargs.h"
#include "zerp_protocol.h"
#include <unistd.h>
#include <cstdio>

#include <QtWidgets/qapplication.h>
#include <QtWidgets/qwidget.h>
#include <QtWidgets/qlineedit.h>
#include <QtWidgets/qlistwidget.h>
#include <QtWidgets/qboxlayout.h>

Q_IMPORT_PLUGIN(QWynlandFbIntegrationPlugin)

struct RofiEntry {
    QString name;
    QString path;
};

static QVector<RofiEntry> loadEntries()
{
    QVector<RofiEntry> entries;
    QFile f("/etc/rofi.conf");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine();
            int eq = line.indexOf('=');
            if (eq > 0) {
                RofiEntry e;
                e.name = line.left(eq);
                e.path = line.mid(eq + 1);
                if (!e.name.isEmpty() && !e.path.isEmpty()) entries.push_back(e);
            }
        }
    }
    return entries;
}

class RofiWindow : public QWidget {
public:
    RofiWindow(int c2sFd) : m_c2sFd(c2sFd), m_entries(loadEntries())
    {
        auto *layout = new QVBoxLayout(this);
        m_search = new QLineEdit(this);
        m_list = new QListWidget(this);
        layout->addWidget(m_search);
        layout->addWidget(m_list);

        connect(m_search, &QLineEdit::textChanged, this, &RofiWindow::applyFilter);
        connect(m_search, &QLineEdit::returnPressed, this, &RofiWindow::launchSelected);
        connect(m_list, &QListWidget::itemActivated, this, &RofiWindow::launchSelected);

        applyFilter(QString());
        m_search->setFocus();
    }

private:
    void applyFilter(const QString &needle)
    {
        m_list->clear();
        for (const RofiEntry &e : m_entries) {
            if (needle.isEmpty() || e.name.contains(needle, Qt::CaseInsensitive)) {
                auto *item = new QListWidgetItem(e.name, m_list);
                item->setData(Qt::UserRole, e.path);
            }
        }
        if (m_list->count() > 0) m_list->setCurrentRow(0);
    }

    void launchSelected()
    {
        write(2, "ROFIDBG: launchSelected called\n", 32);
        QListWidgetItem *item = m_list->currentItem();
        if (!item) { write(2, "ROFIDBG: no current item\n", 26); return; }
        QByteArray path = item->data(Qt::UserRole).toString().toUtf8();

        ZerpSpawnMsg spawnMsg;
        spawnMsg.type = ZERP_MSG_SPAWN;
        int n = path.size();
        if (n > (int)sizeof(spawnMsg.path) - 1) n = (int)sizeof(spawnMsg.path) - 1;
        for (int i = 0; i < n; i++) spawnMsg.path[i] = path[i];
        spawnMsg.path[n] = '\0';
        char dbg[300];
        int dbgLen = snprintf(dbg, sizeof(dbg), "ROFIDBG: sending spawn path='%s' c2sFd=%d\n", spawnMsg.path, m_c2sFd);
        write(2, dbg, dbgLen);
        long wrote1 = write(m_c2sFd, &spawnMsg, sizeof(spawnMsg));
        dbgLen = snprintf(dbg, sizeof(dbg), "ROFIDBG: spawn write returned %ld (expected %zu)\n", wrote1, sizeof(spawnMsg));
        write(2, dbg, dbgLen);

        ZerpMsg closeMsg = { ZERP_MSG_CLOSE, 0, 0, 0, 0 };
        long wrote2 = write(m_c2sFd, &closeMsg, sizeof(closeMsg));
        dbgLen = snprintf(dbg, sizeof(dbg), "ROFIDBG: close write returned %ld\n", wrote2);
        write(2, dbg, dbgLen);

        qApp->quit();
    }

    int m_c2sFd;
    QVector<RofiEntry> m_entries;
    QLineEdit *m_search;
    QListWidget *m_list;
};

int main(int argc, char **argv)
{
    zerp_qpa_capture_args(argc, argv);
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    qputenv("QT_QPA_FONTDIR", "/lib/fonts");

    QApplication app(argc, argv);

    RofiWindow window(zerp_qpa_c2s_fd());
    window.resize(400, 300);
    window.show();

    return app.exec();
}
