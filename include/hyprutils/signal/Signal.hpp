#ifndef HYPRUTILS_SIGNAL_SIGNAL_HPP
#define HYPRUTILS_SIGNAL_SIGNAL_HPP

#include <functional>
#include <vector>
#include <memory>
#include <any>

namespace Hyprutils {
namespace Signal {

class CSignalListener {
public:
    std::function<void(std::any)> m_fnCallback;
    CSignalListener() = default;
    CSignalListener(std::function<void(std::any)> cb) : m_fnCallback(cb) {}
};

using CHyprSignalListener = std::shared_ptr<CSignalListener>;
using CHyprSignalListenerRef = std::shared_ptr<CSignalListener>;

class CSignal {
public:
    std::vector<std::shared_ptr<CSignalListener>> m_vListeners;

    std::shared_ptr<CSignalListener> registerListener(std::function<void(std::any)> cb) {
        auto l = std::make_shared<CSignalListener>(cb);
        m_vListeners.push_back(l);
        return l;
    }

    template <typename Func>
    std::shared_ptr<CSignalListener> registerListener(Func&& cb) {
        auto l = std::make_shared<CSignalListener>([cb](std::any a) {});
        m_vListeners.push_back(l);
        return l;
    }

    template <typename Func>
    std::shared_ptr<CSignalListener> listenStatic(Func&& cb) {
        auto l = std::make_shared<CSignalListener>([cb](std::any a) {});
        m_vListeners.push_back(l);
        return l;
    }

    template <typename Func>
    std::shared_ptr<CSignalListener> listen(Func&& cb) {
        auto l = std::make_shared<CSignalListener>([cb](std::any a) {});
        m_vListeners.push_back(l);
        return l;
    }

    void emit(std::any data = {}) {
        for (auto& l : m_vListeners) {
            if (l && l->m_fnCallback) l->m_fnCallback(data);
        }
    }

    template <typename T>
    std::shared_ptr<CSignalListener> forward(T& other) {
        return listen([](std::any) {});
    }
};

template <typename... Args>
class CSignalT : public CSignal {
public:
    CSignalT() {}
    
    void emit(Args... args) {
        CSignal::emit({});
    }
};

}
}

using CSignal = Hyprutils::Signal::CSignal;
using CHyprSignalListener = Hyprutils::Signal::CHyprSignalListener;
using Hyprutils::Signal::CSignalListener;
using Hyprutils::Signal::CSignalT;

#endif /* HYPRUTILS_SIGNAL_SIGNAL_HPP */
