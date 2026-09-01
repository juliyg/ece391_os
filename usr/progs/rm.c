// YOUR CODE HERE

#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"

void main(int argc, char** argv) {

    if (argc <= 1) {
       dprintf(2, "%s", "EMPTY ARGUMENT\n");
       return;
    }

    else {
        for (int i = 1; i < argc; i++){
            int n = _delete(argv[i]);
            if (n < 0){
            dprintf(2, "%s", "FAILED TO DELETE\n");
            }
        }
    }

}
