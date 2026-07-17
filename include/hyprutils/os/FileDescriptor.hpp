#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <utility>

namespace Hyprutils::OS {
class CFileDescriptor {
public:
    CFileDescriptor() : fd(-1) {}
    CFileDescriptor(int fd_) : fd(fd_) {}
    ~CFileDescriptor() { reset(); }

    CFileDescriptor(const CFileDescriptor&) = delete;
    CFileDescriptor& operator=(const CFileDescriptor&) = delete;

    CFileDescriptor(CFileDescriptor&& o) noexcept : fd(o.fd) { o.fd = -1; }
    CFileDescriptor& operator=(CFileDescriptor&& o) noexcept {
        if (this != &o) {
            reset();
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }

    CFileDescriptor& operator=(int fd_) {
        reset();
        fd = fd_;
        return *this;
    }

    int get() const { return fd; }
    bool isValid() const { return fd >= 0; }
    bool isClosed() const { return fd < 0; }

    int getFlags() const {
        if (fd < 0) return 0;
        return fcntl(fd, F_GETFD);
    }
    int setFlags(int flags) {
        if (fd < 0) return -1;
        return fcntl(fd, F_SETFD, flags);
    }

    int take() {
        int ret = fd;
        fd = -1;
        return ret;
    }

    void reset(int newFd = -1) {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = newFd;
    }

    bool operator==(int o) const { return fd == o; }
    bool operator!=(int o) const { return fd != o; }
    explicit operator bool() const { return isValid(); }
    CFileDescriptor duplicate() const { return CFileDescriptor(-1); }

private:
    int fd = -1;
};
}
using CFileDescriptor = Hyprutils::OS::CFileDescriptor;
