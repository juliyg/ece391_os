// YOUR CODE HERE
#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"

void main(int argc, char** argv) {

    char buf[512];

    if (argc <= 1){
        int n = _read(0, buf, 512);
        while (n > 0){
            _write(1, buf, n);
            n = _read(0, buf, 512);
        }
    }
    else{
        for (int i = 1; i < argc; i++){
            int fd = _open(-1, argv[i]);
            if (fd < 0){
                dprintf(2, "%s", "FAILED TO OPEN");
            }
            else{
                int n = _read(fd, buf, 512);
                while (n > 0){
                    _write(1, buf, n);
                    n = _read(fd, buf, 512);
                }
            }
        }
    }
}