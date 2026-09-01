// YOUR CODE HERE
#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"

void main(int argc, char** argv) {

    char buf[512];
    int char_count =  0;
    int line_count  =  0;
    int word_count  =  0;
    int prev_char = 0;

    if (argc <= 1){
        int n = _read(0, buf, 512);
        if (n < 0){
            dprintf(2, "%s", "FAILED TO READ");
        }
        while (n > 0){
            for (int i = 0; i < n; i++){
                if (buf[i] == '\n') {
                    line_count++;
                    if (prev_char){
                        word_count++;
                    }
                }
                else if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r'){
                    if (prev_char){
                        word_count++;
                    }
                }
                if (!(buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')){
                    prev_char = 1;
                }
                else{
                    prev_char = 0;
                }
                char_count++;
            }
            n = _read(0, buf, 512);
        }
        if (prev_char){
            word_count++;
        }
    }
    else{
        int fd = _open(-1, argv[1]);
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
                    line_count++;
                    if (prev_char){
                        word_count++;
                    }
                }
                else if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r'){
                    if (prev_char){
                        word_count++;
                    }
                }
                if (!(buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')){
                    prev_char = 1;
                }
                else{
                    prev_char = 0;
                }
                char_count++;
            }
            n = _read(fd, buf, 512);
        }
            
        if (prev_char){
            word_count++;
        }   
    }
    dprintf(1, "%d  %d  %d\n", line_count, word_count, char_count);
}