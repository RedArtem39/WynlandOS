#include <QtCore/qglobal.h>
#include <QtCore/qtimer.h>
#include <QtCore/qdatetime.h>
#include <QtGui/qpainter.h>
#include <QtGui/qevent.h>
#include <QtGui/qpainterpath.h>
#include <QtGui/qfont.h>
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qwidget.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qgridlayout.h>
#include <time.h>
#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtWidgets/qlistwidget.h>
#include <QtGui/qfontdatabase.h>
#include <QtGui/qcursor.h>
#include <QtGui/qevent.h>




// 1. Wallpaper Widget using safe Dynamic Properties (No custom fields to prevent ABI mismatch)
class WallpaperWidget : public QWidget {
public:
    WallpaperWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setProperty("phase", 0.0);
        setProperty("styleIndex", 0);
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &WallpaperWidget::animate);
        timer->start(33); // ~30 FPS
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        int style = property("styleIndex").toInt();
        double p = property("phase").toDouble();
        double sin1 = p - (p*p*p)/6.0;
        double cos1 = 1.0 - (p*p)/2.0;

        int cx1 = width() * (0.5 + 0.25 * sin1);
        int cy1 = height() * (0.5 + 0.25 * cos1);
        int cx2 = width() * (0.4 + 0.2 * cos1);
        int cy2 = height() * (0.6 + 0.2 * sin1);

        if (style == 0) {
            // 1. Dynamic Mesh Purple & Emerald (OLED Black base)
            painter.fillRect(rect(), QColor(10, 10, 16));
            QRadialGradient grad1(cx1, cy1, width() * 0.65);
            grad1.setColorAt(0.0, QColor(138, 43, 226, 120));
            grad1.setColorAt(0.5, QColor(75, 0, 130, 35));
            grad1.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), grad1);

            QRadialGradient grad2(cx2, cy2, width() * 0.55);
            grad2.setColorAt(0.0, QColor(0, 206, 209, 110));
            grad2.setColorAt(0.6, QColor(0, 100, 80, 25));
            grad2.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), grad2);
        }
        else if (style == 1) {
            // 2. Sunset Gradient (Warm Sunset Glow)
            painter.fillRect(rect(), QColor(24, 10, 26));
            QRadialGradient grad1(cx1, cy1, width() * 0.7);
            grad1.setColorAt(0.0, QColor(255, 69, 0, 130));
            grad1.setColorAt(0.5, QColor(139, 0, 139, 40));
            grad1.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), grad1);

            QRadialGradient grad2(cx2, cy2, width() * 0.6);
            grad2.setColorAt(0.0, QColor(255, 20, 147, 100));
            grad2.setColorAt(0.6, QColor(75, 0, 130, 30));
            grad2.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), grad2);
        }
        else if (style == 2) {
            // 3. Nordic Aurora (Cold Neon Green & Cyan)
            painter.fillRect(rect(), QColor(6, 12, 18));
            QRadialGradient grad1(cx1, cy1, width() * 0.7);
            grad1.setColorAt(0.0, QColor(57, 255, 20, 90));
            grad1.setColorAt(0.6, QColor(0, 128, 128, 25));
            grad1.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), grad1);

            QRadialGradient grad2(cx2, cy2, width() * 0.5);
            grad2.setColorAt(0.0, QColor(0, 255, 255, 90));
            grad2.setColorAt(0.5, QColor(0, 0, 128, 20));
            grad2.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), grad2);
        }
        else {
            // 4. OLED Midnight Minimalist (Subtle Slate Gray)
            painter.fillRect(rect(), QColor(5, 5, 8));
            QRadialGradient grad1(width() * 0.5, height() * 0.5, width() * 0.8);
            grad1.setColorAt(0.0, QColor(255, 255, 255, 12));
            grad1.setColorAt(0.5, QColor(255, 255, 255, 2));
            grad1.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(rect(), grad1);
        }
    }
private:
    void animate() {
        double p = property("phase").toDouble() + 0.01;
        if (p > 1.5) p = -1.5;
        setProperty("phase", p);
        update();
    }
};

