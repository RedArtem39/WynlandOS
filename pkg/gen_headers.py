import os

headers = [
    ("os/FileDescriptor.hpp", "#pragma once\nnamespace Hyprutils::OS { class CFileDescriptor { public: int get() const { return -1; } bool isValid() const { return false; } }; }\nusing CFileDescriptor = Hyprutils::OS::CFileDescriptor;\n"),
    ("os/Process.hpp", "#pragma once\nnamespace Hyprutils::OS { class CProcess { public: CProcess(const char*, const char*) {} bool runSync() { return true; } }; }\nusing CProcess = Hyprutils::OS::CProcess;\n"),
    ("path/Path.hpp", "#pragma once\n#include <string>\nnamespace Hyprutils::Path { inline std::string findConfig(const std::string& name) { return \"/etc/hypr/hyprland.conf\"; } }\n"),
    ("i18n/I18nEngine.hpp", "#pragma once\n#include <string>\nnamespace Hyprutils::I18n { inline std::string _(const std::string& s) { return s; } }\n"),
    ("string/String.hpp", "#pragma once\n#include <string>\n#include <vector>\nnamespace Hyprutils::String { inline std::string trim(std::string s) { return s; } }\n"),
    ("string/VarList.hpp", "#pragma once\n#include \"VarList2.hpp\"\n"),
    ("string/ConstVarList.hpp", "#pragma once\n#include \"VarList2.hpp\"\nusing CConstVarList = CVarList;\n"),
    ("string/Numeric.hpp", "#pragma once\n#include <string>\nnamespace Hyprutils::String { inline bool isNumber(const std::string& s) { return true; } }\n")
]

for h, content in headers:
    p = os.path.join("include/hyprutils", h)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as f:
        f.write(content)
print("Generated hyprutils headers successfully.")
