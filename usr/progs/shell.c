// shell.c - A basic shell for 391 OS
//
// Copyright (c) 2026 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "../syscall.h"
#include "../string.h"
#include "../shell.h"
#include "../error.h"
#include "../io.h"
#include <string.h>

// INTERNAL CONSTANT DEFINITIONS
//

#define BUFSIZE 256
#define MAXARGS 64

#define SKIP_SPACES(buf) while(*buf == ' ') buf++
#define RST_IO()                    \
    do {                            \
        _close(STDIN);              \
        _iodup(CONSOLEOUT, STDIN);  \
        _close(STDOUT);             \
        _iodup(CONSOLEOUT, STDOUT); \
    } while (0)

// INTERNAL FUNCTION DECLARATIONS
//
static void __attribute__ ((noreturn)) exec(int argc, char* argv[]);
static void handle_sq(int argc, char* argv[]);
static void handle_bg(int argc, char* argv[]);
static int handle_file_input(char* path);
static int handle_file_output(char* path);
static void handle_pipe(int argc, char* argv[]);
static void __attribute__ ((noreturn)) parse_and_exec(char* head);


// Executes the file in argv[0], passing in argc and argv. If argv[0] is not a
// valid file path, it should prepend '/c/' before executing.
//
// On entry exec() assumes:
// - /argc/ >= 1.
// - /argv[0]/ is non-NULL and points to a NUL-terminated string.
//
// This function should not return.
// - on success, executes argv[0], clearing the current memory space.
// - on failure, print an error message and immediately exit.
static void exec(int argc, char* argv[]) {
    // check if it is an absolute path 
    char arg_new[BUFSIZE]; 
    if(argv[0][0] != '/') {
        char mnt[3] = "/c/";
        memcpy(arg_new, mnt, sizeof(mnt)); 

        // check if argv is a valid size 
        if(strlen(argv[0]) + 1 > BUFSIZE - 3) {
            _print("Invalid argument size\n");
            _exit(); 
        }

        // finish creating the new path name
        memcpy(arg_new + sizeof(mnt), argv[0], strlen(argv[0]) + 1); 
    } else {
        if(strlen(argv[0]) + 1 > BUFSIZE) {
            _print("Invalid argument size\n"); 
            _exit(); 
        }

        // create the new string path 
        memcpy(arg_new, argv[0], strlen(argv[0]) + 1); 
    }

    // open the file 
    int fd = _open(-1, arg_new); 
    if(fd < 0) {
        _print("Inputted file name does not exist\n"); 
        _exit(); 
    }
    _exec(fd, argc, argv); 
    _exit();
}

// Execs with /argc/ and /argv/, waits for execution to complete, then 
// returns
//
// On entry handle_sq() assumes:
// - /argc/ >= 1.
// - /argv[0]/ is non-NULL and points to a NUL-terminated string.
//
// On return handle_sq() guarantees (on both success and failure):
// - A process with /argc/ and /argv/ has been exec'd and has completed
//   execution.
static void handle_sq(int argc, char* argv[]) {
    // clone the shell process, so the command will execute as a user program in the clone 
    // the shell process will wait for the command to finish 
    int pid = _fork(); 
    if(pid == 0) {
        exec(argc, argv); 
    } 
    _wait(pid); 
}

// Execs with /argc/ and /argv/, returns immediately
//
// On entry handle_bg() assumes:
// - /argc/ >= 1.
// - /argv[0]/ is non-NULL and points to a NUL-terminated string.
//
// On return handle_bg() guarantees (on both success and failure):
// - A process with /argc/ and /argv/ has been exec'd (may not have 
//   finished execution).
static void handle_bg(int argc, char* argv[]) {
    // the shell process doesn't wait for the command to finish 
    int pid = _fork(); 
    if(pid == 0) {
        exec(argc, argv); 
    } 
}

// Redirects STDIN to /path/.
//
// On entry handle_file_input() assumes:
// - /path/ is non-NULL and points to a NUL-terminated string.
//
// On return handle_file_input() guarantees (on success):
// - STDIN is redirected to the file in /path/.
// - on failure, handle_file_input() returns a negative error code.
static int handle_file_input(char* path) {
    int error = _open(STDIN, path); 
    return error;
}

// Redirects STDOUT to /path/. Attemptes to create /path/ if it does not exist.
//
// On entry handle_file_output() assumes:
// - /path/ is non-NULL and points to a NUL-terminated string.
//
// On return handle_file_output() guarantees (on success):
// - /path/ will exist and STDOUT will be redirected to be /path/.
// - on failure, handle_file_output() returns a negative error code.
static int handle_file_output(char* path) {
    // attempt to open the file for redirection
    int error = _open(STDOUT, path); 

    // check if file was successfully opened 
    if(error < 0) {

        // attempt to create a file 
        error = _create(path); 

        if(error < 0) {
            return error; 
        }
        // open the created file 
        error = _open(STDOUT, path); 
        if(error < 0) {
            return error; 
        }
    }

    // truncate the file match unix like stuff
    unsigned long zero = 0; 
    error = _ioctl(STDOUT, IOC_SETEND, &zero); 
    if(error < 0) {
        return error; 
    }
    // reset the write position 
    error = _ioctl(STDOUT, IOC_SETPOS, &zero); 
    return error;
}

