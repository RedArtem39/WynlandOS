#pragma once
#include <stdint.h>
#include <stddef.h>

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_CLOEXEC 02000000
#define SOCK_NONBLOCK 04000
#define SO_PEERCRED 17
#define SOL_SOCKET 1

#define __SOCKADDR_COMMON(sa_prefix) unsigned short sa_prefix##family
#define __SOCKADDR_COMMON_SIZE (sizeof (unsigned short))

struct ucred {
    int pid;
    int uid;
    int gid;
};
struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};
struct sockaddr_storage {
    unsigned short ss_family;
    char __ss_padding[128 - sizeof(unsigned short)];
};
// sockaddr_un, sockaddr_in, socklen_t and SUN_LEN are in system sys/un.h and netinet/in.h

inline int socket(int domain, int type, int protocol) { return -1; }
inline int bind(int sockfd, const struct sockaddr *addr, unsigned int addrlen) { return -1; }
inline int listen(int sockfd, int backlog) { return -1; }
inline int accept4(int sockfd, struct sockaddr *addr, unsigned int *addrlen, int flags) { return -1; }
inline int getsockopt(int sockfd, int level, int optname, void *optval, unsigned int *optlen) { return -1; }
inline int connect(int sockfd, const struct sockaddr *addr, unsigned int addrlen) { return -1; }
inline long send(int sockfd, const void *buf, size_t len, int flags) { return -1; }
