#pragma once
#include <stdint.h>
#include <stddef.h>

#define FIONREAD  0x541B
#define FIONBIO   0x5421
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

/* Linux ioctl request-number encoding, from <asm-generic/ioctl.h> --
   not provided by this stub's musl-shadowing sibling headers, needed by
   evdev-style callers that build request numbers with _IOR/_IOW/_IOWR. */
#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS  2

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT+_IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT+_IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT+_IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC(dir,type,nr,size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

#define _IO(type,nr)        _IOC(_IOC_NONE,(type),(nr),0)
#define _IOR(type,nr,size)  _IOC(_IOC_READ,(type),(nr),(sizeof(size)))
#define _IOW(type,nr,size)  _IOC(_IOC_WRITE,(type),(nr),(sizeof(size)))
#define _IOWR(type,nr,size) _IOC(_IOC_READ|_IOC_WRITE,(type),(nr),(sizeof(size)))

#ifdef __cplusplus
extern "C" {
#endif
inline int ioctl(int fd, unsigned long request, ...) { return -1; }
#ifdef __cplusplus
}
#endif
