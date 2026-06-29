#include <QtCore/qglobal.h>
#include <QtCore/qtimer.h>
#include <QtCore/qdatetime.h>
#include <QtGui/qpainter.h>
#include <QtGui/qevent.h>
#include <QtGui/qpainterpath.h>
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qwidget.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qgridlayout.h>
#include <time.h>

// 1. Wallpaper Event Filter
class WallpaperFilter : public QObject {
public:
    WallpaperFilter(QObject *parent) : QObject(parent), m_phase(0.0) {}
    
    void step() {
        m_phase += 0.01;
        if (m_phase > 1.5) m_phase = -1.5;
    }
    
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            QWidget *w = qobject_cast<QWidget*>(obj);
            if (!w) return false;
            
            QPainter painter(w);
            painter.setRenderHint(QPainter::Antialiasing);

            double p = m_phase;
            double sin1 = p - (p*p*p)/6.0;
            double cos1 = 1.0 - (p*p)/2.0;

            int cx1 = w->width() * (0.5 + 0.25 * sin1);
            int cy1 = w->height() * (0.5 + 0.25 * cos1);
            int cx2 = w->width() * (0.4 + 0.2 * cos1);
            int cy2 = w->height() * (0.6 + 0.2 * sin1);

            painter.fillRect(w->rect(), QColor(10, 10, 16));

            QRadialGradient grad1(cx1, cy1, w->width() * 0.65);
            grad1.setColorAt(0.0, QColor(138, 43, 226, 120));
            grad1.setColorAt(0.5, QColor(75, 0, 130, 35));
            grad1.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(w->rect(), grad1);

            QRadialGradient grad2(cx2, cy2, w->width() * 0.55);
            grad2.setColorAt(0.0, QColor(0, 206, 209, 110));
            grad2.setColorAt(0.6, QColor(0, 100, 80, 25));
            grad2.setColorAt(1.0, QColor(0, 0, 0, 0));
            painter.fillRect(w->rect(), grad2);
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
    
private:
    double m_phase;
};

// 2. Window Frame Event Filter for dragging and translucent border painting
class WindowFilter : public QObject {
public:
    WindowFilter(QWidget *window) 
        : QObject(window), m_window(window), m_dragging(false) {}
        
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            QPainter painter(m_window);
            painter.setRenderHint(QPainter::Antialiasing);
            
            // Draw window background shape
            QPainterPath path;
            path.addRoundedRect(m_window->rect(), 16, 16);
            painter.fillPath(path, QColor(16, 16, 22, 140));
            
