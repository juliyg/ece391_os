
#include "../string.h"
#include "../syscall.h"
#include "../shell.h"

#define NUART 2
#define BUFSZ 16
#define SHELL_PATH "/c/shell"
#define UART_PATH "/dev/uart"

#define LEAP_YEAR(y) (((y)%4 == 0 && (y)%100 != 0) || ((y)%400 == 0))

#define NSEC_PER_SEC (1000000000UL)
#define NSEC_PER_MIN (60UL * NSEC_PER_SEC)
#define NSEC_PER_HR (60UL * NSEC_PER_MIN)
#define NSEC_PER_DAY (24UL * NSEC_PER_HR)



int main(int argc, char *argv[]) {
    const char *path = "/c/exec_loop";
    const char *pass_msg = "exec_loop ok\n";
    
    int iter = 0;
    if (argc >= 1 && argv[0][0] >= '0' && argv[0][0] <= '9') {
        iter = argv[0][0] - '0';
    }

    char c = '0' + iter;
    _write(2, &c, 1);
    _write(2, "\n", 1);

    if (iter == 9) {
        _write(2, pass_msg, strlen(pass_msg));
        return 0;
    }

    char next[2];
    next[0] = '0' + (iter + 1);
    next[1] = '\0';

    char *newargv[] = {
        next,
        NULL
    };

    int fd = _open(-1, path);
    if (fd < 0) {
        _write(2, "open failed\n", 12);
        return -1;
    }

    _exec(fd, 1, newargv);

    _write(2, "exec failed\n", 12);
    return -1;
}