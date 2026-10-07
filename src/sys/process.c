// process.c - user process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "process.h"
#include "elf.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "thread.h"
#include "riscv.h"
#include "trap.h"
#include "memory.h"
#include "heap.h"
#include "error.h"

// COMPILE-TIME PARAMETERS
//


#ifndef NPROC
#define NPROC 16
#endif



// INTERNAL FUNCTION DECLARATIONS
//


static int build_stack(void * stack, int argc, char ** argv);


static void fork_func(struct condition * forked, struct trap_frame * tfr);

// INTERNAL GLOBAL VARIABLES
//


static struct process main_proc;


static struct process * proctab[NPROC] = {
    &main_proc
};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert (memory_initialized && heap_initialized);
    assert (!procmgr_initialized);

    main_proc.idx = 0;
    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

#define ROUND_DOWN(n,k) ((n)/(k)*(k))

int process_exec(struct io * exeio, int argc, char ** argv) {
    //return 0; 
    //void * new_page_phys = alloc_phys_page(); //allocate new page
    //int newpg_stksz = build_stack(new_page_phys, argc, argv); //Copy argv and strings into new page
    
    reset_active_mspace(); //unmap pages mapped to user address space, free backing physical pages

    uintptr_t new_vma = UMEM_END_VMA - PAGE_SIZE; //starting vma for new page
    int flags = (PTE_R | PTE_W | PTE_U) & (~(PTE_A | PTE_D));

    alloc_and_map_range(new_vma, 1, flags);

    int newpg_stksz = build_stack((void*)new_vma, argc, argv);

    //map_page(new_vma, new_page_phys, PTE_R | PTE_W | PTE_U); //map the new page at the top of the new user process stack with all flags set

    void (*new_entry_ptr)(struct io*);
    //int elfload_ret = elf_load(exeio, new_entry_ptr); //load the executable
    elf_load(exeio, (void (**)(void)) &new_entry_ptr); //load the executable

    struct trap_frame tfr;
    memset(&tfr, 0, sizeof(struct trap_frame));
    // struct trap_frame *tfr = kcalloc(1, sizeof *tfr);
    tfr.sepc = new_entry_ptr;

    tfr.sp = (void*)(UMEM_END_VMA - newpg_stksz);
    //tfr.sp = (void*)(ROUND_DOWN((uintptr_t)(tfr.sp), 16));

    tfr.a0 = (uint64_t)argc;
    tfr.a1 = UMEM_END_VMA - newpg_stksz; //argv on stack is at bottom of stack, same as sp

    uint64_t cur_sstatus = csrr_sstatus(); // Correct version using riscv.h
    cur_sstatus &= ~RISCV_SSTATUS_SIE; // SIE 0 to turn off interrupts before jumping
    cur_sstatus |= RISCV_SSTATUS_SPIE; // SPIE 1 for turning on interrupts after jump
    // cur_sstatus &= ~RISCV_SSTATUS_SPP; // SPP 0 to go to user mode
    cur_sstatus |= RISCV_SSTATUS_SUM;
    tfr.sstatus = cur_sstatus;

    void * thread_stack_anchor = get_running_thread_stack_anchor();
    trap_frame_jump(&tfr, thread_stack_anchor);

}

int process_fork(const struct trap_frame * tfr) {
    struct process * parent = current_process();
    struct process * child = kcalloc(1, sizeof(struct process));
    
    int child_pid = -1;
    for (int procid = 0; procid < NPROC; procid++) { //find open process in table
        if (proctab[procid] == NULL) {
            child_pid = procid;
            child->idx = child_pid;
            proctab[procid] = child;
            break;
        }
    }
    if (child_pid < 0) { //if none available return error
        return -EINVAL;
    }

    child->mtag = clone_active_mspace(); //copy address space

    for (int ioid = 0; ioid < PROCESS_IOMAX; ioid++) { //copy io file descriptors
        if (parent->iotab[ioid] != NULL) {
            child->iotab[ioid] = parent->iotab[ioid];
            ioaddref(child->iotab[ioid]);
        }
    }

    //child trap frame
    //struct trap_frame * child_tfr = kcalloc(1, sizeof(struct trap_frame));
    //memcpy(child_tfr, tfr, sizeof(struct trap_frame)); //copy from parent to child

    struct condition child_done;
    condition_init(&child_done, "child_done"); //condition to wait for child

    int child_tid = thread_spawn("child_proc", (void (*)(void))fork_func, &child_done, tfr);
    if (child_tid < 0) {
        return child_tid; //return error code from thread_spawn
    }
    child->tid = child_tid;
    thread_set_process(child_tid, child);

    condition_wait(&child_done);

    return child_tid;
    //return 0;
}

void process_exit(void) {
    struct process * curproc = current_process();

    if (running_thread() == 0) {
        panic("Main process exited.");
    }

    for (int proc_io_idx = 0; proc_io_idx < PROCESS_IOMAX; proc_io_idx++) {
        if (curproc->iotab[proc_io_idx] != NULL) {
            ioclose(curproc->iotab[proc_io_idx]); //close all open io interfaces
            curproc->iotab[proc_io_idx] = NULL;
        }
    }
    discard_active_mspace(); //clear process memory space

    proctab[curproc->idx] = NULL;

    thread_exit(); //exit current running thread
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

void fork_func(struct condition * done, struct trap_frame * tfr) {
    //return;
    //struct process * proc = current_process();

    tfr->a0 = 0; //child return value
    //switch_mspace(proc->mtag); //switch to child memspace
    condition_broadcast(done); //wake up parent
    trap_frame_jump(tfr, get_running_thread_stack_anchor()); //jump to user space

}


mtag_t kernel_mspace_tag(void)
{
    return main_proc.mtag;
}