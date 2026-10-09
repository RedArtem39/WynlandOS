// WynlandOS - Files: what the QML side asks of the file system.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>

class Fs : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString home READ home CONSTANT)
    Q_PROPERTY(int uid READ uid NOTIFY uidChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    using QObject::QObject;

    QString home() const { return qEnvironmentVariableIsSet("HOME") ? qEnvironmentVariable("HOME") : QStringLiteral("/"); }
    int uid() const;
    QString lastError() const { return m_error; }

    // one entry per file: name, path, dir, size, mtime (ms, 0 unknown),
    // mode (permission bits), owner, kind (dir/text/image/exec/archive/
    // other); directories first, then by name
    Q_INVOKABLE QVariantList list(const QString &dir, bool hidden);
    Q_INVOKABLE QVariantMap info(const QString &path);
    Q_INVOKABLE bool exists(const QString &path) const;
    Q_INVOKABLE bool isDir(const QString &path) const;
    Q_INVOKABLE QString parentOf(const QString &path) const;
    Q_INVOKABLE QString join(const QString &dir, const QString &name) const;

    Q_INVOKABLE bool makeDir(const QString &path);
    Q_INVOKABLE bool makeFile(const QString &path);
    Q_INVOKABLE bool rename(const QString &from, const QString &to);
    Q_INVOKABLE bool remove(const QString &path);           // recursive
    Q_INVOKABLE bool copy(const QString &from, const QString &toDir);
    Q_INVOKABLE bool move(const QString &from, const QString &toDir);
    Q_INVOKABLE QString freeName(const QString &dir, const QString &name) const;

    Q_INVOKABLE QString readText(const QString &path, int maxBytes) const;
    Q_INVOKABLE int countEntries(const QString &dir) const;
    Q_INVOKABLE QString permString(int mode, bool dir) const;
    Q_INVOKABLE QString sizeString(double bytes) const;

    // a program as a new Zerp tile (ZERP_MSG_SPAWN)
    Q_INVOKABLE bool spawn(const QString &path);

signals:
    void uidChanged();
    void lastErrorChanged();

private:
    bool fail(const QString &what);
    bool copyTree(const QString &from, const QString &to);
    QString m_error;
};
