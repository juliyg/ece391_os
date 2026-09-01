// syscall.c - System call handling
//
// Copyright (c) 2024-2026 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "riscv.h"
#include <stdint.h>
#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "misc.h"
#include "process.h"
#include "scnum.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "io.h"

#ifndef PATH_MAX
// Maximum path name length. Must be less than HEAP_ALLOC_MAX.
#define PATH_MAX 255
#endif

// for ab1043 age calculations
#define NSEC_PER_SEC (1000000000UL)
#define NSEC_PER_MIN (60UL * NSEC_PER_SEC)
#define NSEC_PER_HR (60UL * NSEC_PER_MIN)
#define NSEC_PER_DAY (24UL * NSEC_PER_HR)
#define TICK_TO_DAYS(t) ((t) / NSEC_PER_DAY)

#define DAYS_TO_YR_APPX(d) ((d) * 400 / 146097)
#define LEAP_YEAR(y) (((y)%4 == 0 && (y)%100 != 0) || ((y)%400 == 0))

static unsigned long long udob; 
static int dob_initialized; 



// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame *tfr);  // called from excp.c


// INTERNAL FUNCTION DECLARATIONS
//

static long syscall(struct trap_frame *tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char ** argv);
static int sysfork(const struct trap_frame * tfr);
static int syswait(int tid);
static int sysprint(const char * msg);
static int sysusleep(unsigned long us);
static int sysfork_nc(struct trap_frame * tfr); 

static int sysdelete(const char * path);
static int syscreate(const char * path);

static int sysopen(int fd, const char * path);
static int sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int sysioctl(int fd, int cmd, uintptr_t arg_uma);
static int syspipe(int * wfd, int * rfd);
static int sysiodup(int oldfd, int newfd);
static int sysab1043(unsigned long long dob);
static int systhrinfo(void); 

static int allocfd(struct process * proc, int reqfd, int notfd);

// EXPORTED FUNCTION DEFINITIONS
//

void handle_syscall(struct trap_frame * tfr) {
    // set the return result into the trap_frame so it is recieved by user space caller
    tfr->a0 = syscall(tfr); 
}

// INTERNAL FUNCTION DEFINITIONS
//

long syscall(struct trap_frame * tfr) {
    switch(tfr->a7) {
        case SYSCALL_EXIT:
            return sysexit();
        case SYSCALL_EXEC:
            return sysexec(tfr->a0, tfr->a1, (char**) tfr->a2);
        case SYSCALL_WAIT:
            return syswait(tfr->a0); 
        case SYSCALL_PRINT:
            return sysprint((char *) tfr->a0); 
        case SYSCALL_USLEEP:
            return sysusleep(tfr->a0); 
        case SYSCALL_CREATE:
            return syscreate((char *)tfr->a0); 
        case SYSCALL_DELETE:
            return sysdelete((char *) tfr->a0);
        case SYSCALL_OPEN:
            return sysopen(tfr->a0, (char*) tfr->a1); 
        case SYSCALL_CLOSE:
            return sysclose(tfr->a0); 
        case SYSCALL_READ:
            return sysread(tfr->a0, (void *) tfr->a1, tfr->a2); 
        case SYSCALL_WRITE:
            return syswrite(tfr->a0, (void *) tfr->a1, tfr->a2); 
        case SYSCALL_IOCTL:
            return sysioctl(tfr->a0, tfr->a1, tfr->a2);
        case SYSCALL_IODUP:
            return sysiodup(tfr->a0, tfr->a1);
        case SYSCALL_FORK: {
            // the child process, will modify tfr->sepc in order to return through the jump_trap_frame pathway
            // we need to save state for the parent process, in order to return to the correct position, since would otherwise double increment sepc
            void * sepc = tfr->sepc; 
            int result = sysfork(tfr); 
            tfr->sepc = sepc; 
            return result; 
        }
        case SYSCALL_PIPE: 
            return syspipe((int*) tfr->a0, (int*) tfr->a1); 
        case SYSCALL_AB1043: 
            return sysab1043(tfr->a0); 
        case SYSCALL_THRINFO:
            return systhrinfo(); 
        default: 
            return -ENOTSUP; 
    }
}

int sysexit(void) {
    process_exit(); 
    return 0;
}

int sysexec(int fd, int argc, char ** argv) {
    struct process * cur_proc = current_process(); 
    struct io * io;

    // clear fd and check fd 
    if(!(fd >= 0 && fd < PROC_IOMAX) || cur_proc->iotab[fd] == NULL) {
        return -EBADF; 
    }

    io = cur_proc->iotab[fd];
    // validate vector
    if(argc < 0) {
        return -EINVAL; 
    }
    
    int result;
    result = enforce_vptr(argv, sizeof(uintptr_t) * (argc + 1), PTE_U | PTE_W | PTE_R); 
    if(result < 0) {
        return result; 
    }

    if(argv[argc] != NULL) {
        return -EINVAL; 
    }

    for(int i = 0; i < argc; i++) {
        result = validate_vstr(argv[i], PTE_U);
        if(result < 0) {
            return result; 
        }
    }
    cur_proc->iotab[fd] = NULL;
    return process_exec(io, argc, argv); 
}

