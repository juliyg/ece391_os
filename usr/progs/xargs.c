// YOUR CODE HERE
#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"


void main(int argc, char** argv) {

    if (argc <= 1){
        return;
    }

    char buf[512];
    char *inp_argv[64];
    char cur_word_entry[64][64];
    char arg[64];

    if (argv[1][0] == '/'){
       int j = 0;
       while (argv[1][j]){
            arg[j] = argv[1][j];
            j++;
        }
        arg[j] = '\0';
    }
    else{
        arg[0] = '/';
        arg[1] = 'c';
        arg[2] = '/';

        int j = 0;
        while (argv[1][j]){
            arg[j+3] = argv[1][j];
            j++;
        }
        arg[j+3] = '\0';
    }

    inp_argv[0] = arg;
    
    int word_count  =  1;
    int prev_char = 0;

    int n = _read(STDIN, buf, 256);
    if (n < 0){
        dprintf(1, "%s", "FAILED TO READ");
        dprintf(1, "%s", "FAILED TO READ");
    }

    //count number of words in stdin
    int str_idx = 0;
    char cur_word[64];
    while (n > 0){
        for (int i = 0; i < n; i++){
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n'){
                if (prev_char){
                    strncpy(cur_word_entry[word_count], cur_word, str_idx);
                    cur_word_entry[word_count][str_idx] = '\0';
                    inp_argv[word_count] = cur_word_entry[word_count];
                    strncpy(cur_word_entry[word_count], cur_word, str_idx);
                    cur_word_entry[word_count][str_idx] = '\0';
                    inp_argv[word_count] = cur_word_entry[word_count];
                    str_idx = 0;
                    word_count++;

                }
            }
            if (!(buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')){
                prev_char = 1;
                cur_word[str_idx] = buf[i];
                str_idx++;
                
            }
            else{
                prev_char = 0;
            }
        }
        n = _read(0, buf, 256);
    }
    if (prev_char){
        strncpy(cur_word_entry[word_count], cur_word, str_idx);
        cur_word_entry[word_count][str_idx] = '\0';
        inp_argv[word_count] = cur_word_entry[word_count];
        strncpy(cur_word_entry[word_count], cur_word, str_idx);
        cur_word_entry[word_count][str_idx] = '\0';
        inp_argv[word_count] = cur_word_entry[word_count];
        word_count++;

    }

    //dprintf(1, "made it to the end");
    inp_argv[word_count] = NULL;

    int fd = _open(-1, inp_argv[0]);
    _exec(fd, word_count, inp_argv);
}