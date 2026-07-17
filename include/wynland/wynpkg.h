/*
 * WynlandOS - Package Manager (wynpkg) Header
 *
 * Phase 6: Package manager implementation
 */
#pragma once

#include <wynland/boot_info.h>

/* Execute a package manager command */
void cmd_wynpkg(BootInfo *info, const char *cmd);