int sysfork(const struct trap_frame * tfr) {
    return process_fork(tfr);
}


int syswait(int tid) {
    return join_thread(tid); 
}

int sysprint(const char *msg) {
    int result;
    if( ( result = validate_vstr(msg, PTE_U) ) < 0)
        return result; 
    kprintf(msg);
    return 0; 
}

int sysusleep(unsigned long us) {
    sleep_us(us); 
    return 0;
}

int syscreate(const char *path) {
    int result;
    if( ( result = validate_vstr(path, PTE_U) ) < 0) {
        return result; 
    }

    char path_copy[strlen(path)+1];
    memcpy(path_copy, path, strlen(path) + 1); 
    char * mnt;
    char * flname; 
    parse_path(path_copy, &mnt, &flname); 

    return create_file(mnt, flname);
}

int sysdelete(const char * path) {
    int result;
    if( ( result = validate_vstr(path, PTE_U) ) < 0)
        return result; 

    char path_copy[strlen(path)+1];
    memcpy(path_copy, path, strlen(path) + 1); 
    char * mnt;
    char * flname; 
    parse_path(path_copy, &mnt, &flname); 

    return delete_file(mnt, flname);
}

int sysopen(int fd, const char * path) {
    struct process * cur_proc = current_process(); 
    struct io * filio; 

    if( fd >= PROC_IOMAX )  {
        return -EBADF; 
    }

    // ensure string has user space mappings, then parse into mount and filename 
    int result;
    if( ( result = validate_vstr(path, PTE_U) ) < 0)
        return result; 

    char path_copy[strlen(path)+1];
    memcpy(path_copy, path, strlen(path) + 1); 
    char * mnt;
    char * flname; 
    parse_path(path_copy, &mnt, &flname); 

    if ((result = open_file(mnt, flname, &filio)) < 0) {
        return result;
    }

    if(fd < 0) {
        // check for space inside process's iotab 
        for(int i = 0; i < PROC_IOMAX; i++) {
            if(cur_proc->iotab[i] == NULL) {
                cur_proc->iotab[i] = filio; 
                return i;
            }
        }
    } else if (fd >= 0 && fd < PROC_IOMAX) {
        if(cur_proc->iotab[fd] != NULL) {
            iodropref(cur_proc->iotab[fd]); 
        }
        cur_proc->iotab[fd] = filio; 
        return fd; 
    }

    // prevent memory leak if iotab is full 
    iodropref(filio);
    return -EMFILE; 
}

int sysclose(int fd) {
    struct process * cur_proc = current_process();

    if(!(fd >= 0 && fd < PROC_IOMAX) || cur_proc->iotab[fd] == NULL) {
        return -EBADF; 
    }

    iodropref(cur_proc->iotab[fd]);
    cur_proc->iotab[fd] = NULL; 
    return 0;
}

long sysread(int fd, void * buf, size_t bufsz) {
    struct process * cur_proc = current_process();
    if( !(fd >=0 && fd < PROC_IOMAX) || cur_proc->iotab[fd] == NULL) {
        return -EBADF; 
    }

    int result;
    if ( (result = enforce_vptr(buf, bufsz, PTE_U | PTE_R)) < 0) {
        return result; 
    }

    return ioread(cur_proc->iotab[fd], buf, bufsz); 
}

long syswrite(int fd, const void *buf, size_t len) {
    struct process * cur_proc = current_process();
    if( !(fd >=0 && fd < PROC_IOMAX) || cur_proc->iotab[fd] == NULL) {
        return -EBADF; 
    }

    int result;
    if ( (result = enforce_vptr(buf, len, PTE_U | PTE_R)) < 0) {
        return result; 
    }

    return iowrite(cur_proc->iotab[fd], buf, len); 
}

int sysioctl(int fd, int op, uintptr_t arg_uma) {
    struct process * cur_proc = current_process();
    if( !(fd >=0 && fd < PROC_IOMAX) || cur_proc->iotab[fd] == NULL) {
        return -EBADF; 
    }

    // check if *arg is large enough for reads/writes for each specific case
    int result; 
    switch(op) {
        case IOC_GETBLKSZ:
            return ioctl(cur_proc->iotab[fd], op, NULL); 
        case IOC_RESET: 
            return ioctl(cur_proc->iotab[fd], op, NULL); 
        case IOC_GETEND:
            if( (result = enforce_vptr((const void *) arg_uma, sizeof(unsigned long long), PTE_U | PTE_W)) < 0) {
                return result; 
            }
            return ioctl(cur_proc->iotab[fd], op, (void *) arg_uma);
        case IOC_SETEND:
            if( (result = enforce_vptr((const void *) arg_uma, sizeof(unsigned long long), PTE_U | PTE_R)) < 0) {
                return result; 
            }
            return ioctl(cur_proc->iotab[fd], op, (void *) arg_uma);
        case IOC_GETPOS:
            if( (result = enforce_vptr((const void *) arg_uma, sizeof(unsigned long long), PTE_U | PTE_W)) < 0) {
                return result; 
            }
            return ioctl(cur_proc->iotab[fd], op, (void *) arg_uma);
        case IOC_SETPOS:
            if( (result = enforce_vptr((const void *) arg_uma, sizeof(unsigned long long), PTE_U | PTE_R)) < 0) {
                return result; 
            }
            return ioctl(cur_proc->iotab[fd], op, (void *) arg_uma);
        case IOC_MAPBUF:
            if( (result = enforce_vptr((const void *) arg_uma, sizeof(void *), PTE_U | PTE_R)) < 0) {
                return result; 
            }
            return ioctl(cur_proc->iotab[fd], op, (void *) arg_uma);
        default:
            return ioctl_u(cur_proc->iotab[fd], op, arg_uma); 
    }
}

