#pragma once
#include <string>
#include <utility>
#include <optional>

namespace Hyprutils::Path {
    inline std::pair<std::optional<std::string>, std::optional<std::string>> findConfig(const std::string& name, const std::string& ext = "") {
        std::string filename = name + (ext.empty() ? ".conf" : ("." + ext));
        return {std::optional<std::string>("/etc/hypr/" + filename), std::nullopt};
    }
    inline std::string fullConfigPath(const std::string& base, const std::string& name, const std::string& ext = "") {
        return base + "/" + name + (ext.empty() ? ".conf" : ("." + ext));
    }
}
