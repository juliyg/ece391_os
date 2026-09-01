#include "../syscall.h"
#include "../shell.h"

int main(int argc, char *argv[]) {
    unsigned long x = 0;

    for (;;) {
        for (unsigned long i = 0; i < 13000UL; i++) {
            x += i;
        }

        // Use sleep if available, otherwise yield.
        _usleep(1);
    }

    return 0;
}