// WynlandOS - Music: a small player, a Qt Quick app as a Zerp client.
//
// Same plumbing as apps/files (the qwynlandfb QPA plugin compiled in, the
// software scene graph, Zerp's fds in argv[1..3]); the window is
// /usr/share/zerp/qml/music/Main.qml. Files given on the command line are
// played first; the library is ~/Music and /usr/share/wynland/music.

#include "music.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtCore/QtPlugin>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtGui/QGuiApplication>
#include <QtMultimedia/QMediaMetaData>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <cstdio>
#include <unistd.h>

#include "../../qt_qpa/qwynlandfb_zerpargs.h"

Q_IMPORT_PLUGIN(QWynlandFbIntegrationPlugin)

static const QStringList kAudio = { "mp3", "m4a", "aac", "ogg", "oga", "opus", "flac", "wav", "wma", "webm", "mka" };

// ---------------------------------------------------------------- Covers

QImage Covers::requestImage(const QString &id, QSize *size, const QSize &want)
{
    QMutexLocker l(&m_lock);
    QImage img = m_images.value(id.section(QLatin1Char('?'), 0, 0).toInt());
    if (size) *size = img.size();
    if (!img.isNull() && want.isValid() && want.width() > 0 && want.height() > 0)
        img = img.scaled(want, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    return img;
}

void Covers::set(int n, const QImage &img)
{
    QMutexLocker l(&m_lock);
    m_images.insert(n, img.width() > 640 ? img.scaled(640, 640, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation) : img);
}

// ---------------------------------------------------------------- Player

Player::Player(Covers *covers, QObject *parent) : QObject(parent), m_covers(covers)
{
    m_player.setAudioOutput(&m_audio);
    m_audio.setVolume(.8f);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &Player::playingChanged);
    connect(&m_player, &QMediaPlayer::positionChanged, this, &Player::positionChanged);
    connect(&m_player, &QMediaPlayer::durationChanged, this, [this] {
        if (m_index >= 0 && m_index < m_tracks.size() && m_player.duration() > 0) m_tracks[m_index].length = m_player.duration();
        emit durationChanged(); emit metaChanged();
    });
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus s) {
        if (s == QMediaPlayer::EndOfMedia) ended();
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &e) {
        fprintf(stderr, "[music] %s\n", qPrintable(e));
    });
    connect(&m_audio, &QAudioOutput::volumeChanged, this, &Player::volumeChanged);
    connect(&m_probe, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus s) {
        if (s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::InvalidMedia) probed();
    });

    const QString home = QDir::homePath();
    scan(home + QStringLiteral("/Music"));
    scan(QStringLiteral("/usr/share/wynland/music"));
}

void Player::scan(const QString &dir)
{
    QStringList found;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString f = it.next();
        if (kAudio.contains(QFileInfo(f).suffix().toLower())) found << f;
    }
    found.sort();
    addPaths(found);
}

int Player::addPaths(const QStringList &paths)
{
    bool added = false;
    int first = -1;
    for (const QString &p0 : paths) {
        const QString p = QFileInfo(p0.startsWith(QLatin1String("file://")) ? QUrl(p0).toLocalFile() : p0).absoluteFilePath();
        if (!QFileInfo(p).isFile()) continue;
        int at = -1;
        for (int i = 0; i < m_tracks.size(); i++) if (m_tracks[i].path == p) { at = i; break; }
        if (at >= 0) { if (first < 0) first = at; continue; }
        if (first < 0) first = m_tracks.size();
        Track t;
        t.path = p;
        t.title = QFileInfo(p).completeBaseName();
        m_tracks << t;
        added = true;
    }
    if (!added) return first;
    emit tracksChanged();
    if (m_index < 0 && !m_tracks.isEmpty()) {
        m_index = 0;
        m_player.setSource(QUrl::fromLocalFile(m_tracks[0].path));
        emit indexChanged(); emit metaChanged();
    }
    if (m_probing < 0) { m_probing = -1; probeNext(); }
    return first;
}

// the tags and the cover of the next track not read yet
void Player::probeNext()
{
    for (int i = 0; i < m_tracks.size(); i++) {
        if (!m_tracks[i].probed && i > m_probing) {
            m_probing = i;
            m_probe.setSource(QUrl::fromLocalFile(m_tracks[i].path));
            return;
        }
    }
    m_probing = -1;
    m_probe.setSource(QUrl());
}