            // Draw slight border outline
            painter.setPen(QPen(QColor(255, 255, 255, 45), 1));
            painter.drawPath(path);
            return true;
        }
        else if (event->type() == QEvent::MouseButtonPress) {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton && me->pos().y() < 36) {
                m_dragging = true;
                m_dragPosition = me->globalPosition().toPoint() - m_window->frameGeometry().topLeft();
                m_window->raise();
                return true;
            }
        }
        else if (event->type() == QEvent::MouseMove) {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            if ((me->buttons() & Qt::LeftButton) && m_dragging) {
                m_window->move(me->globalPosition().toPoint() - m_dragPosition);
                return true;
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease) {
            m_dragging = false;
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
    
private:
    QWidget *m_window;
    bool m_dragging;
    QPoint m_dragPosition;
};

// Helper function to create safe WynWindow container using composition
QWidget *createWynWindow(const QString &title, QWidget *workspace, int x, int y, int w, int h, QWidget **contentAreaOut) {
    QWidget *win = new QWidget(workspace);
    win->setGeometry(x, y, w, h);
    win->setAttribute(Qt::WA_TranslucentBackground);
    
    WindowFilter *filter = new WindowFilter(win);
    win->installEventFilter(filter);
    
    QVBoxLayout *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    // Titlebar
    QWidget *titleBar = new QWidget(win);
    titleBar->setFixedHeight(36);
    titleBar->setStyleSheet("background-color: rgba(255,255,255,0.07); border-top-left-radius: 16px; border-top-right-radius: 16px;");
    QHBoxLayout *titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(14, 0, 14, 0);
    titleLayout->setSpacing(8);
    
    QPushButton *closeBtn = new QPushButton(titleBar);
    closeBtn->setFixedSize(12, 12);
    closeBtn->setStyleSheet("border-radius: 6px; background-color: #ff5f56; border: none;");
    QObject::connect(closeBtn, &QPushButton::clicked, win, &QWidget::close);
    
    QPushButton *minBtn = new QPushButton(titleBar);
    minBtn->setFixedSize(12, 12);
    minBtn->setStyleSheet("border-radius: 6px; background-color: #ffbd2e; border: none;");
    QObject::connect(minBtn, &QPushButton::clicked, win, &QWidget::hide);
    
    QPushButton *maxBtn = new QPushButton(titleBar);
    maxBtn->setFixedSize(12, 12);
    maxBtn->setStyleSheet("border-radius: 6px; background-color: #27c93f; border: none;");
    
    titleLayout->addWidget(closeBtn);
    titleLayout->addWidget(minBtn);
    titleLayout->addWidget(maxBtn);
    titleLayout->addStretch();
    
    QLabel *lbl = new QLabel(title, titleBar);
    lbl->setStyleSheet("color: rgba(255,255,255,0.85); font-weight: bold; font-size: 13px;");
    titleLayout->addWidget(lbl);
    titleLayout->addStretch();
    
    QWidget *spacer = new QWidget(titleBar);
    spacer->setFixedSize(52, 12);
    titleLayout->addWidget(spacer);
    
    layout->addWidget(titleBar);
    
    QWidget *content = new QWidget(win);
    content->setStyleSheet("background-color: rgba(18,18,24,0.65); border-bottom-left-radius: 16px; border-bottom-right-radius: 16px;");
    layout->addWidget(content);
    
    *contentAreaOut = content;
    return win;
}

// 3. System Stats Monitor Filter
class MonitorFilter : public QObject {
public:
    MonitorFilter(QWidget *widget) : QObject(widget), m_widget(widget) {
        for (int i = 0; i < 40; i++) {
            m_cpuHistory[i] = 0;
            m_memHistory[i] = 0;
        }
    }
    
    void updateStats() {
        int cpu = 15 + (std::rand() % 30);
        int mem = 42 + (std::rand() % 3);
        for (int i = 0; i < 39; i++) {
            m_cpuHistory[i] = m_cpuHistory[i + 1];
            m_memHistory[i] = m_memHistory[i + 1];
        }
        m_cpuHistory[39] = cpu;
        m_memHistory[39] = mem;
        m_widget->update();
    }
    
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::Paint) {
            QPainter painter(m_widget);
            painter.setRenderHint(QPainter::Antialiasing);
            
            // Grid lines
            painter.setPen(QPen(QColor(255, 255, 255, 15), 1, Qt::DashLine));
            for (int i = 1; i < 4; i++) {
                int y = m_widget->height() * i / 4;
                painter.drawLine(0, y, m_widget->width(), y);
            }
            
            drawGraph(painter, m_cpuHistory, QColor(255, 0, 127), "CPU Load: " + QString::number(m_cpuHistory[39]) + "%", 0);
            drawGraph(painter, m_memHistory, QColor(0, 191, 255), "RAM Load: " + QString::number(m_memHistory[39]) + "%", 25);
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
    
private:
    void drawGraph(QPainter &painter, const int *history, QColor color, QString label, int yOffset) {
        QPainterPath path;
        int n = 40;
        for (int i = 0; i < n; i++) {
            double x = m_widget->width() * i / (n - 1);
            double y = m_widget->height() - (m_widget->height() * history[i] / 100.0);
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }

        painter.setPen(QPen(color, 2));
        painter.drawPath(path);

        QLinearGradient grad(0, 0, 0, m_widget->height());
        grad.setColorAt(0.0, QColor(color.red(), color.green(), color.blue(), 45));
        grad.setColorAt(1.0, QColor(color.red(), color.green(), color.blue(), 0));
        QPainterPath fillPath = path;
        fillPath.lineTo(m_widget->width(), m_widget->height());
        fillPath.lineTo(0, m_widget->height());
        fillPath.closeSubpath();
        painter.fillPath(fillPath, grad);

        painter.setPen(color);
        painter.drawText(15, 25 + yOffset, label);
    }
    
    QWidget *m_widget;
    int m_cpuHistory[40];
    int m_memHistory[40];
};

// 4. Keyboard-based Notes Editor Filter
class NotesFilter : public QObject {
public:
    NotesFilter(QWidget *container, QVBoxLayout *notesLayout, QLabel *inputLabel) 
        : QObject(container), m_container(container), m_notesLayout(notesLayout), m_inputLabel(inputLabel) {}
        
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::KeyPress) {
            QKeyEvent *ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                if (!m_currentText.isEmpty()) {
                    addNote(m_currentText);
                    m_currentText.clear();
                    m_inputLabel->setText("Type note: ");
                }
            } else if (ke->key() == Qt::Key_Backspace) {
                if (!m_currentText.isEmpty()) {
                    m_currentText.chop(1);
                    m_inputLabel->setText("Type note: " + m_currentText);
                }
            } else {
                QString txt = ke->text();
                if (!txt.isEmpty() && txt.at(0).isPrint()) {
                    m_currentText += txt;
                    m_inputLabel->setText("Type note: " + m_currentText);
                }
            }
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
    
private:
    void addNote(const QString &text) {
        QLabel *newNote = new QLabel("- " + text, m_container);
        newNote->setStyleSheet("color: rgba(255,255,255,0.9); font-size: 13px;");
        m_notesLayout->insertWidget(m_notesLayout->count() - 1, newNote);
    }
    
    QWidget *m_container;
    QVBoxLayout *m_notesLayout;
    QLabel *m_inputLabel;
    QString m_currentText;
};

