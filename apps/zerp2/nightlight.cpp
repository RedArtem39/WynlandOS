// Zerp 2.0 - night light: the screen warmer after sunset, by itself.
//
// After each frame one full-screen quad multiplies what was drawn by a
// warm colour (glBlendFunc(GL_ZERO, GL_SRC_COLOR): red kept, green and
// blue lowered), the colour of a light of the temperature the time of
// day asks for: 6500 K (nothing) by day, 5000 K once the sun is down,
// deepening to 4000 K (GNOME's) by 23:00, back to the day's as the sun
// comes up -- by degrees, never at once (autoKelvin()). What reaches the
// eyes is what a gamma table in the display would give; the pointer (the
// GPU's cursor plane) is not in the frame.
//
// ~/.config/wynland/nightlight.conf "mode = auto | on | off" (off when
// there is none): the shell's tile writes it, a watch on it (inotify)
// takes it at once, and a change fades in over a second.
//
// Where the sun is: ~/.cache/wynland/location ("lat lon"), asked of
// ip-api.com once (the same service the kernel asks the time zone of);
// without it, the time zone's longitude and 50 deg north.
#include "nightlight.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLFunctions>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace {

// GNOME's night light is 4000 K; f.lux's 3400 K was too much
constexpr double kNightK = 4000.0;   // the night, from 23:00
constexpr double kDuskK = 5000.0;    // just after sunset

// the colour of a light at k kelvin, as a multiplier with 6500 K = white
// (Tanner Helland's fit of the black-body colours)
void kelvinRgb(double k, float out[3])
{
    auto fit = [](double t, float c[3]) {
        t /= 100.0;
        double r = 255, g, b;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
        b = t <= 19 ? 0 : 138.5177312231 * std::log(t - 10) - 305.0447927307;
        c[0] = float(std::clamp(r, 0.0, 255.0) / 255.0);
        c[1] = float(std::clamp(g, 0.0, 255.0) / 255.0);
        c[2] = float(std::clamp(b, 0.0, 255.0) / 255.0);
    };
    float w[3], c[3];
    fit(6500, w);
    fit(k, c);
    for (int i = 0; i < 3; i++) out[i] = std::min(1.0f, c[i] / w[i]);
}

// the sun's height (degrees) at `utc` seen from lat/lon (NOAA's short form)
double sunElevation(const QDateTime &utc, double lat, double lon)
{
    const double rad = M_PI / 180.0;
    const int n = utc.date().dayOfYear();
    const double hours = utc.time().hour() + utc.time().minute() / 60.0 + utc.time().second() / 3600.0;
    const double decl = 23.44 * std::sin(2 * M_PI * (284 + n) / 365.0);
    const double B = 2 * M_PI * (n - 81) / 364.0;
    const double eot = 9.87 * std::sin(2 * B) - 7.53 * std::cos(B) - 1.5 * std::sin(B);   // minutes
    const double solarMin = hours * 60 + lon * 4 + eot;
    const double ha = solarMin / 4 - 180;
    const double s = std::sin(lat * rad) * std::sin(decl * rad) +
                     std::cos(lat * rad) * std::cos(decl * rad) * std::cos(ha * rad);
    return std::asin(std::clamp(s, -1.0, 1.0)) / rad;
}

class NightLight : public QObject
{
public:
    explicit NightLight(QQuickWindow *win) : QObject(win), m_win(win)
    {
        const QString home = QString::fromLocal8Bit(qgetenv("HOME"));
        m_conf = home + QStringLiteral("/.config/wynland/nightlight.conf");
        m_loc = home + QStringLiteral("/.cache/wynland/location");
        QDir().mkpath(home + QStringLiteral("/.config/wynland"));
        QDir().mkpath(home + QStringLiteral("/.cache/wynland"));

        // the file is often replaced (written to a new one, renamed):
        // the directory is watched, and the file whenever it is there
        m_watch.addPath(home + QStringLiteral("/.config/wynland"));
        if (QFile::exists(m_conf)) m_watch.addPath(m_conf);
        connect(&m_watch, &QFileSystemWatcher::directoryChanged, this, [this] { rewatch(); reload(); });
        connect(&m_watch, &QFileSystemWatcher::fileChanged, this, [this] { rewatch(); reload(); });

        m_tick.setInterval(60 * 1000);                 // the sun moves slowly
        connect(&m_tick, &QTimer::timeout, this, [this] { reload(); });   // (reload: retarget, and the file again should a watch be missed)
        m_tick.start();
        m_fade.setInterval(16);
        connect(&m_fade, &QTimer::timeout, this, [this] { step(); });

        connect(win, &QQuickWindow::afterRenderPassRecording, this, [this] { draw(); }, Qt::DirectConnection);
        connect(win, &QQuickWindow::sceneGraphInvalidated, this, [this] { m_prog = 0; }, Qt::DirectConnection);

        loadLocation();
        reload();
        m_cur = m_target;                              // at start: as it should be, no fade
        publish();
    }

private:
    void rewatch()
    {
        if (QFile::exists(m_conf) && !m_watch.files().contains(m_conf)) m_watch.addPath(m_conf);
    }