// 2. macOS Glass Window Frame (No custom fields, utilizes QObject properties for dragging state)
class WynWindow : public QWidget {
public:
    WynWindow(const QString &title, QWidget *parent = nullptr) : QWidget(parent) {
        setProperty("dragging", false);
        setProperty("dragPosition", QPoint(0, 0));
        resize(450, 330);
        setAttribute(Qt::WA_TranslucentBackground);

        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);

        // Titlebar
        QWidget *titleBar = new QWidget(this);
        titleBar->setFixedHeight(36);
        titleBar->setStyleSheet(
            "background-color: rgba(255, 255, 255, 0.07);"
            "border-top-left-radius: 16px;"
            "border-top-right-radius: 16px;"
        );
        QHBoxLayout *titleLayout = new QHBoxLayout(titleBar);
        titleLayout->setContentsMargins(14, 0, 14, 0);
        titleLayout->setSpacing(8);

        // Traffic Lights
        QPushButton *closeBtn = new QPushButton(titleBar);
        closeBtn->setFixedSize(12, 12);
        closeBtn->setStyleSheet("border-radius: 6px; background-color: #ff5f56; border: none;");
        connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);

        QPushButton *minBtn = new QPushButton(titleBar);
        minBtn->setFixedSize(12, 12);
        minBtn->setStyleSheet("border-radius: 6px; background-color: #ffbd2e; border: none;");
        connect(minBtn, &QPushButton::clicked, this, &QWidget::hide);

        QPushButton *maxBtn = new QPushButton(titleBar);
        maxBtn->setFixedSize(12, 12);
        maxBtn->setStyleSheet("border-radius: 6px; background-color: #27c93f; border: none;");

        titleLayout->addWidget(closeBtn);
        titleLayout->addWidget(minBtn);
        titleLayout->addWidget(maxBtn);
        titleLayout->addStretch();

        QLabel *titleLabel = new QLabel(title, titleBar);
        titleLabel->setStyleSheet("color: rgba(255, 255, 255, 0.85); font-weight: bold; font-size: 13px;");
        titleLayout->addWidget(titleLabel);
        titleLayout->addStretch();

        QWidget *spacer = new QWidget(titleBar);
        spacer->setFixedSize(52, 12);
        titleLayout->addWidget(spacer);

        mainLayout->addWidget(titleBar);

        // Content Area Container
        QWidget *contentArea = new QWidget(this);
        contentArea->setObjectName("contentArea");
        contentArea->setStyleSheet(
            "background-color: rgba(18, 18, 24, 0.65);"
            "border-bottom-left-radius: 16px;"
            "border-bottom-right-radius: 16px;"
        );
        mainLayout->addWidget(contentArea);
    }

    QWidget *contentArea() const {
        return findChild<QWidget*>("contentArea");
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(rect(), 16, 16);
        painter.fillPath(path, QColor(16, 16, 22, 140));
        
        // Translucent glass border outline
        painter.setPen(QPen(QColor(255, 255, 255, 45), 1));
        painter.drawPath(path);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && event->pos().y() < 36) {
            setProperty("dragging", true);
            QPoint dragPos = event->globalPosition().toPoint() - frameGeometry().topLeft();
            setProperty("dragPosition", dragPos);
            event->accept();
            raise();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if ((event->buttons() & Qt::LeftButton) && property("dragging").toBool()) {
            QPoint dragPos = property("dragPosition").toPoint();
            move(event->globalPosition().toPoint() - dragPos);
            event->accept();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        setProperty("dragging", false);
        event->accept();
    }
};

// 3. System Stats Monitor Widget using safe QVariantList properties
class SystemMonitor : public QWidget {
public:
    SystemMonitor(QWidget *parent = nullptr) : QWidget(parent) {
        QVariantList emptyList;
        for (int i = 0; i < 30; i++) {
            emptyList.append(0);
        }
        setProperty("cpuHistory", emptyList);
        setProperty("memHistory", emptyList);
        setProperty("diskHistory", emptyList);
        setProperty("netHistory", emptyList);

        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &SystemMonitor::updateStats);
        timer->start(1000);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QVariantList cpu = property("cpuHistory").toList();
        QVariantList mem = property("memHistory").toList();
        QVariantList disk = property("diskHistory").toList();
        QVariantList net = property("netHistory").toList();

