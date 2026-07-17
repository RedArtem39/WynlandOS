#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char uuid_t[16];

inline void uuid_generate_random(uuid_t out) {
    for (int i = 0; i < 16; ++i) out[i] = 0;
}

inline void uuid_unparse(const uuid_t uu, char* out) {
    for (int i = 0; i < 36; ++i) out[i] = '0';
    out[36] = '\0';
}

#ifdef __cplusplus
}
#endif