void Player::probed()
{
    const int i = m_probing;
    if (i >= 0 && i < m_tracks.size()) {
        const QMediaMetaData md = m_probe.metaData();
        Track &t = m_tracks[i];
        t.probed = true;
        const QString title = md.stringValue(QMediaMetaData::Title);
        if (!title.isEmpty()) t.title = title;
        QString artist = md.stringValue(QMediaMetaData::ContributingArtist);
        if (artist.isEmpty()) artist = md.stringValue(QMediaMetaData::AlbumArtist);
        t.artist = artist.isEmpty() ? QStringLiteral(" ") : artist;
        t.album = md.stringValue(QMediaMetaData::AlbumTitle);
        if (m_probe.duration() > 0) t.length = m_probe.duration();
        QImage img = md.value(QMediaMetaData::ThumbnailImage).value<QImage>();
        if (img.isNull()) img = md.value(QMediaMetaData::CoverArtImage).value<QImage>();
        // beside it, as downloaders keep them (Resonance, yt-dlp): NAME.meta.json
        // or NAME.info.json for the title and the artist, NAME.webp/.jpg/.png
        // for the cover -- when the file itself has none
        const QFileInfo fi(t.path);
        const QString base = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName();
        for (const char *ext : { ".meta.json", ".info.json" }) {
            QFile jf(base + QLatin1String(ext));
            if (!jf.open(QIODevice::ReadOnly)) continue;
            const QJsonObject o = QJsonDocument::fromJson(jf.readAll()).object();
            const QString jt = o.value(QStringLiteral("title")).toString(), ja = o.value(QStringLiteral("artist")).toString();
            if (title.isEmpty() && !jt.isEmpty()) t.title = jt;
            if (artist.isEmpty() && !ja.isEmpty()) t.artist = ja;
            else if (artist.isEmpty() && !o.value(QStringLiteral("uploader")).toString().isEmpty()) t.artist = o.value(QStringLiteral("uploader")).toString();
            break;
        }
        for (const char *ext : { ".webp", ".jpg", ".jpeg", ".png" }) {
            if (!img.isNull()) break;
            img.load(base + QLatin1String(ext));
        }
        if (img.isNull()) {
            for (const char *n : { "cover.jpg", "cover.png", "folder.jpg", "front.jpg" }) {
                if (img.load(fi.absolutePath() + QLatin1Char('/') + QLatin1String(n))) break;
            }
        }
        if (!img.isNull()) {
            t.cover = true;
            m_covers->set(i, img);
            t.colors = colorsOf(img);
            img.save(QStringLiteral("/tmp/wynland-music-%1-%2.png").arg(getuid()).arg(i));
        }
        emit tracksChanged();
        if (i == m_index) emit metaChanged();
    }
    QMetaObject::invokeMethod(this, [this] { probeNext(); }, Qt::QueuedConnection);
}

// the cover's colours for Material You's Score: [argb, count] of 12-bit bins
QVariantList Player::colorsOf(const QImage &src)
{
    const QImage img = src.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
    QHash<int, int> bins;
    QHash<int, quint64> sr, sg, sb;
    for (int y = 0; y < img.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x++) {
            const QRgb c = line[x];
            const int k = ((qRed(c) >> 4) << 8) | ((qGreen(c) >> 4) << 4) | (qBlue(c) >> 4);
            bins[k]++; sr[k] += qRed(c); sg[k] += qGreen(c); sb[k] += qBlue(c);
        }
    }
    QVariantList out;
    for (auto it = bins.cbegin(); it != bins.cend(); ++it) {
        const int n = it.value(), k = it.key();
        const quint32 argb = 0xff000000u | (quint32(sr[k] / n) << 16) | (quint32(sg[k] / n) << 8) | quint32(sb[k] / n);
        out << QVariant::fromValue(QVariantList{ double(argb), n });
    }
    return out;
}

QVariantList Player::tracks() const
{
    QVariantList l;
    for (int i = 0; i < m_tracks.size(); i++) {
        const Track &t = m_tracks[i];
        l << QVariantMap{ { "title", t.title }, { "artist", t.artist.trimmed() }, { "length", t.length },
                          { "cover", t.cover ? QStringLiteral("image://cover/%1").arg(i) : QString() } };
    }
    return l;
}

