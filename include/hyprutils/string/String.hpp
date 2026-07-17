#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <charconv>
#include <sstream>
#include <cctype>

namespace Hyprutils::String {

inline std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
        s.erase(0, 1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

inline std::string trim(std::string_view s) {
    return trim(std::string(s));
}

inline void replaceInString(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = str.find(from);
    if (start_pos != std::string::npos)
        str.replace(start_pos, from.length(), to);
}

inline bool isNumber(const std::string& str, bool allowfloat = false) {
    if (str.empty()) return false;
    for (char c : str) {
        if (!std::isdigit(c) && c != '-' && (!allowfloat || c != '.')) return false;
    }
    return true;
}

inline bool isNumber2(const std::string& str, bool allowfloat = true) {
    return isNumber(str, allowfloat);
}

inline bool isNumber(std::string_view str, bool allowfloat = false) {
    return isNumber(std::string(str), allowfloat);
}

inline bool isNumber2(std::string_view str, bool allowfloat = true) {
    return isNumber2(std::string(str), allowfloat);
}

}

#include "Numeric.hpp"

namespace Hyprutils::String {

template <typename T>
inline CNumericResult<T> strToNumber(const std::string& str) {
    CNumericResult<T> res;
    if (str.empty()) {
        res.err = NUMERIC_PARSE_BAD;
        return res;
    }
    try {
        std::stringstream ss(str);
        T val;
        if (ss >> val) {
            res.val = val;
            res.err = NUMERIC_PARSE_OK;
            return res;
        }
    } catch (...) {}
    res.err = NUMERIC_PARSE_BAD;
    return res;
}

template <typename T>
inline CNumericResult<T> strToNumber(std::string_view str) {
    return strToNumber<T>(std::string(str));
}

}

using Hyprutils::String::trim;
using Hyprutils::String::isNumber;
using Hyprutils::String::isNumber2;
using Hyprutils::String::strToNumber;
using Hyprutils::String::replaceInString;
