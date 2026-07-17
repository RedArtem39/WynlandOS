#pragma once
#include <string>
#include <vector>

namespace Hyprutils::OS {
class CProcess {
public:
    CProcess(const std::string& bin, const std::vector<std::string>& args = {}) {}
    CProcess(const char* bin, const char* arg) {}
    bool runSync() { return true; }
    bool runAsync() { return true; }
    void setStdoutFD(int fd) {}
    void addEnv(const std::string& name, const std::string& value) {}
    pid_t pid() const { return 0; }
    std::string stdOut() const { return ""; }
};
}
using CProcess = Hyprutils::OS::CProcess;
