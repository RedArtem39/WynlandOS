// WynlandOS - Web: a WPE WebKit page as a Qt Quick item.
#pragma once

#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtQuick/QQuickPaintedItem>

typedef struct _WebKitWebView WebKitWebView;
typedef struct _WPEView WPEView;

class WebView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool secure READ secure NOTIFY urlChanged)

public:
    explicit WebView(QQuickItem *parent = nullptr);
    ~WebView() override;

    QString url() const { return m_url; }
    QString title() const { return m_title; }
    double progress() const { return m_progress; }
    bool loading() const { return m_loading; }
    bool canGoBack() const;
    bool canGoForward() const;
    QString error() const { return m_error; }
    bool secure() const { return m_url.startsWith(QLatin1String("https://")); }

    Q_INVOKABLE void load(const QString &text);   // a URL, a host, or words to search for
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void stop();

    void paint(QPainter *painter) override;

    // called from GLib callbacks
    void frame(const unsigned char *pixels, int w, int h, unsigned stride);
    void syncState();
    void setError(const QString &e);

Q_SIGNALS:
    void urlChanged();
    void titleChanged();
    void progressChanged();
    void loadingChanged();
    void historyChanged();
    void errorChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void hoverMoveEvent(QHoverEvent *e) override;
    void hoverLeaveEvent(QHoverEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    void resizeView();
    void key(QKeyEvent *e, bool down);
    void pointer(int type, Qt::MouseButton button, const QPointF &pos, Qt::KeyboardModifiers mods);

    WebKitWebView *m_web = nullptr;
    WPEView *m_view = nullptr;
    QImage m_frame;
    QString m_url, m_title, m_error;
    double m_progress = 0;
    bool m_loading = false;
    Qt::MouseButtons m_buttons;
};
