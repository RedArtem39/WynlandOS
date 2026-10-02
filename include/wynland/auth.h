#ifndef WYNLAND_AUTH_H
#define WYNLAND_AUTH_H

#include <wynland/types.h>

bool auth_root_password_set(void);
bool auth_check_root(const char *pw);
bool auth_set_root(const char *pw);

#endif
