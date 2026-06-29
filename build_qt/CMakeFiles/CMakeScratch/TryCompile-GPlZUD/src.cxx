 #include <sys/types.h>
#include <sys/socket.h>
#include <net/if.h>
#include <ifaddrs.h>

int main(void)
{
    /* BEGIN TEST: */
ifaddrs *list;
getifaddrs(&list);
freeifaddrs(list);
    /* END TEST: */
    return 0;
}

