// WynlandOS - Zerp 2.0's key bindings (see keybinds.h)
#include "keybinds.h"
#include "../../zerp_layouts.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QStringList>

#include <cstdio>

// xkb key names -> PC scancode set 1 (+128: after E0)
int KeyBinds::keyFromName(const QString &raw)
{
    static QHash<QString, int> t;
    if (t.isEmpty()) {
        const char *row1 = "qwertyuiop";      // 0x10..0x19
        const char *row2 = "asdfghjkl";       // 0x1E..0x26
        const char *row3 = "zxcvbnm";         // 0x2C..0x32
        for (int i = 0; row1[i]; i++) t.insert(QString(QChar(row1[i])), 0x10 + i);
        for (int i = 0; row2[i]; i++) t.insert(QString(QChar(row2[i])), 0x1E + i);
        for (int i = 0; row3[i]; i++) t.insert(QString(QChar(row3[i])), 0x2C + i);
        for (int i = 1; i <= 9; i++) t.insert(QString::number(i), 0x01 + i);
        t.insert(QStringLiteral("0"), 0x0B);
        const struct { const char *n; int k; } names[] = {
            { "escape", 0x01 }, { "minus", 0x0C }, { "equal", 0x0D }, { "backspace", 0x0E },
            { "tab", 0x0F }, { "bracketleft", 0x1A }, { "bracketright", 0x1B }, { "return", 0x1C },
            { "enter", 0x1C }, { "control_l", 0x1D }, { "semicolon", 0x27 }, { "apostrophe", 0x28 },
            { "grave", 0x29 }, { "shift_l", 0x2A }, { "backslash", 0x2B }, { "comma", 0x33 },
            { "period", 0x34 }, { "slash", 0x35 }, { "shift_r", 0x36 }, { "alt_l", 0x38 },
            { "space", 0x39 }, { "caps_lock", 0x3A }, { "f11", 0x57 }, { "f12", 0x58 },
            { "control_r", 128 + 0x1D }, { "alt_r", 128 + 0x38 }, { "print", 128 + 0x37 },
            { "home", 128 + 0x47 }, { "up", 128 + 0x48 }, { "page_up", 128 + 0x49 }, { "prior", 128 + 0x49 },
            { "left", 128 + 0x4B }, { "right", 128 + 0x4D }, { "end", 128 + 0x4F }, { "down", 128 + 0x50 },
            { "page_down", 128 + 0x51 }, { "next", 128 + 0x51 }, { "insert", 128 + 0x52 },
            { "delete", 128 + 0x53 }, { "super_l", 128 + 0x5B }, { "super_r", 128 + 0x5C },
            { "menu", 128 + 0x5D },
        };
        for (const auto &n : names) t.insert(QString::fromLatin1(n.n), n.k);
        for (int i = 1; i <= 10; i++) t.insert(QStringLiteral("f%1").arg(i), 0x3A + i);
    }
    return t.value(raw.trimmed().toLower(), 0);
}

int KeyBinds::modsFromString(const QString &s)
{
    int m = 0;
    QString w = s.toUpper();
    for (QChar sep : { QChar('_'), QChar('+'), QChar('|') }) w.replace(sep, QChar(' '));
    for (const QString &p : w.split(QChar(' '), Qt::SkipEmptyParts)) {
        if (p == "SUPER" || p == "WIN" || p == "LOGO" || p == "MOD4" || p == "META") m |= Super;
        else if (p == "SHIFT") m |= Shift;
        else if (p == "CTRL" || p == "CONTROL") m |= Ctrl;
        else if (p == "ALT" || p == "MOD1") m |= Alt;
    }
    return m;
}

// the modifier a key is, 0 for none
int KeyBinds::modOfKey(int kid)
{
    switch (kid) {
    case 0x2A: case 0x36: return Shift;
    case 0x1D: case 128 + 0x1D: return Ctrl;
    case 0x38: case 128 + 0x38: return Alt;
    case 128 + 0x5B: case 128 + 0x5C: return Super;
    }
    return 0;
}