QString Player::title() const { return m_index >= 0 && m_index < m_tracks.size() ? m_tracks[m_index].title : QString(); }
QString Player::artist() const { return m_index >= 0 && m_index < m_tracks.size() ? m_tracks[m_index].artist.trimmed() : QString(); }
QString Player::cover() const
{
    return m_index >= 0 && m_index < m_tracks.size() && m_tracks[m_index].cover ? QStringLiteral("image://cover/%1").arg(m_index) : QString();
}
QString Player::coverFile() const
{
    return m_index >= 0 && m_index < m_tracks.size() && m_tracks[m_index].cover
        ? QStringLiteral("file:///tmp/wynland-music-%1-%2.png").arg(getuid()).arg(m_index) : QString();
}
QVariantList Player::coverColors() const { return m_index >= 0 && m_index < m_tracks.size() ? m_tracks[m_index].colors : QVariantList(); }

void Player::setVolume(double v) { m_audio.setVolume(float(qBound(0.0, v, 1.0))); }
void Player::setShuffle(bool s) { if (s != m_shuffle) { m_shuffle = s; emit modeChanged(); } }
void Player::setRepeat(int r) { r = qBound(0, r, 2); if (r != m_repeat) { m_repeat = r; emit modeChanged(); } }

void Player::playAt(int i)
{
    if (i < 0 || i >= m_tracks.size()) return;
    if (i != m_index) {
        m_index = i;
        m_player.setSource(QUrl::fromLocalFile(m_tracks[i].path));
        emit indexChanged(); emit metaChanged();
    }
    m_player.play();
}
void Player::toggle() { playing() ? pause() : play(); }
void Player::play() { if (m_index < 0 && !m_tracks.isEmpty()) playAt(0); else m_player.play(); }
void Player::pause() { m_player.pause(); }
void Player::stop() { m_player.stop(); }
void Player::seek(qint64 ms) { m_player.setPosition(qBound<qint64>(0, ms, qMax<qint64>(0, duration()))); emit seeked(m_player.position()); }
void Player::next()
{
    if (m_tracks.isEmpty()) return;
    if (m_shuffle && m_tracks.size() > 1) {
        int n;
        do n = int(QRandomGenerator::global()->bounded(m_tracks.size())); while (n == m_index);
        playAt(n);
    } else playAt((m_index + 1) % m_tracks.size());
}
void Player::prev()
{
    if (m_tracks.isEmpty()) return;
    if (position() > 3000) { seek(0); return; }        // (as every player: back to its start first)
    playAt((m_index - 1 + m_tracks.size()) % m_tracks.size());
}
void Player::ended()
{
    if (m_repeat == 2) { seek(0); m_player.play(); return; }
    if (!m_shuffle && m_index == m_tracks.size() - 1 && m_repeat == 0) { m_player.stop(); return; }
    next();
}

// ---------------------------------------------------------------- MPRIS

void MprisRoot::Quit() { QCoreApplication::quit(); }

MprisPlayer::MprisPlayer(QObject *parent, Player *p) : QDBusAbstractAdaptor(parent), m_p(p)
{
    setAutoRelaySignals(false);
    connect(p, &Player::playingChanged, this, [this] { changed({ "PlaybackStatus" }); });
    connect(p, &Player::metaChanged, this, [this] { changed({ "Metadata" }); });
    connect(p, &Player::durationChanged, this, [this] { changed({ "Metadata" }); });
    connect(p, &Player::volumeChanged, this, [this] { changed({ "Volume" }); });
    connect(p, &Player::modeChanged, this, [this] { changed({ "LoopStatus", "Shuffle" }); });
    connect(p, &Player::seeked, this, [this](qint64 ms) { emit Seeked(ms * 1000); });
}

QString MprisPlayer::status() const
{
    switch (m_p->playing() ? 1 : m_p->index() >= 0 && m_p->position() > 0 ? 2 : 0) {
    case 1: return QStringLiteral("Playing");
    case 2: return QStringLiteral("Paused");
    default: return QStringLiteral("Stopped");
    }
}
QString MprisPlayer::loop() const { return m_p->repeat() == 2 ? QStringLiteral("Track") : m_p->repeat() == 1 ? QStringLiteral("Playlist") : QStringLiteral("None"); }
void MprisPlayer::setLoop(const QString &l) { m_p->setRepeat(l == QLatin1String("Track") ? 2 : l == QLatin1String("Playlist") ? 1 : 0); }

