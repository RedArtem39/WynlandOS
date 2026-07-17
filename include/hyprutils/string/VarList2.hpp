#ifndef HYPRUTILS_STRING_VAR_LIST2_HPP
#define HYPRUTILS_STRING_VAR_LIST2_HPP

#include <string>
#include <vector>
#include <sstream>
#include <string_view>

namespace Hyprutils {
namespace String {

class CVarList {
public:
    std::vector<std::string> m_vArgs;

    CVarList() = default;
    CVarList(const std::string_view& in, const size_t max = 0, const char delim = ',', const bool removeEmpty = false, const bool skipSpaces = false) {
        std::string s(in);
        std::stringstream ss(s);
        std::string item;
        size_t count = 0;
        while (std::getline(ss, item, delim)) {
            if (removeEmpty && item.empty()) continue;
            if (max > 0 && count + 1 == max) {
                // last item gets the rest of the string
                std::string rest;
                std::getline(ss, rest, '\0');
                if (!rest.empty()) item += delim + rest;
                m_vArgs.push_back(item);
                break;
            }
            m_vArgs.push_back(item);
            count++;
        }
    }

    size_t size() const { return m_vArgs.size(); }
    std::string operator[](const size_t idx) const { return idx < m_vArgs.size() ? m_vArgs[idx] : ""; }
    auto begin() const { return m_vArgs.begin(); }
    auto end() const { return m_vArgs.end(); }

    bool contains(const std::string_view& str) const {
        for (const auto& s : m_vArgs) {
            if (s == str) return true;
        }
        return false;
    }

    std::string join(const std::string& delim = " ", size_t start = 0, size_t end = 0) const {
        std::string res;
        size_t actual_end = (end == 0 || end >= m_vArgs.size()) ? m_vArgs.size() : end + 1;
        for (size_t i = start; i < actual_end; ++i) {
            res += m_vArgs[i];
            if (i + 1 < actual_end) res += delim;
        }
        return res;
    }
    
    template<typename Callable>
    void map(Callable&& func) {
        for(auto& s : m_vArgs) {
            func(s);
        }
    }
    
    void append(const std::string& item) {
        m_vArgs.push_back(item);
    }
};

using CVarList2 = CVarList;

}
}

using CVarList = Hyprutils::String::CVarList;
using CVarList2 = Hyprutils::String::CVarList2;

#endif /* HYPRUTILS_STRING_VAR_LIST2_HPP */