        int w = width() / 2;
        int h = height() / 2;

        drawQuadrant(painter, QRect(5, 5, w - 10, h - 10), cpu, QColor(255, 45, 85), "CPU LOAD", "%");
        drawQuadrant(painter, QRect(w + 5, 5, w - 10, h - 10), mem, QColor(0, 122, 255), "RAM LOAD", "%");
        drawQuadrant(painter, QRect(5, h + 5, w - 10, h - 10), disk, QColor(255, 204, 0), "DISK WRITE", " KB/s");
        drawQuadrant(painter, QRect(w + 5, h + 5, w - 10, h - 10), net, QColor(52, 199, 89), "NET SPEED", " Mbps");
    }
private:
    void drawQuadrant(QPainter &painter, QRect rect, const QVariantList &history, QColor color, QString label, QString unit) {
        // Draw quadrant frame (Double-Bezel dark-glass panel)
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 8));
        painter.drawRoundedRect(rect, 10, 10);
        painter.setPen(QPen(QColor(255, 255, 255, 20), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect, 10, 10);

        // Header label & current value
        int currentVal = history.isEmpty() ? 0 : history.last().toInt();
        painter.setPen(QColor(255, 255, 255, 130));
        QFont f = painter.font();
        f.setPointSize(10);
        f.setBold(true);
        painter.setFont(f);
        painter.drawText(rect.left() + 15, rect.top() + 25, label);

        painter.setPen(color);
        f.setPointSize(15);
        painter.setFont(f);
        painter.drawText(rect.right() - 85, rect.top() + 28, QString::number(currentVal) + unit);

        // Grid lines inside graph area
        int graphTop = rect.top() + 45;
        int graphHeight = rect.height() - 60;
        painter.setPen(QPen(QColor(255, 255, 255, 8), 1, Qt::DashLine));
        for (int i = 1; i < 3; i++) {
            int gy = graphTop + graphHeight * i / 3;
            painter.drawLine(rect.left() + 15, gy, rect.right() - 15, gy);
        }

        // Draw graph path
        QPainterPath path;
        int n = history.size();
        if (n > 1) {
            double graphWidth = rect.width() - 30;
            int maxVal = (label.contains("CPU") || label.contains("RAM")) ? 100 : 120;
            for (int i = 0; i < n; i++) {
                double x = rect.left() + 15 + graphWidth * i / (n - 1);
                int val = history[i].toInt();
                if (val > maxVal) val = maxVal;
                double y = graphTop + graphHeight - (graphHeight * val / (double)maxVal);
                if (i == 0) path.moveTo(x, y);
                else path.lineTo(x, y);
            }

            // Draw line
            painter.setPen(QPen(color, 2));
            painter.drawPath(path);

            // Draw gradient fill
            QLinearGradient grad(0, graphTop, 0, graphTop + graphHeight);
            grad.setColorAt(0.0, QColor(color.red(), color.green(), color.blue(), 45));
            grad.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
            QPainterPath fillPath = path;
            fillPath.lineTo(rect.left() + 15 + graphWidth, graphTop + graphHeight);
            fillPath.lineTo(rect.left() + 15, graphTop + graphHeight);
            fillPath.closeSubpath();
            painter.setPen(Qt::NoPen);
            painter.fillPath(fillPath, grad);
        }
    }

    void updateStats() {
        QVariantList cpu = property("cpuHistory").toList();
        QVariantList mem = property("memHistory").toList();
        QVariantList disk = property("diskHistory").toList();
        QVariantList net = property("netHistory").toList();

        int cpuVal = 10 + (std::rand() % 35);
        int memVal = 44 + (std::rand() % 2);
        int diskVal = 5 + (std::rand() % 95);
        int netVal = 10 + (std::rand() % 80);

        cpu.removeFirst(); cpu.append(cpuVal);
        mem.removeFirst(); mem.append(memVal);
        disk.removeFirst(); disk.append(diskVal);
        net.removeFirst(); net.append(netVal);

        setProperty("cpuHistory", cpu);
        setProperty("memHistory", mem);
        setProperty("diskHistory", disk);
        setProperty("netHistory", net);
        update();
    }
};

