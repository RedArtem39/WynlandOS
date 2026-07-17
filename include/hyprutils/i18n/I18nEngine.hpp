#pragma once
#include <string>
#include <unordered_map>
#include <functional>

namespace Hyprutils::I18n { 
    inline std::string _(const std::string& s) { return s; } 

    using translationVarMap = std::unordered_map<std::string, std::string>;

    struct LocaleInfo {
        std::string locale() const { return "en_US"; }
    };

    class CI18nEngine {
    public:
        void registerEntry(const char* locale, int key, const char* translation) {}
        void registerEntry(const char* locale, int key, std::function<std::string(const translationVarMap&)> func) {}
        std::string localize(int key, const translationVarMap& vars = {}) { return ""; }
        std::string localizeEntry(const std::string& locale, int key, const translationVarMap& vars = {}) { return ""; }
        void setLocale(const std::string& locale) {}
        void setFallbackLocale(const std::string& locale) {}
        LocaleInfo getSystemLocale() { return {}; }
    };
}
