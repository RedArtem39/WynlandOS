// WynlandOS - Files: the file manager, a Qt Quick app as a Zerp client.
//
// Same plumbing as apps/qml/qmldemo.cpp (qwynlandfb QPA plugin compiled
// in, software scene graph, fds from Zerp in argv[1..3]); the UI is
// /usr/share/zerp/qml/files/Main.qml, the file system work is Fs below.

#include "fs.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>
#include <QtCore/QtPlugin>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "../../qt_qpa/qwynlandfb_zerpargs.h"
#include "../../zerp_protocol.h"

Q_IMPORT_PLUGIN(QWynlandFbIntegrationPlugin)

// ---------------------------------------------------------------- Fs

static QString kindOf(const QFileInfo &fi, mode_t mode)
{
    if (fi.isDir()) return QStringLiteral("dir");
    static const QStringList images = { "png", "jpg", "jpeg", "gif", "bmp", "svg", "webp", "ico" };
    static const QStringList archives = { "zip", "tar", "gz", "xz", "bz2", "zst", "7z", "deb" };
    static const QStringList texts = { "txt", "md", "c", "h", "cpp", "hpp", "py", "sh", "js", "json",
                                       "qml", "conf", "cfg", "ini", "toml", "yaml", "yml", "xml",
                                       "html", "css", "log", "wyn", "rs", "go", "lua", "asm", "s" };
    const QString ext = fi.suffix().toLower();
    if (images.contains(ext)) return QStringLiteral("image");
    if (archives.contains(ext)) return QStringLiteral("archive");
    if (texts.contains(ext)) return QStringLiteral("text");
    if (mode & 0111) return QStringLiteral("exec");
    if (ext == QLatin1String("elf") || ext == QLatin1String("so")) return QStringLiteral("exec");
    // no extension: text if the first bytes look like text
    QFile f(fi.filePath());
    if (fi.size() > 0 && f.open(QIODevice::ReadOnly)) {
        const QByteArray head = f.read(512);
        if (head.startsWith("\x7f" "ELF")) return QStringLiteral("exec");
        bool text = true;
        for (char c : head)
            if ((unsigned char)c < 9 || ((unsigned char)c > 13 && (unsigned char)c < 32)) { text = false; break; }
        if (text) return QStringLiteral("text");
    }
    return QStringLiteral("other");
}

static QVariantMap entry(const QFileInfo &fi)
{
    struct stat st;
    mode_t mode = 0;
    uid_t owner = 0;
    if (::stat(QFile::encodeName(fi.filePath()).constData(), &st) == 0) { mode = st.st_mode; owner = st.st_uid; }
    const QDateTime mt = fi.lastModified();
    QVariantMap m;
    m[QStringLiteral("name")] = fi.fileName();
    m[QStringLiteral("path")] = fi.filePath();
    m[QStringLiteral("dir")] = fi.isDir();
    m[QStringLiteral("size")] = double(fi.isDir() ? 0 : fi.size());
    m[QStringLiteral("mtime")] = double(mt.isValid() && mt.toSecsSinceEpoch() > 86400 ? mt.toMSecsSinceEpoch() : 0);
    m[QStringLiteral("mode")] = int(mode & 07777);
    m[QStringLiteral("owner")] = int(owner);
    m[QStringLiteral("link")] = fi.isSymLink();
    m[QStringLiteral("kind")] = kindOf(fi, mode);
    return m;
}

int Fs::uid() const { return int(::getuid()); }

bool Fs::fail(const QString &what)
{
    const int e = errno;
    m_error = what;
    if (e == EACCES || e == EPERM)
        m_error += QStringLiteral(": permission denied") + (::getuid() ? QStringLiteral(" (ary su in a terminal for root)") : QString());
    else if (e)
        m_error += QStringLiteral(": ") + QString::fromLocal8Bit(strerror(e));
    emit lastErrorChanged();
    return false;
}

