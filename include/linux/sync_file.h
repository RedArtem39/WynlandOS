#pragma once
#include <linux/types.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

struct sync_merge_data {
    char  name[32];
    __s32 fd2;
    __s32 fence;
    __u32 flags;
    __u32 pad;
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE 0x1235 // Just a dummy value for stub
