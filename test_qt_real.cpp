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

// 1. Wallpaper Widget using safe Dynamic Properties (No custom fields to prevent ABI mismatch)
class WallpaperWidget : public QWidget {
public:
    WallpaperWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setProperty("phase", 0.0);
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &WallpaperWidget::animate);
        timer->start(33); // ~30 FPS
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        double p = property("phase").toDouble();
        double sin1 = p - (p*p*p)/6.0;
        double cos1 = 1.0 - (p*p)/2.0;

        int cx1 = width() * (0.5 + 0.25 * sin1);
        int cy1 = height() * (0.5 + 0.25 * cos1);
        int cx2 = width() * (0.4 + 0.2 * cos1);
        int cy2 = height() * (0.6 + 0.2 * sin1);

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
        for (int i = 0; i < 40; i++) {
            emptyList.append(0);
        }
        setProperty("cpuHistory", emptyList);
        setProperty("memHistory", emptyList);

        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &SystemMonitor::updateStats);
        timer->start(1000);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        // Grid lines
        painter.setPen(QPen(QColor(255, 255, 255, 15), 1, Qt::DashLine));
        for (int i = 1; i < 4; i++) {
            int y = height() * i / 4;
            painter.drawLine(0, y, width(), y);
        }

        QVariantList cpu = property("cpuHistory").toList();
        QVariantList mem = property("memHistory").toList();

        drawGraph(painter, cpu, QColor(255, 0, 127), "CPU Load: " + QString::number(cpu.last().toInt()) + "%", 0);
        drawGraph(painter, mem, QColor(0, 191, 255), "RAM Load: " + QString::number(mem.last().toInt()) + "%", 25);
    }
private:
    void drawGraph(QPainter &painter, const QVariantList &history, QColor color, QString label, int yOffset) {
        QPainterPath path;
        int n = history.size();
        for (int i = 0; i < n; i++) {
            double x = width() * i / (n - 1);
            double y = height() - (height() * history[i].toInt() / 100.0);
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }

        painter.setPen(QPen(color, 2));
        painter.drawPath(path);

        QLinearGradient grad(0, 0, 0, height());
        grad.setColorAt(0.0, QColor(color.red(), color.green(), color.blue(), 45));
        grad.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
        QPainterPath fillPath = path;
        fillPath.lineTo(width(), height());
        fillPath.lineTo(0, height());
        fillPath.closeSubpath();
        painter.fillPath(fillPath, grad);

        painter.setPen(color);
        painter.drawText(15, 25 + yOffset, label);
    }

    void updateStats() {
        QVariantList cpu = property("cpuHistory").toList();
        QVariantList mem = property("memHistory").toList();

        int cpuVal = 15 + (std::rand() % 30);
        int memVal = 42 + (std::rand() % 3);

        cpu.removeFirst();
        cpu.append(cpuVal);

        mem.removeFirst();
        mem.append(memVal);

        setProperty("cpuHistory", cpu);
        setProperty("memHistory", mem);
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
        layout->setContentsMargins(15, 15, 15, 15);
        layout->setSpacing(10);

        QLabel *title = new QLabel("Quick Notes (Type directly, press Enter to add):", this);
        title->setStyleSheet("color: white; font-weight: bold; font-size: 13px;");
        layout->addWidget(title);

        QWidget *notesContainer = new QWidget(this);
        notesContainer->setObjectName("notesContainer");
        notesContainer->setStyleSheet("background-color: rgba(0,0,0,0.2); border-radius: 8px;");
        
        QVBoxLayout *notesLayout = new QVBoxLayout(notesContainer);
        notesLayout->setObjectName("notesLayout");
        notesLayout->setContentsMargins(10, 10, 10, 10);
        notesLayout->setSpacing(6);
        notesLayout->addStretch();
        layout->addWidget(notesContainer);

        QLabel *inputLabel = new QLabel("Type note: ", this);
        inputLabel->setObjectName("inputLabel");
        inputLabel->setStyleSheet(
            "background-color: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15);"
            "color: #a0a0ff; border-radius: 8px; padding: 8px; font-size: 13px; font-weight: bold;"
        );
        layout->addWidget(inputLabel);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        QString currentText = property("currentText").toString();
        QLabel *inputLabel = findChild<QLabel*>("inputLabel");

        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (!currentText.isEmpty()) {
                addNote(currentText);
                currentText.clear();
                if (inputLabel) inputLabel->setText("Type note: ");
            }
        } else if (event->key() == Qt::Key_Backspace) {
            if (!currentText.isEmpty()) {
                currentText.chop(1);
                if (inputLabel) inputLabel->setText("Type note: " + currentText);
            }
        } else {
            QString txt = event->text();
            if (!txt.isEmpty() && txt.at(0).isPrint()) {
                currentText += txt;
                if (inputLabel) inputLabel->setText("Type note: " + currentText);
            }
        }
        setProperty("currentText", currentText);
        event->accept();
    }