QVariantList Fs::list(const QString &dir, bool hidden)
{
    QDir d(dir);
    QDir::Filters f = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (hidden) f |= QDir::Hidden;
    errno = 0;
    if (!d.exists() || !d.isReadable()) { fail(QStringLiteral("Can't open ") + dir); return {}; }
    const QFileInfoList infos = d.entryInfoList(f, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    QVariantList out;
    out.reserve(infos.size());
    for (const QFileInfo &fi : infos) out.append(entry(fi));
    return out;
}

QVariantMap Fs::info(const QString &path) { return entry(QFileInfo(path)); }
bool Fs::exists(const QString &path) const { return QFileInfo::exists(path); }
bool Fs::isDir(const QString &path) const { return QFileInfo(path).isDir(); }

QString Fs::parentOf(const QString &path) const
{
    if (path == QLatin1String("/")) return path;
    const QString p = QFileInfo(path).path();
    return p.isEmpty() ? QStringLiteral("/") : p;
}

QString Fs::join(const QString &dir, const QString &name) const
{
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

bool Fs::makeDir(const QString &path)
{
    errno = 0;
    if (::mkdir(QFile::encodeName(path).constData(), 0755) == 0) return true;
    return fail(QStringLiteral("Can't create ") + QFileInfo(path).fileName());
}

bool Fs::makeFile(const QString &path)
{
    QFile f(path);
    errno = 0;
    if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return true;
    return fail(QStringLiteral("Can't create ") + QFileInfo(path).fileName());
}

bool Fs::rename(const QString &from, const QString &to)
{
    if (from == to) return true;
    if (QFileInfo::exists(to)) { errno = EEXIST; return fail(QFileInfo(to).fileName() + QStringLiteral(" already exists")); }
    errno = 0;
    if (::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0) return true;
    return fail(QStringLiteral("Can't rename ") + QFileInfo(from).fileName());
}

bool Fs::remove(const QString &path)
{
    const QFileInfo fi(path);
    errno = 0;
    if (fi.isDir() && !fi.isSymLink()) {
        QDir d(path);
        const QFileInfoList kids = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &k : kids)
            if (!remove(k.filePath())) return false;
        if (::rmdir(QFile::encodeName(path).constData()) == 0) return true;
    } else if (::unlink(QFile::encodeName(path).constData()) == 0) {
        return true;
    }
    return fail(QStringLiteral("Can't delete ") + fi.fileName());
}

QString Fs::freeName(const QString &dir, const QString &name) const
{
    if (!QFileInfo::exists(join(dir, name))) return name;
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName().isEmpty() ? name : fi.completeBaseName();
    const QString ext = fi.completeBaseName().isEmpty() || fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    for (int i = 2; i < 1000; i++) {
        const QString n = QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(ext);
        if (!QFileInfo::exists(join(dir, n))) return n;
    }
    return name;
}

bool Fs::copyTree(const QString &from, const QString &to)
{
    const QFileInfo fi(from);
    errno = 0;
    if (fi.isDir()) {
        if (::mkdir(QFile::encodeName(to).constData(), 0755) != 0 && errno != EEXIST)
            return fail(QStringLiteral("Can't create ") + QFileInfo(to).fileName());
        QDir d(from);
        for (const QFileInfo &k : d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            if (!copyTree(k.filePath(), join(to, k.fileName()))) return false;
        return true;
    }
    // by hand: no copy_file_range/sendfile/FICLONE assumptions
    QFile in(from), out(to);
    if (!in.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Can't read ") + fi.fileName());
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail(QStringLiteral("Can't write ") + QFileInfo(to).fileName());
    QByteArray buf;
    while (!(buf = in.read(256 * 1024)).isEmpty())
        if (out.write(buf) != buf.size()) return fail(QStringLiteral("Can't write ") + QFileInfo(to).fileName());
    if (fi.permissions() & QFileDevice::ExeOwner)
        out.setPermissions(out.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);
    return true;
}

bool Fs::copy(const QString &from, const QString &toDir)
{
    if (QFileInfo(from).isDir() && (toDir + QLatin1Char('/')).startsWith(from + QLatin1Char('/'))) {
        errno = 0;
        return fail(QStringLiteral("Can't copy a folder into itself"));
    }
    const QString name = freeName(toDir, QFileInfo(from).fileName());
    return copyTree(from, join(toDir, name));
}

bool Fs::move(const QString &from, const QString &toDir)
{
    if (parentOf(from) == toDir) return true;
    const QString to = join(toDir, freeName(toDir, QFileInfo(from).fileName()));
    if (::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0) return true;
    // across what rename can't do: copy, then delete
    return copyTree(from, to) && remove(from);
}

QString Fs::readText(const QString &path, int maxBytes) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.read(maxBytes));
}

int Fs::countEntries(const QString &dir) const
{
    return int(QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size());
}

QString Fs::permString(int mode, bool dir) const
{
    QString s = dir ? QStringLiteral("d") : QStringLiteral("-");
    const char *rwx = "rwx";
    for (int i = 8; i >= 0; i--) s += (mode & (1 << i)) ? QLatin1Char(rwx[(8 - i) % 3]) : QLatin1Char('-');
    if (mode & 01000) s[9] = (mode & 1) ? QLatin1Char('t') : QLatin1Char('T');
    return s;
}

QString Fs::sizeString(double b) const
{
    if (b < 1024) return QStringLiteral("%1 B").arg(qint64(b));
    const char *u[] = { "KB", "MB", "GB", "TB" };
    int i = -1;
    do { b /= 1024; i++; } while (b >= 1024 && i < 3);
    return QStringLiteral("%1 %2").arg(b, 0, 'f', b < 10 ? 1 : 0).arg(QLatin1String(u[i]));
}

bool Fs::spawn(const QString &path)
{
    const int fd = zerp_qpa_c2s_fd();
    if (fd < 0) { errno = 0; return fail(QStringLiteral("Not running under Zerp")); }
    ZerpSpawnMsg m;
    memset(&m, 0, sizeof m);
    m.type = ZERP_MSG_SPAWN;
    const QByteArray p = QFile::encodeName(path);
    if (p.size() >= int(sizeof m.path)) { errno = ENAMETOOLONG; return fail(QStringLiteral("Can't start ") + path); }
    memcpy(m.path, p.constData(), size_t(p.size()));
    errno = 0;
    if (::write(fd, &m, sizeof m) != (ssize_t)sizeof m) return fail(QStringLiteral("Can't start ") + path);
    return true;
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
    zerp_qpa_capture_args(argc, argv);
    qputenv("QT_QPA_PLATFORM", "qwynlandfb");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    if (!qEnvironmentVariableIsSet("FONTCONFIG_FILE"))
        qputenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf");
    if (!qEnvironmentVariableIsSet("HOME"))
        qputenv("HOME", "/home/user");

    QGuiApplication app(argc, argv);
    Fs fs;
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("fs"), &fs);
    engine.rootContext()->setContextProperty(QStringLiteral("startDir"),
        argc > 4 ? QString::fromLocal8Bit(argv[4]) : fs.home());
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [](const QList<QQmlError> &ws) {
        for (const QQmlError &w : ws) fprintf(stderr, "[files] %s\n", qPrintable(w.toString()));
    });
    const QString qml = QStringLiteral("/usr/share/zerp/qml/files/Main.qml");
    engine.load(QUrl::fromLocalFile(qml));
    if (engine.rootObjects().isEmpty()) {
        fprintf(stderr, "[files] FAIL: could not load %s\n", qPrintable(qml));
        return 1;
    }
    fprintf(stderr, "[files] up\n");
    return app.exec();
}
