#include "../syscall.h"
#include "../shell.h"

int main(int argc, char *argv[]) {
    int i = 0; 
    while(1) {
        for (unsigned long i = 0; i < 1300UL; i++) {
            i++;
        }
        // simulate waiting for user input etc 
        _usleep(1);
    }
    return 0; 
}