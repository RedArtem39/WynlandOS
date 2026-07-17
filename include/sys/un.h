#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct sockaddr_un {
    unsigned short sun_family;
    char sun_path[108];
};
#define SUN_LEN(ptr) ((size_t)(((struct sockaddr_un *) 0)->sun_path) + strlen((ptr)->sun_path))