// 4. Keyboard-based Notes Editor Widget
class NotesWidget : public QWidget {
public:
    NotesWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setFocusPolicy(Qt::StrongFocus);
        setProperty("currentText", QString(""));

        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(10);

        QLabel *title = new QLabel("Quick Notes (Type and press Enter to save):", this);
        title->setStyleSheet("color: rgba(255, 255, 255, 0.7); font-weight: bold; font-size: 12px;");
        layout->addWidget(title);

        QListWidget *listWidget = new QListWidget(this);
        listWidget->setObjectName("notesList");
        listWidget->setStyleSheet(
            "QListWidget {"
            "  background-color: rgba(0, 0, 0, 0.2);"
            "  border: 1px solid rgba(255, 255, 255, 0.1);"
            "  border-radius: 8px;"
            "  color: white;"
            "  font-size: 13px;"
            "  padding: 5px;"
            "}"
            "QListWidget::item {"
            "  padding: 6px 8px; border-bottom: 1px solid rgba(255, 255, 255, 0.05);"
            "}"
        );
        layout->addWidget(listWidget);

        // Prepopulate with todo items
        listWidget->addItem("• Build WynlandOS Kernel  ✅");
        listWidget->addItem("• Run Real Qt6 Applications  ✅");
        listWidget->addItem("• Implement macOS Tahoe Dock  🚀");

        QLabel *inputLabel = new QLabel("Type: ", this);
        inputLabel->setObjectName("inputLabel");
        inputLabel->setStyleSheet(
            "background-color: rgba(255, 255, 255, 0.07);"
            "border: 1px solid rgba(255, 255, 255, 0.15);"
            "color: white; border-radius: 6px; padding: 8px; font-size: 12px;"
        );
        layout->addWidget(inputLabel);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        QString currentText = property("currentText").toString();
        QLabel *inputLabel = findChild<QLabel*>("inputLabel");

        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (!currentText.isEmpty()) {
                QListWidget *listWidget = findChild<QListWidget*>("notesList");
                if (listWidget) {
                    listWidget->addItem("• " + currentText);
                }
                currentText.clear();
                if (inputLabel) inputLabel->setText("Type: ");
            }
        } else if (event->key() == Qt::Key_Backspace) {
            if (!currentText.isEmpty()) {
                currentText.chop(1);
                if (inputLabel) inputLabel->setText("Type: " + currentText);
            }
        } else {
            QString txt = event->text();
            if (!txt.isEmpty() && txt.at(0).isPrint()) {
                currentText += txt;
                if (inputLabel) inputLabel->setText("Type: " + currentText);
            }
        }
        setProperty("currentText", currentText);
        event->accept();
    }
};

