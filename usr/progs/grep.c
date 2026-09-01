// YOUR CODE HERE
#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"

void main(int argc, char** argv) {

    char buf[512];
    char cur_line[512];
    int line_len = 0;
    int inp_count = 0;
    int match = 0;
    //int found = 0;

    if (argc < 3 ){
        dprintf(STDOUT, "%s", "Not enough args");
    }
    else{
        int i = 0;
        while (argv[1][i] != '\0') {
            inp_count++;
            i++;
        }

        for (int v = 2; v < argc; v++){
        line_len = 0;
        int fd = _open(-1, argv[v]);
        if (fd < 0){
            dprintf(2, "%s", "FAILED TO OPEN");
        }
        int n = _read(fd, buf, 512);
        if (n < 0){
            dprintf(2, "%s", "FAILED TO READ");
        }
         while (n > 0){
            for (int i = 0; i < n; i++){
                if (buf[i] == '\n') {
                    match = 0;
                    for (int j = 0; j <= line_len - inp_count; j++){
                        match = 1;
                        for (int k = j; k < j+inp_count; k++){
                            if (cur_line[k] != argv[1][k-j]){
                                match = 0;
                            }
                        }
                        if (match){
                            break;
                        }
                    }

                    if (match) {
                        _write(STDOUT, cur_line, line_len);
                        _write(STDOUT, "\n", 1);
                    }

                    line_len = 0;
                }
                else {
                cur_line[line_len] = buf[i];
                line_len++;
                }
            }
            n = _read(fd, buf, 512);
        }
            if (line_len > 0){
                for (int j = 0; j <= line_len - inp_count; j++){
                    match = 1;
                    for (int k = j; k < j+inp_count; k++){
                        if (cur_line[k] != argv[1][k-j]){
                            match = 0;
                        }
                    }
                    if (match){
                        break;
                    }
                }

                    if (match) {
                        _write(STDOUT, cur_line, line_len);
                        _write(STDOUT, "\n", 1);
                    }
            }
            _close(fd);
        }
    }
}