// Creates a pipe and a reader and writer process. The writer's STDOUT is 
// redirected to the input of the pipe and the reader's STDIN is redirected
// to the output of the pipe. The writer will immediately exec with argc and
// argv, while the reader will return to continue parsing the remainder of the
// input.
//
// On entry handle_pipe() assumes:
// - /argc/ >= 1.
// - /argv[0]/ is non-NULL and points to a NUL-terminated string.
//
// On return handle_pipe() guarantees (on success):
// - The writer's STDOUT is redirected to the input of a newly created pipe and
//   has exec'ed with /argc/ and /argv/.
// - The reader's STDIN is redirected to the input of said pipe and has 
//   returned.
// - on failure, handle_pipe() exits immediately.
static void handle_pipe(int argc, char* argv[]) {
    int wfdptr = -1; 
    int rfdptr = -1; 

    // get the new fds for the files 
    int error = _pipe(&wfdptr, &rfdptr); 

    // spawn a child, the child will be the lhs argument for the pipe 
    int pid = _fork(); 

    
    if(pid == 0) {
        if(error < 0) {
            _exit(); 
        }
        // our STDOUT for the lhs should still be the same as before, we dont need this wfdptr 
        _close(rfdptr); 

        // free up space so we can replace STDOUT
        _close(STDOUT);

        // move the new STDOUT into STDOUT fd location
        _iodup(wfdptr, STDOUT); 

        //remove duplicate 
        _close(wfdptr); 
        exec(argc, argv); 
    }

    // process for the rhs of the argument, we want STDOUT to be console stil

    // rhs is not writing to a pipe right now
    _close(wfdptr);

    // free up space so we can replace STDIN
    _close(STDIN);

    //move the new STDIN into STDin fd location
    _iodup(rfdptr, STDIN); 

    _close(rfdptr); 
}

// checks if the current character is a terminator (<, |, \0, etc)
static int is_terminator(char c) {
    switch (c) {
        case ' ':
        case '\0':
        case SQ:
        case BG:
        case FIN:
        case FOUT:
        case PIPE:
            return 1;
        default:
            return 0;
    }
}

// takes in an input string, returns the terminator character 
// sets *end to the string starting at the first terminator character 
static char find_terminator(char* head, char** end) {
    *end = head;
    while (!is_terminator(**end)) (*end)++;
    return **end;
}


static void parse_and_exec(char* head) {
    int argc;
    char* argv[MAXARGS + 1]; // +1 for NULL termination
    char* end;
    char term;
    int res;

    // skip spaces before the string (___string) --> (string)
    SKIP_SPACES(head);

    // handle args
    // example 
    for (argc = 0; argc < MAXARGS;) {
        // find the terminator for the head string 
        term = find_terminator(head, &end);

        // isolate the head string by replacing the terminator with the null terminator (string\0)
        *end = '\0';
        
        // if head = \0, that means string is a null string so we dont add it to the argv
        if (head != end)
            argv[argc++] = head;
        
        // if the terminator is not space, it is |, \0, > etc and we break from the loop to do some work
        if (term != ' ') break;

        // otherwise we start the second iteration, and remove any spaces before we get to the next string
        end++;
        SKIP_SPACES(end);
        head = end;
    }

    if (argc == 0) _exit(); // nothing to do

	// Null-terminate the argument array
	argv[argc] = NULL;

    // at this point, anything remaining should be redirection

    // for non redirection terminators
    // this breaks the parsing problem recursively, current argc, and argv contain the command and arguments
    // we recurse onto the next portion of the string with a fresh string until all strings are exhuasted 

    // for redirection terminators we keep parsing until we get to the last redirection terminator 

    while (term != '\0') {
        head = end + 1;
        SKIP_SPACES(head);
        switch (term) {
        case SQ: // run sequentially
            handle_sq(argc, argv);
            // prev cmd has been exec'd and finished
            // reset any redirections and exec the rest
            RST_IO();
            parse_and_exec(head);
            
        case BG: // run in background
            handle_bg(argc, argv);
            // prev cmd has been exec'd, and is running in the background
            // reset any redirections and exec the rest
            RST_IO();
            parse_and_exec(head);

        case FIN: // file input redirection
            term = find_terminator(head, &end);
            *end = '\0';
            res = handle_file_input(head);
            if (res < 0) _exit();
            // we may have more redirection, so we continue
            break;

        case FOUT: // file output redirection
            term = find_terminator(head, &end);
            *end = '\0';
            res = handle_file_output(head);
            if (res < 0) _exit();
            // we may have more redirection, so we continue
            break;

        case PIPE: // set up pipe
            handle_pipe(argc, argv);
            // writer has been exec'd and is writing into the pipe
            // now parse and exec reader, who is reading from the pipe
            parse_and_exec(head);
        }

        if (term == ' ') {
            end++;
            SKIP_SPACES(end);
            term = *end;
        }
    }

    exec(argc, argv);
}

void main() {
    char buf[BUFSIZE];

    buf[BUFSIZE-1] = '\0'; // terminate

    RST_IO();

    // Your starting prompt
	printf("Starting Justin and Vineet's mighty Shell\n");

	for (;;) {
        // Your shell prompt
        // Make sure your prompt ends in one of '>', '#', '$', '%'
		printf(/* CHANGE ME */ "Orange Chicken OS> ");
		getsn(buf, BUFSIZE - 1);

		if (0 == strcmp(buf, "exit"))
			return;

        // design idea: base shell, parser, exec processes
        int pid = _fork(); 
        if(pid == 0) {
            parse_and_exec(buf); 
        }
        _wait(pid); 
	}
}