class TicTacToe : public QWidget {
public:
    TicTacToe(QWidget *parent = nullptr) : QWidget(parent) {
        setProperty("turn", 'X');
        setProperty("gameOver", false);

        QGridLayout *layout = new QGridLayout();
        layout->setObjectName("buttonsLayout");
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        QLabel *status = new QLabel("Turn: Player X", this);
        status->setObjectName("statusLabel");
        status->setStyleSheet("color: white; font-weight: bold; font-size: 14px;");
        status->setAlignment(Qt::AlignCenter);

        for (int i = 0; i < 9; i++) {
            QPushButton *btn = new QPushButton("", this);
            btn->setObjectName(QString("btn_%1").arg(i));
            btn->setFixedSize(64, 64);
            btn->setStyleSheet(
                "QPushButton {"
                "  background-color: rgba(255, 255, 255, 0.08);"
                "  border: 1px solid rgba(255, 255, 255, 0.2);"
                "  border-radius: 10px;"
                "  color: white;"
                "  font-size: 22px;"
                "  font-weight: bold;"
                "}"
                "QPushButton:hover {"
                "  background-color: rgba(255, 255, 255, 0.15);"
                "}"
            );
            layout->addWidget(btn, i / 3, i % 3);
            connect(btn, &QPushButton::clicked, [=]() { makeMove(i); });
        }

        QPushButton *reset = new QPushButton("Restart", this);
        reset->setStyleSheet(
            "background-color: #0078d7; border: none; color: white;"
            "padding: 8px 16px; border-radius: 6px; font-weight: bold;"
        );
        connect(reset, &QPushButton::clicked, this, &TicTacToe::resetGame);

        QVBoxLayout *main = new QVBoxLayout(this);
        main->addWidget(status);
        main->addLayout(layout);
        main->addWidget(reset);
    }

private:
    void makeMove(int idx) {
        bool gameOver = property("gameOver").toBool();
        if (gameOver) return;

        QPushButton *btn = findChild<QPushButton*>(QString("btn_%1").arg(idx));
        if (!btn || !btn->text().isEmpty()) return;

        char turn = property("turn").toChar().toLatin1();
        btn->setText(QString(turn));
        btn->setStyleSheet(
            QString("QPushButton {"
                    "  background-color: rgba(255, 255, 255, 0.08);"
                    "  border: 1px solid rgba(255, 255, 255, 0.2);"
                    "  border-radius: 10px;"
                    "  color: %1;"
                    "  font-size: 22px;"
                    "  font-weight: bold;"
                    "}").arg(turn == 'X' ? "#ff5f56" : "#27c93f")
        );

        QLabel *status = findChild<QLabel*>("statusLabel");

        if (checkWin()) {
            if (status) status->setText(QString("Player %1 wins!").arg(turn));
            setProperty("gameOver", true);
            return;
        }

        turn = (turn == 'X') ? 'O' : 'X';
        setProperty("turn", turn);
        if (status) status->setText(QString("Turn: Player %1").arg(turn));
    }

    bool checkWin() {
        int wins[8][3] = {
            {0,1,2}, {3,4,5}, {6,7,8},
            {0,3,6}, {1,4,7}, {2,5,8},
            {0,4,8}, {2,4,6}
        };
        for (auto &w : wins) {
            QPushButton *b0 = findChild<QPushButton*>(QString("btn_%1").arg(w[0]));
            QPushButton *b1 = findChild<QPushButton*>(QString("btn_%1").arg(w[1]));
            QPushButton *b2 = findChild<QPushButton*>(QString("btn_%1").arg(w[2]));

            if (b0 && b1 && b2 && !b0->text().isEmpty() &&
                b0->text() == b1->text() &&
                b0->text() == b2->text()) {
                return true;
            }
        }
        return false;
    }

    void resetGame() {
        for (int i = 0; i < 9; i++) {
            QPushButton *btn = findChild<QPushButton*>(QString("btn_%1").arg(i));
            if (btn) {
                btn->setText("");
                btn->setStyleSheet(
                    "QPushButton {"
                    "  background-color: rgba(255, 255, 255, 0.08);"
                    "  border: 1px solid rgba(255, 255, 255, 0.2);"
                    "  border-radius: 10px;"
                    "  color: white;"
                    "  font-size: 22px;"
                    "  font-weight: bold;"
                    "}"
                );
            }
        }
        setProperty("turn", 'X');
        setProperty("gameOver", false);
        QLabel *status = findChild<QLabel*>("statusLabel");
        if (status) status->setText("Turn: Player X");
    }
};

