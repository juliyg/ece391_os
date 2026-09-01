// YOUR CODE HERE

#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"

void main(int argc, char** argv) {

    char buf[512];

    if (argc <= 1) {
        int fd = _open(-1, "/");
        if (fd < 0){
            dprintf(2, "%s" , "FAILED TO OPEN");
            return;
        }
        int n = _read(fd, buf, 512);
        while (n > 0){
            _write(1, buf, n); 
            dprintf(1, "%s", "\n");
            n = _read(fd, buf, 512);
            }
    }

    else {
        int fd = _open(-1, argv[1]);
        if (fd < 0){
            dprintf(2, "%s" , "FAILED TO OPEN");
            return;
        }
        int n = _read(fd, buf, 512);
        while (n > 0){
            _write(1, buf, n); 
            dprintf(1, "%s", "\n");
            n = _read(fd, buf, 512);
            }
        }

}