private:
    void addNote(const QString &text) {
        QWidget *notesContainer = findChild<QWidget*>("notesContainer");
        QVBoxLayout *notesLayout = findChild<QVBoxLayout*>("notesLayout");
        if (notesContainer && notesLayout) {
            QLabel *newNote = new QLabel("- " + text, notesContainer);
            newNote->setStyleSheet("color: rgba(255,255,255,0.9); font-size: 13px;");
            notesLayout->insertWidget(notesLayout->count() - 1, newNote);
        }
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
        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        QLabel *pathLabel = new QLabel("Current Directory: /", this);
        pathLabel->setStyleSheet("color: white; font-weight: bold; font-size: 13px;");
        layout->addWidget(pathLabel);

        QWidget *listContainer = new QWidget(this);
        listContainer->setStyleSheet(
            "background-color: rgba(0, 0, 0, 0.25);"
            "border: 1px solid rgba(255, 255, 255, 0.15);"
            "border-radius: 8px;"
        );
        QVBoxLayout *listLayout = new QVBoxLayout(listContainer);
        listLayout->setContentsMargins(12, 12, 12, 12);
        listLayout->setSpacing(8);

        const char *items[] = {
            "[DIR] .", "[DIR] ..", "[DIR] plugins", "[DIR] lib", 
            "[FILE] qtreal.elf", "[FILE] serial.log", "[FILE] qemu.log"
        };
        for (int i = 0; i < 7; i++) {
            QLabel *lbl = new QLabel(items[i], listContainer);
            lbl->setStyleSheet("color: rgba(255, 255, 255, 0.9); font-size: 13px; font-family: 'Plus Jakarta Sans', sans-serif;");
            listLayout->addWidget(lbl);
        }
        listLayout->addStretch();
        layout->addWidget(listContainer);
    }
};

// 7. macOS Dock Zoom Icon Button (Utilizes properties for scaling animation state)
class DockButton : public QPushButton {
public:
    DockButton(const QString &text, QWidget *parent = nullptr) : QPushButton(parent) {
        setProperty("scale", 1.0);
        setProperty("targetScale", 1.0);
        setFixedSize(54, 54);
        
        QTimer *timer = new QTimer(this);
        timer->setInterval(16); // ~60 FPS
        connect(timer, &QTimer::timeout, this, &DockButton::stepAnimation);

        QLabel *label = new QLabel(text, this);
        label->setStyleSheet("color: white; font-weight: bold; font-size: 24px;");
        label->setAlignment(Qt::AlignCenter);

        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(label);
        setStyleSheet("border: none; background: transparent;");
    }
protected:
    void enterEvent(QEnterEvent *) override {
        setProperty("targetScale", 1.35);
        startTimer();
    }
    void leaveEvent(QEvent *) override {
        setProperty("targetScale", 1.0);
        startTimer();
    }
private:
    void startTimer() {
        QTimer *timer = findChild<QTimer*>();
        if (timer) timer->start();
    }
    void stepAnimation() {
        double scale = property("scale").toDouble();
        double targetScale = property("targetScale").toDouble();
        double diff = targetScale - scale;
        double abs_diff = (diff < 0) ? -diff : diff;
        
        if (abs_diff < 0.01) {
            scale = targetScale;
            QTimer *timer = findChild<QTimer*>();
            if (timer) timer->stop();
        } else {
            scale += diff * 0.25;
        }
        setProperty("scale", scale);
        int size = 54 * scale;
        setFixedSize(size, size);
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    QApplication app(argc, argv);

    // Set Default TTF Font loaded from /lib/fonts/DejaVuSans.ttf
    app.setFont(QFont("DejaVu Sans", 11));

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
    wallpaper->setGeometry(0, 0, 1920, 1080);
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
    
    QLabel *logoLabel = new QLabel(" WynlandOS", topPanel);
    logoLabel->setStyleSheet("color: white; font-weight: bold; font-size: 13px;");
    topLayout->addWidget(logoLabel);

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
    workspace->setGeometry(0, 30, 1920, 980);
    workspace->setAttribute(Qt::WA_TranslucentBackground);



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

    DockButton *btnMon = new DockButton("📊", dockFrame);
    QObject::connect(btnMon, &QPushButton::clicked, [=]() {
        if (winMonitor->isHidden()) winMonitor->show();
        winMonitor->raise();
    });
    dockLayout->addWidget(btnMon);

    DockButton *btnText = new DockButton("📝", dockFrame);
    QObject::connect(btnText, &QPushButton::clicked, [=]() {
        if (winText->isHidden()) winText->show();
        winText->raise();
        notes->setFocus();
    });
    dockLayout->addWidget(btnText);

    DockButton *btnExp = new DockButton("📁", dockFrame);
    QObject::connect(btnExp, &QPushButton::clicked, [=]() {
        if (winExplorer->isHidden()) winExplorer->show();
        winExplorer->raise();
    });
    dockLayout->addWidget(btnExp);

    DockButton *btnGame = new DockButton("🎮", dockFrame);
    QObject::connect(btnGame, &QPushButton::clicked, [=]() {
        if (winGame->isHidden()) winGame->show();
        winGame->raise();
    });
    dockLayout->addWidget(btnGame);

    // Center dock on screen
    QWidget *dockContainer = new QWidget(&desktop);
    dockContainer->setFixedHeight(80);
    dockContainer->setFixedWidth(300);
    QHBoxLayout *containerLayout = new QHBoxLayout(dockContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->addWidget(dockFrame);

    desktopLayout->addWidget(dockContainer);
    desktopLayout->addSpacing(15);

    desktop.show();
    return app.exec();
}
