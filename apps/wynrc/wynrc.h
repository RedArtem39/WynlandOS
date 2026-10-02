// WynlandOS - wynrc: shared definitions of the init (wynrc.c) and the
// rc-service / rc-status / rc-update tool (rc.c).
#pragma once

#define WYNRC_SOCKET      "\0wynrc"              /* abstract AF_UNIX name */
#define WYNRC_SOCKET_LEN  6
#define WYNRC_SERVICES    "/etc/wynrc/services"
#define WYNRC_RUNLEVELS   "/etc/wynrc/runlevels"
#define WYNRC_BOOTCFG     "/etc/wynland/boot.cfg"
#define WYNRC_HWFACTS     "/etc/wynland/hw"