// 6. Simple VFS File Explorer Component
class FileExplorer : public QWidget {
public:
    FileExplorer(QWidget *parent = nullptr) : QWidget(parent) {
        setProperty("currentPath", "/");
        
        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        QHBoxLayout *navLayout = new QHBoxLayout();
        QPushButton *upBtn = new QPushButton("⬆", this);
        upBtn->setFixedSize(28, 28);
        upBtn->setStyleSheet(
            "QPushButton {"
            "  background-color: rgba(255, 255, 255, 0.08); border: 1px solid rgba(255, 255, 255, 0.2);"
            "  color: white; border-radius: 6px; font-weight: bold;"
            "}"
            "QPushButton:hover { background-color: rgba(255, 255, 255, 0.15); }"
        );
        connect(upBtn, &QPushButton::clicked, [=]() { goUp(); });
        navLayout->addWidget(upBtn);

        QLabel *pathLabel = new QLabel("Current: /", this);
        pathLabel->setObjectName("pathLabel");
        pathLabel->setStyleSheet("color: rgba(255, 255, 255, 0.85); font-weight: bold; font-size: 12px;");
        navLayout->addWidget(pathLabel);
        navLayout->addStretch();
        layout->addLayout(navLayout);

        QListWidget *listWidget = new QListWidget(this);
        listWidget->setObjectName("fileList");
        listWidget->setStyleSheet(
            "QListWidget {"
            "  background-color: rgba(0, 0, 0, 0.25);"
            "  border: 1px solid rgba(255, 255, 255, 0.15);"
            "  border-radius: 8px;"
            "  color: rgba(255, 255, 255, 0.95);"
            "  font-size: 13px;"
            "  padding: 5px;"
            "}"
            "QListWidget::item {"
            "  padding: 6px 8px; border-radius: 4px;"
            "}"
            "QListWidget::item:hover {"
            "  background-color: rgba(255, 255, 255, 0.1);"
            "}"
            "QListWidget::item:selected {"
            "  background-color: rgba(0, 122, 255, 0.5); color: white;"
            "}"
        );
        connect(listWidget, &QListWidget::itemDoubleClicked, [=](QListWidgetItem *item) { onItemDoubleClicked(item); });
        connect(listWidget, &QListWidget::itemClicked, [=](QListWidgetItem *item) { onItemClicked(item); });
        layout->addWidget(listWidget);

        QLabel *statusLabel = new QLabel("Select a file or folder", this);
        statusLabel->setObjectName("statusLabel");
        statusLabel->setStyleSheet("color: rgba(255, 255, 255, 0.5); font-size: 11px;");
        layout->addWidget(statusLabel);

        refresh();
    }

    void goUp() {
        QString path = property("currentPath").toString();
        if (path == "/") return;
        int idx = path.lastIndexOf('/');
        if (idx == 0) path = "/";
        else path = path.left(idx);
        setProperty("currentPath", path);
        refresh();
    }

    void onItemDoubleClicked(QListWidgetItem *item) {
        QString name = item->text();
        if (name.startsWith("📁 ")) {
            name = name.mid(2);
            QString path = property("currentPath").toString();
            if (path == "/") path = "/" + name;
            else path = path + "/" + name;
            setProperty("currentPath", path);
            refresh();
        }
    }

    void onItemClicked(QListWidgetItem *item) {
        QString name = item->text();
        QLabel *statusLabel = findChild<QLabel*>("statusLabel");
        if (!statusLabel) return;
        if (name.startsWith("📁 ")) {
            statusLabel->setText("Directory: " + name.mid(2));
        } else if (name.startsWith("📄 ")) {
            name = name.mid(2);
            QString path = property("currentPath").toString();
            QString fullPath = (path == "/") ? "/" + name : path + "/" + name;
            QFileInfo info(fullPath);
            statusLabel->setText(QString("File: %1 (%2 bytes)").arg(name).arg(info.size()));
        }
    }

    void refresh() {
        QString path = property("currentPath").toString();
        QLabel *pathLabel = findChild<QLabel*>("pathLabel");
        if (pathLabel) pathLabel->setText("Current: " + path);

        QListWidget *listWidget = findChild<QListWidget*>("fileList");
        if (!listWidget) return;
        listWidget->clear();

        QDir dir(path);
        QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
        for (const QFileInfo &info : list) {
            QString displayName;
            if (info.isDir()) {
                displayName = "📁 " + info.fileName();
            } else {
                displayName = "📄 " + info.fileName();
            }
            listWidget->addItem(displayName);
        }
    }
};

// 7. Premium Minimalist Dock Button
class DockButton : public QPushButton {
public:
    DockButton(const QString &text, QWidget *parent = nullptr) : QPushButton(text, parent) {
        setFixedSize(70, 48);
        setStyleSheet(
            "QPushButton {"
            "  font-size: 13px;"
            "  font-weight: bold;"
            "  color: rgba(255, 255, 255, 0.75);"
            "  border: none;"
            "  border-radius: 12px;"
            "  background-color: rgba(255, 255, 255, 0.06);"
            "}"
            "QPushButton:hover {"
            "  color: white;"
            "  background-color: rgba(255, 255, 255, 0.16);"
            "}"
            "QPushButton:pressed {"
            "  background-color: rgba(255, 255, 255, 0.28);"
            "}"
        );
    }
};

