#pragma once
#include <stdint.h>
#include <stddef.h>

#define POLLIN 1

struct pollfd {
    int fd;
    short events;
    short revents;
};

inline int poll(struct pollfd *fds, unsigned int nfds, int timeout) { return -1; }
