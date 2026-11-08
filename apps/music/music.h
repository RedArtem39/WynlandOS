// WynlandOS - Music: a small player, a Qt Quick app as a Zerp client.
// The playing (QMediaPlayer, FFmpeg underneath), the library (~/Music and
// /usr/share/wynland/music, their tags and covers), the cover's colours
// for the window's Material You theme, and MPRIS (the shell's bar, its
// dashboard and its visualizer see what plays).
#pragma once

#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtDBus/QDBusAbstractAdaptor>
#include <QtDBus/QDBusObjectPath>
#include <QtGui/QImage>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>
#include <QtQuick/QQuickImageProvider>

// the covers, for QML's Image: image://cover/<n>
class Covers : public QQuickImageProvider
{
public:
    Covers() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &want) override;
    void set(int n, const QImage &img);
private:
    QMutex m_lock;
    QHash<int, QImage> m_images;
};

class Player : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
    Q_PROPERTY(int index READ index NOTIFY indexChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString title READ title NOTIFY metaChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY metaChanged)
    Q_PROPERTY(QString cover READ cover NOTIFY metaChanged)
    Q_PROPERTY(QVariantList coverColors READ coverColors NOTIFY metaChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY modeChanged)
    Q_PROPERTY(int repeat READ repeat WRITE setRepeat NOTIFY modeChanged)   // 0 off, 1 all, 2 one

public:
    explicit Player(Covers *covers, QObject *parent = nullptr);

    QVariantList tracks() const;
    int index() const { return m_index; }
    bool playing() const { return m_player.playbackState() == QMediaPlayer::PlayingState; }
    qint64 position() const { return m_player.position(); }
    qint64 duration() const { return m_player.duration(); }
    QString title() const;
    QString artist() const;
    QString cover() const;
    QString coverFile() const;          // for MPRIS' artUrl
    QVariantList coverColors() const;
    double volume() const { return m_audio.volume(); }
    void setVolume(double v);
    bool shuffle() const { return m_shuffle; }
    void setShuffle(bool s);
    int repeat() const { return m_repeat; }
    void setRepeat(int r);

    // the index of the first of them (added now or there already); -1: none
    Q_INVOKABLE int addPaths(const QStringList &paths);
    Q_INVOKABLE void playAt(int i);
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    Q_INVOKABLE void prev();
    Q_INVOKABLE void seek(qint64 ms);

signals:
    void tracksChanged();
    void indexChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();
    void metaChanged();
    void volumeChanged();
    void modeChanged();
    void seeked(qint64 ms);

private:
    struct Track { QString path, title, artist, album; qint64 length = 0; bool cover = false, probed = false; QVariantList colors; };
    void probeNext();
    void probed();
    void scan(const QString &dir);
    void ended();
    static QVariantList colorsOf(const QImage &img);

    Covers *m_covers;
    QList<Track> m_tracks;
    int m_index = -1;
    bool m_shuffle = false;
    int m_repeat = 1;
    QMediaPlayer m_player;
    QAudioOutput m_audio;
    QMediaPlayer m_probe;               // reads the tags and covers, one track after another
    int m_probing = -1;
};

// MPRIS: org.mpris.MediaPlayer2 and its .Player, on the session bus
class MprisRoot : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ yes)
    Q_PROPERTY(bool CanRaise READ no)
    Q_PROPERTY(bool HasTrackList READ no)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ schemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ mimes)
public:
    explicit MprisRoot(QObject *parent) : QDBusAbstractAdaptor(parent) {}
    bool yes() const { return true; }
    bool no() const { return false; }
    QString identity() const { return QStringLiteral("Музыка"); }
    QString desktopEntry() const { return QStringLiteral("wynland-music"); }
    QStringList schemes() const { return { QStringLiteral("file") }; }
    QStringList mimes() const { return { QStringLiteral("audio/mpeg"), QStringLiteral("audio/mp4"), QStringLiteral("audio/ogg"), QStringLiteral("audio/flac"), QStringLiteral("audio/x-wav") }; }
public slots:
    void Raise() {}
    void Quit();
};

class MprisPlayer : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ status)
    Q_PROPERTY(QString LoopStatus READ loop WRITE setLoop)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double Rate READ one)
    Q_PROPERTY(double MinimumRate READ one)
    Q_PROPERTY(double MaximumRate READ one)
    Q_PROPERTY(bool CanGoNext READ yes)
    Q_PROPERTY(bool CanGoPrevious READ yes)
    Q_PROPERTY(bool CanPlay READ yes)
    Q_PROPERTY(bool CanPause READ yes)
    Q_PROPERTY(bool CanSeek READ yes)
    Q_PROPERTY(bool CanControl READ yes)
public:
    MprisPlayer(QObject *parent, Player *p);
    QString status() const;
    QString loop() const;
    void setLoop(const QString &l);
    bool shuffle() const { return m_p->shuffle(); }
    void setShuffle(bool s) { m_p->setShuffle(s); }
    QVariantMap metadata() const;
    double volume() const { return m_p->volume(); }
    void setVolume(double v) { m_p->setVolume(v); }
    qlonglong position() const { return m_p->position() * 1000; }
    double one() const { return 1.0; }
    bool yes() const { return true; }
public slots:
    void Next() { m_p->next(); }
    void Previous() { m_p->prev(); }
    void Pause() { m_p->pause(); }
    void PlayPause() { m_p->toggle(); }
    void Stop() { m_p->stop(); }
    void Play() { m_p->play(); }
    void Seek(qlonglong us) { m_p->seek(m_p->position() + us / 1000); }
    void SetPosition(const QDBusObjectPath &, qlonglong us) { m_p->seek(us / 1000); }
    void OpenUri(const QString &uri);
signals:
    void Seeked(qlonglong us);
private:
    void changed(const QStringList &names);
    Player *m_p;
};