// 5. Dock Button Zoom Event Filter
class DockButtonFilter : public QObject {
public:
    DockButtonFilter(QPushButton *btn) 
        : QObject(btn), m_btn(btn), m_scale(1.0), m_targetScale(1.0) 
    {
        m_timer = new QTimer(this);
        m_timer->setInterval(16);
        QObject::connect(m_timer, &QTimer::timeout, [=]() {
            double diff = m_targetScale - m_scale;
            double abs_diff = (diff < 0) ? -diff : diff;
            if (abs_diff < 0.01) {
                m_scale = m_targetScale;
                m_timer->stop();
            } else {
                m_scale += diff * 0.25;
            }
            int size = 54 * m_scale;
            m_btn->setFixedSize(size, size);
        });
    }
    
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::Enter) {
            m_targetScale = 1.35;
            m_timer->start();
        } else if (event->type() == QEvent::Leave) {
            m_targetScale = 1.0;
            m_timer->start();
        }
        return QObject::eventFilter(obj, event);
    }
    
private:
    QPushButton *m_btn;
    double m_scale;
    double m_targetScale;
    QTimer *m_timer;
};

// 6. Tic-Tac-Toe Game Controller Class (Not inherited from QWidget)
class TicTacToeController : public QObject {
public:
    TicTacToeController(QWidget *container) : QObject(container), m_turn('X'), m_gameOver(false) {
        QGridLayout *layout = new QGridLayout(container);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        m_status = new QLabel("Turn: Player X", container);
        m_status->setStyleSheet("color: white; font-weight: bold; font-size: 14px;");
        m_status->setAlignment(Qt::AlignCenter);

        for (int i = 0; i < 9; i++) {
            m_buttons[i] = new QPushButton("", container);
            m_buttons[i]->setFixedSize(64, 64);
            m_buttons[i]->setStyleSheet(
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
            layout->addWidget(m_buttons[i], i / 3, i % 3);
            QObject::connect(m_buttons[i], &QPushButton::clicked, [=]() { makeMove(i); });
        }

        QPushButton *reset = new QPushButton("Restart", container);
        reset->setStyleSheet(
            "background-color: #0078d7; border: none; color: white;"
            "padding: 8px 16px; border-radius: 6px; font-weight: bold;"
        );
        QObject::connect(reset, &QPushButton::clicked, [=]() { resetGame(); });

        QVBoxLayout *main = new QVBoxLayout(container);
        main->addWidget(m_status);
        main->addLayout(layout);
        main->addWidget(reset);
    }
    
private:
    void makeMove(int idx) {
        if (m_gameOver || !m_buttons[idx]->text().isEmpty()) return;
        
        m_buttons[idx]->setText(QString(m_turn));
        m_buttons[idx]->setStyleSheet(
            QString("QPushButton {"
                    "  background-color: rgba(255, 255, 255, 0.08);"
                    "  border: 1px solid rgba(255, 255, 255, 0.2);"
                    "  border-radius: 10px;"
                    "  color: %1;"
                    "  font-size: 22px;"
                    "  font-weight: bold;"
                    "}").arg(m_turn == 'X' ? "#ff5f56" : "#27c93f")
        );

        if (checkWin()) {
            m_status->setText(QString("Player %1 wins!").arg(m_turn));
            m_gameOver = true;
            return;
        }

        m_turn = (m_turn == 'X') ? 'O' : 'X';
        m_status->setText(QString("Turn: Player %1").arg(m_turn));
    }

    bool checkWin() {
        int wins[8][3] = {
            {0,1,2}, {3,4,5}, {6,7,8},
            {0,3,6}, {1,4,7}, {2,5,8},
            {0,4,8}, {2,4,6}
        };
        for (auto &w : wins) {
            if (!m_buttons[w[0]]->text().isEmpty() &&
                m_buttons[w[0]]->text() == m_buttons[w[1]]->text() &&
                m_buttons[w[0]]->text() == m_buttons[w[2]]->text()) {
                return true;
            }
        }
        return false;
    }

    void resetGame() {
        for (int i = 0; i < 9; i++) {
            m_buttons[i]->setText("");
            m_buttons[i]->setStyleSheet(
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
        m_turn = 'X';
        m_gameOver = false;
        m_status->setText("Turn: Player X");
    }

    QPushButton *m_buttons[9];
    QLabel *m_status;
    char m_turn;
    bool m_gameOver;
};

// 7. Simple VFS File Explorer Controller
class FileExplorerController : public QObject {
public:
    FileExplorerController(QWidget *container) : QObject(container) {
        QVBoxLayout *layout = new QVBoxLayout(container);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        m_pathLabel = new QLabel("Current Directory: /", container);
        m_pathLabel->setStyleSheet("color: white; font-weight: bold; font-size: 13px;");
        layout->addWidget(m_pathLabel);

        QWidget *listContainer = new QWidget(container);
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
private:
    QLabel *m_pathLabel;
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    QApplication app(argc, argv);

    // Main desktop container
    QWidget desktop;
    desktop.setWindowTitle("WynlandOS Desktop");
    desktop.resize(1920, 1080);
    desktop.showFullScreen();

    QVBoxLayout *desktopLayout = new QVBoxLayout(&desktop);
    desktopLayout->setContentsMargins(0, 0, 0, 0);
    desktopLayout->setSpacing(0);

    // Set background wallpaper widget
    QWidget *wallpaper = new QWidget(&desktop);
    wallpaper->setGeometry(0, 0, 1920, 1080);
    WallpaperFilter *wallFilter = new WallpaperFilter(wallpaper);
    wallpaper->installEventFilter(wallFilter);
    wallpaper->lower();
    
    QTimer *wallTimer = new QTimer(&desktop);
    QObject::connect(wallTimer, &QTimer::timeout, [=]() {
        wallFilter->step();
        wallpaper->update();
    });
    wallTimer->start(33);

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
    logoLabel->setStyleSheet("color: white; font-weight: bold; font-family: 'Plus Jakarta Sans', sans-serif; font-size: 13px;");
    topLayout->addWidget(logoLabel);

    topLayout->addStretch();
    QLabel *clockLabel = new QLabel(topPanel);
    clockLabel->setStyleSheet("color: white; font-family: 'Plus Jakarta Sans', sans-serif; font-size: 13px; font-weight: 500;");
    topLayout->addWidget(clockLabel);
    topLayout->addStretch();

    QLabel *trayLabel = new QLabel("Wi-Fi  🔋 100%", topPanel);
    trayLabel->setStyleSheet("color: rgba(255, 255, 255, 0.8); font-family: 'Plus Jakarta Sans', sans-serif; font-size: 12px;");
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

    // Main workspace container (for windows)
    QWidget *workspace = new QWidget(&desktop);
    workspace->resize(1920, 980);
    workspace->move(0, 30);
    workspace->setAttribute(Qt::WA_TranslucentBackground);

    // Create desktop windows safely
    QWidget *monContent = nullptr;
    QWidget *winMonitor = createWynWindow("System Monitor", workspace, 100, 100, 450, 330, &monContent);
    QVBoxLayout *monLayout = new QVBoxLayout(monContent);
    monLayout->setContentsMargins(0, 0, 0, 0);
    QWidget *statsWidget = new QWidget(monContent);
    MonitorFilter *monFilter = new MonitorFilter(statsWidget);
    statsWidget->installEventFilter(monFilter);
    monLayout->addWidget(statsWidget);
    
    QTimer *statsTimer = new QTimer(&desktop);
    QObject::connect(statsTimer, &QTimer::timeout, [=]() { monFilter->updateStats(); });
    statsTimer->start(1000);
    winMonitor->show();

    // Todo List notes window
    QWidget *notesContent = nullptr;
    QWidget *winText = createWynWindow("Notes / Todo List", workspace, 600, 100, 450, 330, &notesContent);
    winText->setFocusPolicy(Qt::StrongFocus);
    QVBoxLayout *textLayout = new QVBoxLayout(notesContent);
    textLayout->setContentsMargins(15, 15, 15, 15);
    textLayout->setSpacing(10);

    QLabel *notesTitle = new QLabel("Quick Notes (Type directly, press Enter to add):", notesContent);
    notesTitle->setStyleSheet("color: white; font-weight: bold; font-size: 13px;");
    textLayout->addWidget(notesTitle);

    QWidget *notesContainer = new QWidget(notesContent);
    notesContainer->setStyleSheet("background-color: rgba(0,0,0,0.2); border-radius: 8px;");
    QVBoxLayout *notesListLayout = new QVBoxLayout(notesContainer);
    notesListLayout->setContentsMargins(10, 10, 10, 10);
    notesListLayout->setSpacing(6);
    notesListLayout->addStretch();
    textLayout->addWidget(notesContainer);

    QLabel *inputLabel = new QLabel("Type note: ", notesContent);
    inputLabel->setStyleSheet(
        "background-color: rgba(255,255,255,0.08); border: 1px solid rgba(255,255,255,0.15);"
        "color: #a0a0ff; border-radius: 8px; padding: 8px; font-size: 13px; font-weight: bold;"
    );
    textLayout->addWidget(inputLabel);

    NotesFilter *notesFilter = new NotesFilter(notesContainer, notesListLayout, inputLabel);
    winText->installEventFilter(notesFilter);
    winText->show();

    // File Explorer Window
    QWidget *expContent = nullptr;
    QWidget *winExplorer = createWynWindow("File Explorer", workspace, 100, 500, 450, 330, &expContent);
    QVBoxLayout *expLayout = new QVBoxLayout(expContent);
    expLayout->setContentsMargins(0, 0, 0, 0);
    new FileExplorerController(expContent);
    winExplorer->show();

    // Tic-Tac-Toe Game Window
    QWidget *gameContent = nullptr;
    QWidget *winGame = createWynWindow("Tic-Tac-Toe Game", workspace, 600, 500, 240, 320, &gameContent);
    QVBoxLayout *gameLayout = new QVBoxLayout(gameContent);
    gameLayout->setContentsMargins(0, 0, 0, 0);
    new TicTacToeController(gameContent);
    winGame->show();

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

    QPushButton *btnMon = new QPushButton("📊", dockFrame);
    btnMon->setFixedSize(54, 54);
    btnMon->setStyleSheet("border: none; font-size: 24px; background: transparent;");
    btnMon->installEventFilter(new DockButtonFilter(btnMon));
    QObject::connect(btnMon, &QPushButton::clicked, [=]() {
        if (winMonitor->isHidden()) winMonitor->show();
        winMonitor->raise();
    });
    dockLayout->addWidget(btnMon);

    QPushButton *btnText = new QPushButton("📝", dockFrame);
    btnText->setFixedSize(54, 54);
    btnText->setStyleSheet("border: none; font-size: 24px; background: transparent;");
    btnText->installEventFilter(new DockButtonFilter(btnText));
    QObject::connect(btnText, &QPushButton::clicked, [=]() {
        if (winText->isHidden()) winText->show();
        winText->raise();
        winText->setFocus();
    });
    dockLayout->addWidget(btnText);

    QPushButton *btnExp = new QPushButton("📁", dockFrame);
    btnExp->setFixedSize(54, 54);
    btnExp->setStyleSheet("border: none; font-size: 24px; background: transparent;");
    btnExp->installEventFilter(new DockButtonFilter(btnExp));
    QObject::connect(btnExp, &QPushButton::clicked, [=]() {
        if (winExplorer->isHidden()) winExplorer->show();
        winExplorer->raise();
    });
    dockLayout->addWidget(btnExp);

    QPushButton *btnGame = new QPushButton("🎮", dockFrame);
    btnGame->setFixedSize(54, 54);
    btnGame->setStyleSheet("border: none; font-size: 24px; background: transparent;");
    btnGame->installEventFilter(new DockButtonFilter(btnGame));
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
