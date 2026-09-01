// process.c - Processes and process manager
//
// Copyright (c) 2024-2026 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "process.h"

#include "conf.h"
#include "elf.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"
#include "trap.h"
#include "io.h"
#include "console.h"


// INTERNAL FUNCTION DECLARATIONS
//

static int build_stack(void * stack, int argc, char ** argv);

static void fork_func(struct condition * forked, struct trap_frame * tfr);

// INTERNAL GLOBAL VARIABLES
//

static struct process main_proc;

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert(memory_initialized && heap_initialized);
    assert(!procmgr_initialized);

    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_attach_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

int process_exec(struct io * exeio, int argc, char ** argv) {
    // set the trap frame to point to below the stack anchor (this is fine since in the init case we abandon the main stack)
    // the main.c in the kernel is not returned to so we can clobber the stack 
    struct trap_frame * trap_frame = running_thread_stack_anchor() - sizeof(struct trap_frame); 

    // allocate physical page (one to one mapping with kernel vma)
    void * kernel_stack = alloc_phys_page(); 
    
    // Make argv[i] point to virtual address space corresponding to data (top of user address space)
    // Copy data from strings into the kernel page, above the arguments 

    // make sure s-mode can access user mapped virtual memory 
    csrs_sstatus(RISCV_SSTATUS_SUM); 

    int stksz; 
    if ( (stksz = build_stack(kernel_stack, argc, argv)) < 0) {
        free_phys_page(kernel_stack); 
        iodropref(exeio); 
        return stksz; 
    }

    // reset non-global mappings (any current userspace mappings)
    reset_active_mspace(); 

    // Map the kernel page to the top of the user address space (the argv[i] now point to the copied)
    map_page(UMEM_END_VMA - PAGE_SIZE, kernel_stack, PTE_W | PTE_R | PTE_U); 

    // entry point for the new executable (cast to function pointer pointer) 
    if (elf_load(exeio, (void (**) (void)) &trap_frame->sepc) < 0) {
        process_exit(); 
    }

    // drop the reference to the executable io object that the kernel has 
    iodropref(exeio); 

    // setup sp for the user program
    trap_frame->sp = (void *) (UMEM_END_VMA - stksz); 

    // provide arguments to entrypoint of the process 
    trap_frame->a0 = argc; 
    trap_frame->a1 = (long) trap_frame->sp; 

    // set sstatus, so that usermode is reached and interrupts are enabled in usermode
    trap_frame->sstatus = RISCV_SSTATUS_SPIE | RISCV_SSTATUS_SUM; 

    // Enter entrypoint of the process 
    trap_frame_jump(trap_frame, trap_frame);
    return 0; 
}

int process_fork(const struct trap_frame * tfr) {
    struct process * curr_proc = current_process(); 

    // allocate memory for the child process metadata 
    struct process * child_proc = kcalloc(1, sizeof(struct process)); 

    // create child start condition variable
    struct condition child_start; 
    condition_init(&child_start, NULL);

    // attempt to spawn a new kernel thread for the process 
    // fork_func serves as a state restore to the user mode of the child process using the parent's tfr (wrapper around trap_frame_jump)
    int new_tid = spawn_thread(NULL, (void (*) (void)) &fork_func, &child_start, tfr);  
    if(new_tid < 0) {
        kfree(child_proc); 
        return new_tid; 
    }

    // initialize child io objects 
    child_proc->exeio = NULL; 
    for(int i = 0; i < PROC_IOMAX; i++) {
        if (curr_proc->iotab[i] == NULL) {
            child_proc->iotab[i] = NULL;
            continue; 
        } else {
            child_proc->iotab[i] = ioaddref(curr_proc->iotab[i]); 
        }
    }
    

    child_proc->tid = new_tid; 
    thread_attach_process(new_tid, child_proc);    

    // clone address space and attach process struct 
    mtag_t new_mtag = clone_active_mspace(); 
    child_proc->mtag = new_mtag;

    // do not return until the child process begins (prevents parent thread from corrupting stack frame after returning to usermode)
    condition_wait(&child_start);     
    
    // return the tid of the new child_process, this follows the typical syscall path -> restores tfr->a0
    return new_tid; 
}

void fork_func(struct condition * done, struct trap_frame * tfr) {
    // broad cast that the child has entered u-mode 
    condition_broadcast(done); 
    tfr->a0 = 0; 
    tfr->sepc += 4;
    trap_frame_jump(tfr, running_thread_stack_anchor() - sizeof(struct trap_frame)); 
}


void process_exit(void) {
    trace("PROCESS EXIT: tid=%d\n", current_process()->tid);
    struct process * curr_proc = current_process();
    // if curr_proc wraps the main kernel thread, writeback cache and shutdown os 
    if(curr_proc->tid == 0 ) {
        flush_all_filesys(); 
        shutdown(); 
    } else {
        // drop references to all io objects 
        for(int i = 0; i < PROC_IOMAX; i++) {
            if(curr_proc->iotab[i] != NULL) {
                iodropref(curr_proc->iotab[i]); 
            }
        }

        // free address space and mappings of the process 
        discard_active_mspace();

        // detach process from thread, then free the process metadata
        thread_attach_process(curr_proc->tid, NULL); 
        kfree(curr_proc); 
        curr_proc = NULL;
        exit_running_thread(); 
        panic("exit_running_thread returned");
    }
}

// INTERNAL FUNCTION DEFINITIONS
//

int build_stack(void * stack, int argc, char ** argv) {
    size_t stksz, argsz;
    uintptr_t * newargv;
    char * p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc)
        return -ENOMEM;
    
    stksz = (argc+1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i])+1;
        if (PAGE_SIZE - stksz < argsz)
            return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert (stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv+argc+1);                

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i])+1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}
