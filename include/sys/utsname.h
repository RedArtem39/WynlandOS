#pragma once
#include <stdint.h>
#include <stddef.h>
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

inline int uname(struct utsname *buf) {
    if (buf) {
        for(int i=0;i<65;++i) buf->sysname[i] = buf->nodename[i] = buf->release[i] = buf->version[i] = buf->machine[i] = 0;
        buf->sysname[0] = 'W'; buf->sysname[1] = 'y'; buf->sysname[2] = 'n';
    }
    return 0;
}