class DesktopResizer : public QObject {
    QWidget *m_wallpaper;
    QWidget *m_workspace;
public:
    DesktopResizer(QWidget *wallpaper, QWidget *workspace)
        : QObject(wallpaper), m_wallpaper(wallpaper), m_workspace(workspace) {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::Resize) {
            QResizeEvent *re = static_cast<QResizeEvent*>(event);
            int w = re->size().width();
            int h = re->size().height();
            m_wallpaper->setGeometry(0, 0, w, h);
            m_workspace->setGeometry(0, 30, w, h - 30 - 80 - 15);
        }
        return false;
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    qputenv("QT_QPA_FONTDIR", "/lib/fonts");
    QApplication app(argc, argv);

    // Load DejaVu Sans font explicitly via QFontDatabase to bypass directory scanning issues
    int fontId = QFontDatabase::addApplicationFont("/lib/fonts/DejaVuSans.ttf");
    if (fontId != -1 && !QFontDatabase::applicationFontFamilies(fontId).isEmpty()) {
        QString family = QFontDatabase::applicationFontFamilies(fontId).at(0);
        app.setFont(QFont(family, 11));
    } else {
        app.setFont(QFont("DejaVu Sans", 11));
    }

    QCursor::setPos(960, 540);

    // Main desktop container
    QWidget desktop;
    desktop.setWindowTitle("WynlandOS Desktop");
    desktop.resize(1920, 1080);
    desktop.showFullScreen();

    QVBoxLayout *desktopLayout = new QVBoxLayout(&desktop);
    desktopLayout->setContentsMargins(0, 0, 0, 0);
    desktopLayout->setSpacing(0);

    // Set background wallpaper widget (shown correctly on top of desktop background)
    WallpaperWidget *wallpaper = new WallpaperWidget(&desktop);
    wallpaper->lower();

    // Top Panel (WynPanel)
    QWidget *topPanel = new QWidget(&desktop);
    topPanel->setFixedHeight(30);
    topPanel->setStyleSheet(
        "background-color: rgba(10, 10, 15, 0.4);"
        "border-bottom: 1px solid rgba(255, 255, 255, 0.1);"
    );
    QHBoxLayout *topLayout = new QHBoxLayout(topPanel);
    topLayout->setContentsMargins(20, 0, 20, 0);
    
    QPushButton *logoBtn = new QPushButton(" WynlandOS", topPanel);
    logoBtn->setStyleSheet("color: white; font-weight: bold; font-size: 13px; border: none; background: transparent; padding: 0px;");
    QObject::connect(logoBtn, &QPushButton::clicked, [=]() {
        int nextStyle = (wallpaper->property("styleIndex").toInt() + 1) % 4;
        wallpaper->setProperty("styleIndex", nextStyle);
        wallpaper->update();
    });
    topLayout->addWidget(logoBtn);

    topLayout->addStretch();
    QLabel *clockLabel = new QLabel(topPanel);
    clockLabel->setStyleSheet("color: white; font-size: 13px; font-weight: 500;");
    topLayout->addWidget(clockLabel);
    topLayout->addStretch();

    QLabel *trayLabel = new QLabel("Wi-Fi  🔋 100%", topPanel);
    trayLabel->setStyleSheet("color: rgba(255, 255, 255, 0.8); font-size: 12px;");
    topLayout->addWidget(trayLabel);

    desktopLayout->addWidget(topPanel);
    desktopLayout->addStretch();

    // Clock update timer
    QTimer *clockTimer = new QTimer(&desktop);
    QObject::connect(clockTimer, &QTimer::timeout, [=]() {
        static int h = 21, m = 30, s = 0;
        s++;
        if (s >= 60) { s = 0; m++; }
        if (m >= 60) { m = 0; h++; }
        if (h >= 24) { h = 0; }
        char buffer[80];
        sprintf(buffer, "Mon 29 Jun  %02d:%02d:%02d", h, m, s);
        clockLabel->setText(buffer);
    });
    clockTimer->start(1000);
    clockLabel->setText("Mon 29 Jun  21:30:00");

