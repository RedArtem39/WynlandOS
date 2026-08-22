#include <stdio.h>
#include <dirent.h>
#include <string.h>

int main(void) {
    DIR *d = opendir("/plugins/platforms");
    if (!d) { printf("opendir failed\n"); return 1; }
    struct dirent *e;
    int i = 0;
    while ((e = readdir(d)) != NULL) {
        printf("entry %d: name='%s' len=%d type=%d\n", i, e->d_name, (int)strlen(e->d_name), e->d_type);
        i++;
        if (i > 10) break;
    }
    closedir(d);
    printf("done, %d entries\n", i);
    return 0;
}