bool KeyBinds::parseFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QHash<QString, QString> vars;
    int n = 0;
    while (!f.atEnd()) {
        n++;
        QString line = QString::fromUtf8(f.readLine());
        const int hash = line.indexOf(QChar('#'));
        if (hash >= 0) line.truncate(hash);
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const int eq = line.indexOf(QChar('='));
        if (eq < 0) continue;
        const QString lhs = line.left(eq).trimmed();
        QString rhs = line.mid(eq + 1).trimmed();
        // $variables, longest names first (no prefix eats a longer one)
        if (lhs.startsWith(QChar('$'))) { vars.insert(lhs, rhs); continue; }
        QStringList names = vars.keys();
        std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
        for (const QString &v : names) rhs.replace(v, vars.value(v));
        // the keyboard layouts (also as Hyprland's input:kb_layout)
        QString opt = lhs;
        if (opt.startsWith(QStringLiteral("input:"))) opt = opt.mid(6);
        if (opt == QStringLiteral("kb_layout")) {
            QVector<int> ls;
            for (const QString &name : rhs.split(QChar(','), Qt::SkipEmptyParts)) {
                int found = -1;
                for (int i = 0; i < ZERP_LAYOUT_COUNT; i++)
                    if (name.trimmed() == QLatin1String(ZERP_LAYOUT_XKB[i])) found = i;
                if (found < 0) fprintf(stderr, "[zerp2] %s:%d: no layout \"%s\" (there are us, ru)\n", qPrintable(path), n, qPrintable(name.trimmed()));
                else if (!ls.contains(found)) ls << found;
            }
            if (!ls.isEmpty()) m_layouts = ls;
            continue;
        }
        if (opt == QStringLiteral("kb_options")) {
            if (rhs.contains(QStringLiteral("grp:win_space_toggle"))) m_toggle = SuperSpace;
            else if (rhs.contains(QStringLiteral("grp:alt_shift_toggle"))) m_toggle = AltShift;
            continue;
        }
        if (!lhs.startsWith(QStringLiteral("bind"))) continue;
        const QString flags = lhs.mid(4);
        const QStringList parts = rhs.split(QChar(','));
        if (parts.size() < 3) {
            fprintf(stderr, "[zerp2] %s:%d: a binding needs MODS, KEY, DISPATCHER\n", qPrintable(path), n);
            continue;
        }
        Bind b;
        b.mods = modsFromString(parts[0]);
        b.key = keyFromName(parts[1]);
        b.dispatcher = parts[2].trimmed().toLower();
        b.args = parts.mid(3).join(QChar(',')).trimmed();
        b.release = flags.contains(QChar('r'));
        b.pass = flags.contains(QChar('n'));
        b.line = n;
        if (!b.key) {
            fprintf(stderr, "[zerp2] %s:%d: no such key \"%s\"\n", qPrintable(path), n, qPrintable(parts[1].trimmed()));
            continue;
        }
        m_binds.append(b);
    }
    fprintf(stderr, "[zerp2] %d key bindings from %s\n", int(m_binds.size()), qPrintable(path));
    return true;
}

void KeyBinds::load()
{
    m_binds.clear();
    m_layouts = { 0, 1 };
    m_toggle = AltShift;
    const QString home = qEnvironmentVariable("HOME");
    if (!home.isEmpty() && parseFile(home + QStringLiteral("/.config/zerp2/keybinds.conf"))) return;
    parseFile(QStringLiteral("/etc/zerp2/keybinds.conf"));
}

bool KeyBinds::key(int kid, bool release)
{
    if (kid <= 0 || kid >= 256) return false;
    const int own = modOfKey(kid);
    if (!release) {
        // anything after a modifier spoils its own release binding
        for (bool &a : m_alone) a = false;
        if (own) {
            m_mods |= own;
            m_alone[kid] = true;
        }
        for (int i = 0; i < m_binds.size(); i++) {
            const Bind &b = m_binds[i];
            if (b.release || b.key != kid) continue;
            // a modifier's own binding: the other modifiers held, plus itself
            if ((b.mods | own) != m_mods) continue;
            m_active[kid] = i;
            if (m_run) m_run(b, true);
            m_swallow[kid] = !b.pass;
            return !b.pass;
        }
        return false;
    }

    bool swallow = m_swallow[kid];
    m_swallow[kid] = false;
    if (m_active[kid] >= 0) {
        if (m_run) m_run(m_binds[m_active[kid]], false);   // the key went up
        m_active[kid] = -1;
    }
    // release bindings: with the modifiers held when it went up (a
    // modifier's own included); a modifier's only when it was alone
    const int held = m_mods;
    const bool alone = m_alone[kid];
    m_alone[kid] = false;
    if (own) m_mods &= ~own;
    for (const Bind &b : m_binds) {
        if (!b.release || b.key != kid) continue;
        if ((b.mods | own) != held) continue;
        if (own && !alone) continue;
        if (m_run) { m_run(b, true); m_run(b, false); }
        return swallow || !b.pass;
    }
    return swallow;
}