    // Main workspace container (for windows) - MUST BE EXPLICITLY SHOWN
    QWidget *workspace = new QWidget(&desktop);
    workspace->setAttribute(Qt::WA_TranslucentBackground);

    // Setup resizer event filter to dynamically adapt to any resolution
    desktop.installEventFilter(new DesktopResizer(wallpaper, workspace));



    // Create desktop windows inside workspace
    WynWindow *winMonitor = new WynWindow("System Monitor", workspace);
    winMonitor->move(100, 100);
    QVBoxLayout *monLayout = new QVBoxLayout(winMonitor->contentArea());
    monLayout->setContentsMargins(0, 0, 0, 0);
    monLayout->addWidget(new SystemMonitor(winMonitor->contentArea()));
    winMonitor->show();

    WynWindow *winText = new WynWindow("Notes / Todo List", workspace);
    winText->move(600, 100);
    QVBoxLayout *textLayout = new QVBoxLayout(winText->contentArea());
    textLayout->setContentsMargins(0, 0, 0, 0);
    NotesWidget *notes = new NotesWidget(winText->contentArea());
    textLayout->addWidget(notes);
    winText->show();

    WynWindow *winExplorer = new WynWindow("File Explorer", workspace);
    winExplorer->move(100, 500);
    QVBoxLayout *expLayout = new QVBoxLayout(winExplorer->contentArea());
    expLayout->setContentsMargins(0, 0, 0, 0);
    expLayout->addWidget(new FileExplorer(winExplorer->contentArea()));
    winExplorer->show();

    WynWindow *winGame = new WynWindow("Tic-Tac-Toe Game", workspace);
    winGame->move(600, 500);
    winGame->resize(240, 320);
    QVBoxLayout *gameLayout = new QVBoxLayout(winGame->contentArea());
    gameLayout->setContentsMargins(0, 0, 0, 0);
    gameLayout->addWidget(new TicTacToe(winGame->contentArea()));
    winGame->show();



    // Show workspace container to reveal all child windows
    workspace->show();

    // Bottom macOS-style floating dock (WynDock)
    QWidget *dockFrame = new QWidget(&desktop);
    dockFrame->setFixedHeight(68);
    dockFrame->setStyleSheet(
        "background-color: rgba(255, 255, 255, 0.07);"
        "border: 1px solid rgba(255, 255, 255, 0.15);"
        "border-radius: 20px;"
    );
    
    QHBoxLayout *dockLayout = new QHBoxLayout(dockFrame);
    dockLayout->setContentsMargins(12, 0, 12, 0);
    dockLayout->setSpacing(14);

    DockButton *btnMon = new DockButton("Stats", dockFrame);
    QObject::connect(btnMon, &QPushButton::clicked, [=]() {
        if (winMonitor->isHidden()) winMonitor->show();
        winMonitor->raise();
    });
    dockLayout->addWidget(btnMon);

    DockButton *btnText = new DockButton("Notes", dockFrame);
    QObject::connect(btnText, &QPushButton::clicked, [=]() {
        if (winText->isHidden()) winText->show();
        winText->raise();
        notes->setFocus();
    });
    dockLayout->addWidget(btnText);

    DockButton *btnExp = new DockButton("Files", dockFrame);
    QObject::connect(btnExp, &QPushButton::clicked, [=]() {
        if (winExplorer->isHidden()) winExplorer->show();
        winExplorer->raise();
    });
    dockLayout->addWidget(btnExp);

    DockButton *btnGame = new DockButton("Play", dockFrame);
    QObject::connect(btnGame, &QPushButton::clicked, [=]() {
        if (winGame->isHidden()) winGame->show();
        winGame->raise();
    });
    dockLayout->addWidget(btnGame);

    // Center dock on screen
    QWidget *dockContainer = new QWidget(&desktop);
    dockContainer->setFixedHeight(80);
    dockContainer->setFixedWidth(360);
    QHBoxLayout *containerLayout = new QHBoxLayout(dockContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->addWidget(dockFrame);

    desktopLayout->addWidget(dockContainer);
    desktopLayout->addSpacing(15);

    desktop.show();
    return app.exec();
}