int syspipe(int * wfdptr, int * rfdptr) {
    struct process * cur_proc = current_process(); 

    // validate pointers 
    int error = enforce_vptr(wfdptr, sizeof(int), PTE_U | PTE_W);
    if(error < 0) {
        return error;
    }

    error = enforce_vptr(rfdptr, sizeof(int), PTE_U | PTE_W);
    if(error < 0) {
        return error;
    }

    // if either is greater than the iotab index max fail
    if(*wfdptr >= PROC_IOMAX || *rfdptr >= PROC_IOMAX) {
        return -EBADF;
    }

    // if they point to each other fail
    if( (*wfdptr >= 0 && *rfdptr >= 0) && (*wfdptr == *rfdptr) ) {
        return -EBADF;
    }

    // create io pipe object 
    struct io * wio;
    struct io * rio; 
    create_iopipe(&wio, &rio); 
    

    // if wfdptr is negative, find an open spot else check if the wfdptr is free
    int wfd = -1, rfd = -1;  
    if(*wfdptr < 0) {
        for(int i = 0; i < PROC_IOMAX; i++) {
            if(cur_proc->iotab[i] == NULL) {
                wfd = i; 
                break;
            }
        }
    } else {
        if(cur_proc->iotab[*wfdptr] == NULL) {
            wfd = *wfdptr;
        } 
    }

    // set the io object so it is not empty on the second scan and check if it succeeded
    // drop ref to prevent resource leak 
    if(wfd < 0) {
        iodropref(wio);
        iodropref(rio); 
        return -EBADF; 
    } else {
        cur_proc->iotab[wfd] = wio; 
    }

    // if rfdptr is negative, find an open spot else check if the wfdptr is free
    if(*rfdptr < 0) {
        for(int i = 0; i < PROC_IOMAX; i++) {
            if(cur_proc->iotab[i] == NULL) {
                rfd = i; 
                break;
            }
        }
    } else {
        if(cur_proc->iotab[*rfdptr] == NULL) {
            rfd = *rfdptr; 
        }
    }

    // if there is not enough room for both fds, return error 
    // make sure we reset the iotab we set previously 
    if(rfd < 0) {
        cur_proc->iotab[wfd] = NULL; 
        iodropref(wio);
        iodropref(rio); 
        return -EBADF; 
    } else {
        cur_proc->iotab[rfd] = rio; 
    }

    *wfdptr = wfd; 
    *rfdptr = rfd; 
    return 0;
}

int sysiodup(int oldfd, int newfd) {
    struct process * cur_proc = current_process();
    if( !(oldfd >=0 && oldfd < PROC_IOMAX) || cur_proc->iotab[oldfd] == NULL) {
        return -EBADF; 
    }

    if( newfd >= PROC_IOMAX) {
        return -EBADF; 
    }

    // check that newfd isn't occupied 
    if(newfd >= 0) {
        if(cur_proc->iotab[newfd] != NULL) {
            return -EBADF; 
        }

        // allocate if it is valid
        cur_proc->iotab[newfd] = ioaddref(cur_proc->iotab[oldfd]);
        return newfd;
    }

    // copy into newfd iotab
    // if < 0, find available
    // otherwise directly copy 
    if(newfd < 0) {
        for(int i = 0; i < PROC_IOMAX; i++) {
            if(cur_proc->iotab[i] == NULL) {
                cur_proc->iotab[i] = ioaddref(cur_proc->iotab[oldfd]); 
                return i;
            }
        }
    } 

    return -EBADF;
}

int sysab1043(unsigned long long dob){
    // save dob on first call 
    if(dob_initialized == 0) {
        udob = dob; 
        dob_initialized = 1;
    }

    // open rtc device and get cur_time 
    struct io * rtc; 
    uint64_t time;
    open_device("rtc", &rtc); 
    ioread(rtc, &time, sizeof(uint64_t)); 
    iodropref(rtc); 
    // calculate age 
    long age = DAYS_TO_YR_APPX(TICK_TO_DAYS(time - udob)); 

    if(age < 13) {
        return 0; 
    } else if (age >= 13 && age < 16) {
        return 1; 
    } else if (age >=16 && age < 18) {
        return 2; 
    } else {
        return 3; 
    }
}


int systhrinfo(void) {
    display_thread_info(); 
    return 0; 
}