QVariantMap MprisPlayer::metadata() const
{
    QVariantMap m;
    if (m_p->index() < 0) return m;
    m.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/wynland/music/track/%1").arg(m_p->index()))));
    if (m_p->duration() > 0) m.insert(QStringLiteral("mpris:length"), qlonglong(m_p->duration()) * 1000);
    m.insert(QStringLiteral("xesam:title"), m_p->title());
    if (!m_p->artist().isEmpty()) m.insert(QStringLiteral("xesam:artist"), QStringList{ m_p->artist() });
    if (!m_p->coverFile().isEmpty()) m.insert(QStringLiteral("mpris:artUrl"), m_p->coverFile());
    return m;
}

void MprisPlayer::OpenUri(const QString &uri)
{
    m_p->playAt(m_p->addPaths({ uri }));
}

void MprisPlayer::changed(const QStringList &names)
{
    QVariantMap props;
    for (const QString &n : names) {
        if (n == QLatin1String("PlaybackStatus")) props.insert(n, status());
        else if (n == QLatin1String("Metadata")) props.insert(n, metadata());
        else if (n == QLatin1String("Volume")) props.insert(n, volume());
        else if (n == QLatin1String("LoopStatus")) props.insert(n, loop());
        else if (n == QLatin1String("Shuffle")) props.insert(n, shuffle());
    }
    QDBusMessage sig = QDBusMessage::createSignal(QStringLiteral("/org/mpris/MediaPlayer2"),
                                                  QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
    sig << QStringLiteral("org.mpris.MediaPlayer2.Player") << props << QStringList();
    QDBusConnection::sessionBus().send(sig);
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
    const int zerpArgs = zerp_qpa_capture_args(argc, argv);   // Zerp's fds first, when it started us
    // (MUSIC_HOST=1: on a development host, Qt's own platform and the QML
    // from MUSIC_QML -- to try it without the OS)
    const bool host = qEnvironmentVariableIsSet("MUSIC_HOST");
    if (!host) qputenv("QT_QPA_PLATFORM", zerp_qpa_platform());
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    if (!qEnvironmentVariableIsSet("FONTCONFIG_FILE"))
        qputenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf");
    if (!qEnvironmentVariableIsSet("HOME"))
        qputenv("HOME", "/home/user");

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("wynland-music"));
    app.setDesktopFileName(QStringLiteral("wynland-music"));

    auto *covers = new Covers;
    Player player(covers);
    QStringList args;
    for (int i = 1 + zerpArgs; i < argc; i++) args << QString::fromLocal8Bit(argv[i]);
    const int asked = args.isEmpty() ? -1 : player.addPaths(args);

    // MPRIS on the session bus (no bus: the player still plays)
    QObject mprisObj;
    new MprisRoot(&mprisObj);
    new MprisPlayer(&mprisObj, &player);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        bus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), &mprisObj, QDBusConnection::ExportAdaptors);
        if (!bus.registerService(QStringLiteral("org.mpris.MediaPlayer2.wynland_music")))
            fprintf(stderr, "[music] MPRIS: the name is taken\n");
    } else fprintf(stderr, "[music] MPRIS: no session bus\n");

    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml"));
    engine.addImageProvider(QStringLiteral("cover"), covers);
    engine.rootContext()->setContextProperty(QStringLiteral("player"), &player);
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &ws) {
        for (const QQmlError &w : ws) fprintf(stderr, "[music] %s\n", qPrintable(w.toString()));
    });
    const QString qml = host && qEnvironmentVariableIsSet("MUSIC_QML") ? qEnvironmentVariable("MUSIC_QML")
                                                                        : QStringLiteral("/usr/share/zerp/qml/music/Main.qml");
    engine.load(QUrl::fromLocalFile(qml));
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "[music] FAIL: could not load %s\n", qPrintable(qml));
        return 1;
    }
    if (asked >= 0) player.playAt(asked);         // the file asked for, playing
    fprintf(stderr, "[music] up\n");
    return app.exec();
}
