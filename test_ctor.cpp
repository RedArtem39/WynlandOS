#include <unistd.h>

struct Probe {
    Probe() { write(2, "CTOR-RAN\n", 9); }
};
static Probe g_probe;

int main() {
    write(2, "MAIN-RAN\n", 9);
    return 0;
}