    void reload()
    {
        QString mode = QStringLiteral("off");
        QFile f(m_conf);
        if (f.open(QIODevice::ReadOnly)) {
            for (const QByteArray &raw : f.readAll().split('\n')) {
                const QByteArray line = raw.trimmed();
                if (!line.startsWith("mode")) continue;
                const int eq = line.indexOf('=');
                if (eq > 0) mode = QString::fromLatin1(line.mid(eq + 1).trimmed()).toLower();
            }
        }
        if (mode != m_mode) fprintf(stderr, "[zerp2] night light: %s\n", qPrintable(mode));
        m_mode = mode;
        retarget();
    }

    void loadLocation()
    {
        QFile f(m_loc);
        if (f.open(QIODevice::ReadOnly)) {
            const QList<QByteArray> v = f.readAll().simplified().split(' ');
            bool a = false, b = false;
            if (v.size() == 2) { m_lat = v[0].toDouble(&a); m_lon = v[1].toDouble(&b); }
            if (a && b) { m_haveLoc = true; return; }
        }
        // the time zone's own longitude meanwhile; the real place asked for once
        m_lon = QDateTime::currentDateTime().offsetFromUtc() / 3600.0 * 15.0;
        m_lat = 50.0;
        auto *p = new QProcess(this);
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
            const QList<QByteArray> v = p->readAllStandardOutput().split('\n');
            p->deleteLater();
            bool a = false, b = false;
            double lat = 0, lon = 0;
            if (code == 0 && v.size() >= 2) { lat = v[0].trimmed().toDouble(&a); lon = v[1].trimmed().toDouble(&b); }
            if (!a || !b) { QTimer::singleShot(10 * 60 * 1000, this, [this] { loadLocation(); }); return; }
            m_lat = lat; m_lon = lon; m_haveLoc = true;
            QFile o(m_loc);
            if (o.open(QIODevice::WriteOnly | QIODevice::Truncate))
                o.write(QByteArray::number(lat, 'f', 2) + ' ' + QByteArray::number(lon, 'f', 2) + '\n');
            retarget();
        });
        p->start(QStringLiteral("/usr/bin/curl"),
                 {QStringLiteral("-s"), QStringLiteral("-m"), QStringLiteral("10"),
                  QStringLiteral("http://ip-api.com/line/?fields=lat,lon")});
    }

    // the temperature wanted now (auto): the day's light until the sun
    // goes down; a little warmer through the dusk (5000 K); the evening
    // deepening to the night's 4000 K between 21:00 and 23:00 (the clock's
    // hours: the night is the person's, not the sun's); the dawn bringing
    // the day back as the sun comes up
    double autoKelvin() const
    {
        auto ease = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); };
        const QDateTime utc = QDateTime::currentDateTimeUtc();
        const double e = sunElevation(utc, m_lat, m_lon);
        const double dark = ease((3.0 - e) / 9.0);     // +3 deg: day .. -6 deg: night
        if (dark <= 0) return 6500;
        const bool setting = sunElevation(utc.addSecs(600), m_lat, m_lon) < e;
        if (!setting) return 6500 + (kNightK - 6500) * dark;     // after midnight, the dawn
        const QTime t = QDateTime::currentDateTime().time();
        double h = t.hour() + t.minute() / 60.0;
        if (h < 12) h += 24;                               // past midnight: still the night
        const double late = ease((h - 21.0) / 2.0);
        const double dusk = 6500 + (kDuskK - 6500) * dark;
        return dusk + (kNightK - kDuskK) * late * dark;
    }

    // the strength wanted now: 0 none .. 1 the night's colour
    void retarget()
    {
        double t = 0;
        if (m_mode == QLatin1String("on")) t = 1;
        else if (m_mode == QLatin1String("auto")) t = (6500 - autoKelvin()) / (6500 - kNightK);
        m_target = std::clamp(t, 0.0, 1.0);
        if (std::fabs(m_target - m_cur) > 1e-3) { if (!m_fade.isActive()) m_fade.start(); }
        else { m_cur = m_target; publish(); }
    }

    void step()
    {
        const double d = m_target - m_cur;
        if (std::fabs(d) <= 1.0 / 60) { m_cur = m_target; m_fade.stop(); }
        else m_cur += d > 0 ? 1.0 / 60 : -1.0 / 60;    // a second from none to all
        publish();
    }

    void publish()
    {
        float c[3] = {1, 1, 1};
        if (m_cur > 0.001) kelvinRgb(6500.0 + (kNightK - 6500.0) * m_cur, c);
        uint32_t packed = 0;
        if (c[1] < 0.999f || c[2] < 0.999f)
            packed = 1u << 31 | uint32_t(c[0] * 1023) << 20 | uint32_t(c[1] * 1023) << 10 | uint32_t(c[2] * 1023);
        if (m_color.exchange(packed) != packed) m_win->update();
    }

    // render thread: the quad over the frame just drawn
    void draw()
    {
        const uint32_t packed = m_color.load();
        if (!packed) return;
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        if (!ctx) return;
        m_win->beginExternalCommands();
        QOpenGLExtraFunctions *gl = ctx->extraFunctions();
        if (!m_prog) build(gl);
        if (m_prog) {
            const float r = ((packed >> 20) & 1023) / 1023.0f, g = ((packed >> 10) & 1023) / 1023.0f,
                        b = (packed & 1023) / 1023.0f;
            static const float tri[] = {-1, -1, 3, -1, -1, 3};   // one triangle covers the screen
            gl->glDisable(GL_DEPTH_TEST);
            gl->glDisable(GL_STENCIL_TEST);
            gl->glDisable(GL_SCISSOR_TEST);
            gl->glDisable(GL_CULL_FACE);
            gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            gl->glEnable(GL_BLEND);
            gl->glBlendEquation(GL_FUNC_ADD);
            gl->glBlendFunc(GL_ZERO, GL_SRC_COLOR);   // what is there, times the colour
            const QSize px = m_win->size() * m_win->effectiveDevicePixelRatio();
            gl->glViewport(0, 0, px.width(), px.height());
            if (ctx->format().majorVersion() >= 3) gl->glBindVertexArray(0);
            gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
            gl->glUseProgram(m_prog);
            gl->glUniform3f(m_uColor, r, g, b);
            gl->glEnableVertexAttribArray(0);
            gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
            gl->glDrawArrays(GL_TRIANGLES, 0, 3);
            gl->glDisableVertexAttribArray(0);
            gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        m_win->endExternalCommands();
    }

    void build(QOpenGLExtraFunctions *gl)
    {
        static const char *vs = "attribute vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
        static const char *fs = "precision mediump float;\nuniform vec3 c;\nvoid main() { gl_FragColor = vec4(c, 1.0); }\n";
        auto sh = [gl](GLenum type, const char *src) {
            GLuint s = gl->glCreateShader(type);
            gl->glShaderSource(s, 1, &src, nullptr);
            gl->glCompileShader(s);
            GLint ok = 0;
            gl->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) { fprintf(stderr, "[zerp2] night light: a shader did not compile\n"); gl->glDeleteShader(s); return 0u; }
            return s;
        };
        GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
        if (!v || !f) return;
        GLuint p = gl->glCreateProgram();
        gl->glAttachShader(p, v);
        gl->glAttachShader(p, f);
        gl->glBindAttribLocation(p, 0, "p");
        gl->glLinkProgram(p);
        gl->glDeleteShader(v);
        gl->glDeleteShader(f);
        GLint ok = 0;
        gl->glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { fprintf(stderr, "[zerp2] night light: the program did not link\n"); gl->glDeleteProgram(p); return; }
        m_prog = p;
        m_uColor = gl->glGetUniformLocation(p, "c");
    }

    QQuickWindow *m_win;
    QString m_conf, m_loc, m_mode = QStringLiteral("off");
    QFileSystemWatcher m_watch;
    QTimer m_tick, m_fade;
    double m_lat = 50, m_lon = 0, m_cur = 0, m_target = 0;
    bool m_haveLoc = false;
    std::atomic<uint32_t> m_color{0};      // 0: nothing to draw; else 1 | r10 g10 b10
    GLuint m_prog = 0;                     // (render thread)
    GLint m_uColor = -1;
};

} // namespace

void nightlightAttach(QQuickWindow *win)
{
    new NightLight(win);
}
