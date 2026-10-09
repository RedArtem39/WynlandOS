// WynlandOS - Zerp 2.0's login screen (greeter.h).
#include "greeter.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QVariantMap>

#include <fcntl.h>
#include <unistd.h>

Greeter::Greeter(int reqFd, int repFd, bool firstBoot, QObject *parent)
    : QObject(parent), m_req(reqFd), m_rep(repFd), m_first(firstBoot)
{
    // the accounts that can log in: /etc/passwd, uid 1000 and up
    QFile f(QStringLiteral("/etc/passwd"));
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : f.readAll().split('\n')) {
            const QList<QByteArray> p = line.split(':');
            if (p.size() < 7) continue;
            const uint uid = p[2].toUInt();
            if (uid < 1000 || uid >= 60000) continue;
            QVariantMap u;
            u[QStringLiteral("name")] = QString::fromUtf8(p[0]);
            const QString full = QString::fromUtf8(p[4]).split(',').first().trimmed();
            u[QStringLiteral("fullName")] = full.isEmpty() ? QString::fromUtf8(p[0]) : full;
            m_users.append(u);
        }
    }
    fcntl(m_rep, F_SETFL, O_NONBLOCK);
    m_notifier = new QSocketNotifier(m_rep, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Greeter::readReply);
}

void Greeter::send(const QByteArray &line)
{
    QByteArray all = line + '\n';
    const char *p = all.constData();
    qsizetype left = all.size();
    while (left > 0) {
        ssize_t w = ::write(m_req, p, size_t(left));
        if (w <= 0) break;
        p += w;
        left -= w;
    }
    all.fill('\0');
    m_busy = true;
    emit busyChanged();
}

void Greeter::login(const QString &user, const QString &password)
{
    if (m_busy) return;
    send("LOGIN " + user.toUtf8().toHex() + ' ' + password.toUtf8().toHex());
}

void Greeter::createAccount(const QString &user, const QString &fullName, const QString &password)
{
    if (m_busy) return;
    QByteArray full = fullName.toUtf8();
    if (full.isEmpty()) full = user.toUtf8();
    send("CREATE " + user.toUtf8().toHex() + ' ' + full.toHex() + ' ' + password.toUtf8().toHex());
}

void Greeter::readReply()
{
    char buf[256];
    ssize_t n;
    while ((n = ::read(m_rep, buf, sizeof buf)) > 0) m_in.append(buf, n);
    if (n == 0) m_notifier->setEnabled(false);       // wynlogin is gone
    const qsizetype nl = m_in.indexOf('\n');
    if (nl < 0) return;
    const QByteArray line = m_in.left(nl);
    m_in.remove(0, nl + 1);
    m_busy = false;
    emit busyChanged();
    if (line == "OK") emit accepted();
    else emit refused(QString::fromUtf8(line.startsWith("FAIL ") ? line.mid(5) : line));
}

void Greeter::quit()
{
    QCoreApplication::exit(0);
}
