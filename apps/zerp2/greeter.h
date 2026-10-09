// WynlandOS - Zerp 2.0's login screen: the QML side's link to wynlogin.
//
// zerp2 --greeter REQ_FD REP_FD [--first] is started by wynlogin (as the
// unprivileged "greeter" account) instead of the desktop. The screen asks;
// wynlogin checks the password and starts the session once we exit. See
// apps/accounts/wynlogin.c for the protocol.
#pragma once

#include <QtCore/QObject>
#include <QtCore/QSocketNotifier>
#include <QtCore/QString>
#include <QtCore/QVariantList>

class Greeter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList users READ users CONSTANT)
    Q_PROPERTY(bool firstBoot READ firstBoot CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
public:
    Greeter(int reqFd, int repFd, bool firstBoot, QObject *parent = nullptr);

    QVariantList users() const { return m_users; }
    bool firstBoot() const { return m_first; }
    bool busy() const { return m_busy; }

    Q_INVOKABLE void login(const QString &user, const QString &password);
    Q_INVOKABLE void createAccount(const QString &user, const QString &fullName, const QString &password);
    Q_INVOKABLE void quit();          // after the fade-out: wynlogin takes over

signals:
    void busyChanged();
    void accepted();                  // fade out, then quit()
    void refused(const QString &reason);

private:
    void send(const QByteArray &line);
    void readReply();

    int m_req, m_rep;
    bool m_first, m_busy = false;
    QVariantList m_users;
    QByteArray m_in;
    QSocketNotifier *m_notifier;
};
