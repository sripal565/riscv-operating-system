/*! @file syscall.c
    @brief system call handlers 
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/



#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "scnum.h"
#include "process.h"
#include "memory.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "intr.h"
#include "timer.h"
#include "error.h"
#include "thread.h"
#include "process.h"
#include "fs.h"

// EXPORTED FUNCTION DECLARATIONS
//

extern void handle_syscall(struct trap_frame * tfr); // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame * tfr);

static int sysexit(void);
static int sysexec(int fd, int argc, char ** argv);
static int sysfork(const struct trap_frame * tfr);
static int syswait(int tid);
static int sysprint(const char * msg);
static int sysusleep(unsigned long us);

static int sysdevopen(int fd, const char * name, int instno);
static int sysfsopen(int fd, const char * name);

static int sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int sysioctl(int fd, int cmd, void * arg);
static int syspipe(int * wfdptr, int * rfdptr);

static int sysfscreate(const char* name); 
static int sysfsdelete(const char* name);
static int sysiodup(int oldfd, int newfd);
// EXPORTED FUNCTION DEFINITIONS
//



void handle_syscall(struct trap_frame * tfr) {    
    tfr->sepc += 4;
    tfr->a0 = syscall(tfr);
}

// INTERNAL FUNCTION DEFINITIONS
//



int64_t syscall(const struct trap_frame * tfr) {
    switch(tfr->a7){
        case SYSCALL_EXIT:
            return sysexit();
            break;
        case SYSCALL_EXEC:
            return sysexec((int)tfr->a0, (int)tfr->a1, (char**)tfr->a2);
            break;
        case SYSCALL_FORK:
            return sysfork((const struct trap_frame *)tfr);
            break;
        case SYSCALL_WAIT:
            return syswait((int)tfr->a0);
            break;
        case SYSCALL_PRINT:
            return sysprint((const char*)tfr->a0);
            break;
        case SYSCALL_USLEEP:
            return sysusleep((unsigned long)tfr->a0);
            break;
        case SYSCALL_DEVOPEN:
            return sysdevopen((int)tfr->a0, (const char *) tfr->a1, (int) tfr->a2);
            break;
        case SYSCALL_FSOPEN:
            return sysfsopen((int)tfr->a0, (const char *)tfr->a1);
            break;
        case SYSCALL_CLOSE:
            return sysclose((int)tfr->a0);
            break;
        case SYSCALL_READ:
            return sysread((int)tfr->a0, (void *)tfr->a1, (size_t)tfr->a2);
            break;
        case SYSCALL_WRITE:
            return syswrite((int)tfr->a0, (const void*)tfr->a1, (size_t)tfr->a2);
            break;
        case SYSCALL_IOCTL:
            return sysioctl((int)tfr->a0, (int)tfr->a1, (void *) tfr->a2);
            break;
        case SYSCALL_PIPE:
            return syspipe((int *)tfr->a0, (int *)tfr->a1);
            break;
        case SYSCALL_FSCREATE:
            return sysfscreate((const char*)tfr->a0);
            break;
        case SYSCALL_FSDELETE:
            return sysfsdelete((const char*)tfr->a0);
            break;
        case SYSCALL_IODUP:
            return sysiodup((int)tfr->a0, (int)tfr->a1);
            break;
        default:
            return -ENOTSUP;
    }
    return -ENOTSUP;
}




int sysexit(void) {
    process_exit();
    return 0;
}




int sysexec(int fd, int argc, char ** argv) {
    struct process * save = current_process();
    if(save->iotab[fd] == NULL){
        return -EBADFD;
    }
    int execution_result = process_exec(save->iotab[fd], argc, argv);
    save->iotab[fd] = NULL;
    return execution_result;
}



int sysfork(const struct trap_frame * tfr) {
    //for checkpoint 3?
    return process_fork(tfr);
}


int syswait(int tid) {
    //for checkpoint 3?
    trace("%s(%d)", __func__, tid);
    if (0 <= tid){
        return thread_join(tid);
    }
    else{
        return -EINVAL;
    }
}



int sysprint(const char * msg) {
    // int result;
    trace("%s(msg=%p)", __func__, msg);
    // result = memory_validate_vstr(msg, PTE_U);
    // if (result != 0){
    //     return result;
    // }
    kprintf("Thread <%s:%d> says: %s\n",thread_name(running_thread()),running_thread(), msg);
    return 0;
}


int sysusleep(unsigned long us) {
    //for checkpoint 3?
    sleep_us(us);
    return 0;
}



int sysdevopen(int fd, const char * name, int instno) {
    if(fd >= 0 && fd <16){
        if(current_process()->iotab[fd] == NULL){
            open_device(name, instno, &current_process()->iotab[fd]);
            return fd;
        }else{
            return -EBADFD;
        }
    }else if(fd == -1){
        for(int i = 0; i<16; i++){
            if(current_process()->iotab[i] == NULL){
                open_device(name, instno, &current_process()->iotab[i]);
                return i;
            }
        }
        return -EMFILE;
    }else{
        return -EBADFD;
    }
}


int sysfsopen(int fd, const char * name) {
    if(fd >= 0 && fd<16){
        if(current_process()->iotab[fd] == NULL){
            fsopen(name, &current_process()->iotab[fd]);
            return fd;
        }else{
            return -EBADFD;
        }
    }else if(fd == -1){
        for(int i = 0; i<16; i++){
            if(current_process()->iotab[i] == NULL){
                fsopen(name, &current_process()->iotab[i]);
                return i;
            }
        }
        return -EMFILE;
    }else{
        return -EBADFD;
    }
}




int sysclose(int fd) {
    if(fd < 0 || fd >= 16 || current_process()->iotab[fd] == NULL){
        return -EBADFD;
    }else{
        ioclose(current_process()->iotab[fd]);
        current_process()->iotab[fd] = NULL;
        return 0;
    }
    return 0;
}



long sysread(int fd, void * buf, size_t bufsz) {
    
    if(fd < 0 || fd >= 16 || current_process()->iotab[fd] == NULL){
        return -EBADFD;
    }else{
        //what does 0 on end of file mean?
        //I think the io functions already take care of validity checks for bufsz
        //do we need to open device or file first?
        return ioread(current_process()->iotab[fd], buf, bufsz);
    }
}



long syswrite(int fd, const void * buf, size_t len) {
    if(fd < 0 || fd >= 16 || current_process()->iotab[fd] == NULL){
        return -EBADFD;
    }else{
        //what does 0 on end of file mean?
        //I think the io functions already take care of validity checks for bufsz
        return iowrite(current_process()->iotab[fd], buf, len);
    }
}


int sysioctl(int fd, int cmd, void * arg) {
    if(fd < 0 || fd >= 16 || current_process()->iotab[fd] == NULL){
        return -EBADFD;
    }else{
        return ioctl(current_process()->iotab[fd], cmd, arg);
    }
    return -ENOTSUP;
}



int syspipe(int *wfdptr, int *rfdptr)
{
    // implement in checkpoint 3
    if (wfdptr == NULL || rfdptr == NULL)
    {
        return -EBADFD;
    }

    struct io *wio;
    struct io *rio;

    if (*wfdptr < 0 && *rfdptr < 0)
    {
        int open1 = -1;
        int open2 = -1;
        for (int i = 0; i < PROCESS_IOMAX; i++)
        {
            if (current_process()->iotab[i] == NULL)
            {
                open1 = i;
                break;
            }
        }
        for (int i = open1 + 1; i < PROCESS_IOMAX; i++)
        {
            if (current_process()->iotab[i] == NULL && i != open1)
            {
                open2 = i;
                break;
            }
        }
        if (open1 < 0 || open2 < 0)
        {
            return -EMFILE;
        }
        create_pipe(&wio, &rio);
        current_process()->iotab[open1] = wio;
        current_process()->iotab[open2] = rio;
        *wfdptr = open1;
        *rfdptr = open2;
        return 0;
    }
    if (*wfdptr >= 0 && *rfdptr >= 0 && *wfdptr != *rfdptr)
    {
        if (current_process()->iotab[*wfdptr] || current_process()->iotab[*rfdptr] || *wfdptr >= PROCESS_IOMAX || *rfdptr >= PROCESS_IOMAX)
        {
            return -EBADFD;
        }
        create_pipe(&wio, &rio);
        current_process()->iotab[*wfdptr] = wio;
        current_process()->iotab[*rfdptr] = rio;
        return 0;
    }

    return -EBADFD;
}



int sysfscreate(const char* name) {
    return fscreate(name);
}




int sysfsdelete(const char* name) {
    return fsdelete(name);
}


int sysiodup(int oldfd, int newfd){
    if(oldfd < 0 || oldfd >= 16 || current_process()->iotab[oldfd] == NULL){
        return -EBADFD;
    }
    struct io* io_old = current_process()->iotab[oldfd];
    if(newfd < 0 || newfd >= 16){
        return -EBADFD;
    }
    struct io* io_new = ioaddref(io_old);
    current_process()->iotab[newfd] = io_new;
    return newfd;
}
