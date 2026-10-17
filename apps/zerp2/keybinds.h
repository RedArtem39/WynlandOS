// WynlandOS - Zerp 2.0: key bindings in Hyprland's (classic) syntax.
//
//     $terminal = /usr/bin/term
//     bind  = SUPER, T, exec, $terminal
//     bindr = SUPER, SUPER_L, global, caelestia:launcher
//
// From ~/.config/zerp2/keybinds.conf, or /etc/zerp2/keybinds.conf without
// one (Caelestia's bindings, translated). Flags after "bind": r -- on
// release (a modifier's own release bind fires only if no other key came
// in between: Super alone opens the launcher, Super+T does not), n --
// the key still reaches the window; l and e are accepted. Keys are xkb
// names (T, Return, Page_Down, SUPER_L, F12, ...), any case.
//
// Zerp gets PC scancodes (set 1): a key here is code + 128 when it came
// after an E0 prefix. Feed every scancode to key(); it says whether the
// window must not see it.
#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>
#include <functional>

class KeyBinds
{
public:
    enum Mod { Shift = 1, Ctrl = 2, Alt = 4, Super = 8 };
    struct Bind {
        int mods = 0;
        int key = 0;              // scancode (+128 for E0)
        bool release = false, pass = false;
        QString dispatcher, args;
        int line = 0;
    };
    // run a binding's action; pressed false: its key went up (globals)
    using Run = std::function<void(const Bind &, bool pressed)>;

    void load();                  // the user's file, else the system's
    void setRun(Run r) { m_run = std::move(r); }
    // one key event (raw scancode, E0 already folded in); true: swallow it
    bool key(int kid, bool release);
    int count() const { return m_binds.size(); }
    // the modifiers held now (the window's own state is Zerp's business)
    int mods() const { return m_mods; }

private:
    bool parseFile(const QString &path);
    static int keyFromName(const QString &name);
    static int modsFromString(const QString &s);
    static int modOfKey(int kid);

    QVector<Bind> m_binds;
    Run m_run;
    int m_mods = 0;
    bool m_alone[256] = {};       // a modifier pressed with nothing after it (yet)
    int m_active[256];            // a press binding's index while its key is down, -1
    bool m_swallow[256] = {};
public:
    KeyBinds() { for (int &a : m_active) a = -1; }
};
