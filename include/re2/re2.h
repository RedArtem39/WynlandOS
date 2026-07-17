#pragma once
#include <string>
#include <string_view>
#include <regex>
#include <memory>

namespace re2 {
class RE2 {
public:
    std::string pattern;
    std::regex rx;

    RE2(const std::string& pat) : pattern(pat) {
        try { rx = std::regex(pattern); } catch (...) {}
    }
    RE2(std::string_view pat) : pattern(pat) {
        try { rx = std::regex(pattern); } catch (...) {}
    }
    RE2(const char* pat) : pattern(pat ? pat : "") {
        try { rx = std::regex(pattern); } catch (...) {}
    }

    static bool FullMatch(std::string_view text, const RE2& re) {
        try {
            return std::regex_match(text.begin(), text.end(), re.rx);
        } catch (...) { return false; }
    }
    static bool FullMatch(const std::string& text, const RE2& re) {
        try {
            return std::regex_match(text, re.rx);
        } catch (...) { return false; }
    }
    static bool FullMatch(const std::string& text, const std::string& pattern) {
        try {
            return std::regex_match(text, std::regex(pattern));
        } catch (...) { return false; }
    }
    static bool FullMatch(std::string_view text, const std::string& pattern) {
        try {
            return std::regex_match(text.begin(), text.end(), std::regex(pattern));
        } catch (...) { return false; }
    }
};
}
using RE2 = re2::RE2;
