//  thread.c - Threads
//
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#ifdef THREAD_TRACE
#define TRACE
#endif

#ifdef THREAD_DEBUG
#define DEBUG
#endif

#include "thread.h"

#include <stddef.h>
#include <stdint.h>

#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "memory.h"
#include "error.h"
#include "process.h"

#include <stdarg.h>

//  COMPILE-TIME PARAMETERS
//

//  NTHR is the maximum number of threads

#ifndef NTHR
#define NTHR 16
#endif

#ifndef STACK_SIZE
#define STACK_SIZE 4000
#endif

//  EXPORTED GLOBAL VARIABLES
//

char thrmgr_initialized = 0;

//  INTERNAL TYPE DEFINITIONS
//

enum thread_state
{
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_EXITED
};

struct thread_context
{
    uint64_t s[12];
    void *ra;
    void *sp;
};

// struct thread_stack_anchor
// {
//     struct thread *ktp;
//     void *kgp;
// };

struct thread
{
    struct thread_context ctx; //  must be first member (thrasm.s)
    int id;                    //  index into thrtab[]
    enum thread_state state;
    const char *name;
    struct thread_stack_anchor *stack_anchor;
    void *stack_lowest;
    struct thread *parent;
    struct thread *list_next;
    struct condition *wait_cond;
    struct condition child_exit;
    struct lock *lock_list;

    struct process * proc;
};


//  INTERNAL MACRO DEFINITIONS
//   

//  Pointer to running thread, which is kept in the tp (x4) register.

#define TP ((struct thread *)__builtin_thread_pointer())

//  Macro for changing thread state. If compiled for debugging (DEBUG is
//  defined), prints function that changed thread state.

#define set_thread_state(t, s)                                               \
    do                                                                       \
    {                                                                        \
        debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
              (t)->name, (t)->id,                                            \
              thread_state_name((t)->state),                                 \
              thread_state_name(s),                                          \
              TP->name, TP->id,                                              \
              __func__);                                                     \
        (t)->state = (s);                                                    \
    } while (0)

//  INTERNAL FUNCTION DECLARATIONS
//

//  Initializes the main and idle threads. called from threads_init().

static void init_main_thread(void);
static void init_idle_thread(void);

//  Sets the RISC-V thread pointer to point to a thread.

static void set_running_thread(struct thread *thr);

//  Returns a string representing the state name. Used by debug and trace
//  statements, so marked unused to avoid compiler warnings.

static const char *thread_state_name(enum thread_state state)
    __attribute__((unused));

//  void thread_reclaim(int tid)
//
//  Reclaims a thread's slot in thrtab and makes its parent the parent of its
//  children. Frees the struct thread of the thread.

static void thread_reclaim(int tid);

//  struct thread * create_thread(const char * name)
//
//  Creates and initializes a new thread structure. The new thread is not added
//  to any list and does not have a valid context (_thread_switch cannot be
//  called to switch to the new thread).

static struct thread *create_thread(const char *name);

//  void running_thread_suspend(void)
//  Suspends the currently running thread and resumes the next thread on the
//  ready-to-run list using _thread_swtch (in threasm.s). Must be called with
//  interrupts enabled. Returns when the current thread is next scheduled for
//  execution. If the current thread is TP, it is marked READY and placed
//  on the ready-to-run list. Note that running_thread_suspend will only return if the
//  current thread becomes READY.

static void running_thread_suspend(void);

//  The following functions manipulate a thread list (struct thread_list). Note
//  that threads form a linked list via the list_next member of each thread
//  structure. Thread lists are used for the ready-to-run list (ready_list) and
//  for the list of waiting threads of each condition variable. These functions
//  are not interrupt-safe! The caller must disable interrupts before calling any
//  thread list function that may modify a list that is used in an ISR.

static void tlclear(struct thread_list *list);
static int tlempty(const struct thread_list *list);
static void tlinsert(struct thread_list *list, struct thread *thr);
static struct thread *tlremove(struct thread_list *list);
// static void tlappend(struct thread_list *l0, struct thread_list *l1);

static void idle_thread_func(void);

//  IMPORTED FUNCTION DECLARATIONS
//  defined in thrasm.s
//

extern struct thread *_thread_swtch(struct thread *thr);

extern void _thread_startup(void);

//  INTERNAL GLOBAL VARIABLES
//

#define MAIN_TID 0
#define IDLE_TID (NTHR - 1)

static struct thread main_thread;
static struct thread idle_thread;

extern char _main_stack_lowest[]; //  from start.s
extern char _main_stack_anchor[]; //  from start.s

static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_RUNNING,
    .stack_anchor = (void *)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"};

extern char _idle_stack_lowest[]; //  from thrasm.s
extern char _idle_stack_anchor[]; //  from thrasm.s

static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void *)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
    //  FIXME your code goes here
    .ctx.s[8] = (uint64_t)idle_thread_func // pointer to idle thread entry function

};

static struct thread *thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread};

static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread};

//  EXPORTED FUNCTION DEFINITIONS
//

int running_thread(void)
{
    return TP->id;
}

void thrmgr_init(void)
{
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}

// int thread_spawn(const char *name, void (*entry)(void), ...)
// Inputs: const char *name - name of the new thread
//         void (*entry)(void) - entry function for the new thread
//         ... - additional arguments for the entry function
// Outputs: int - thread id of the spawned thread
// Description: creates a new thread, initializes its context with entry function and arguments, marks it READY, and adds it to the ready list
// Side Effects: allocates a new thread and its stack, updates ready list and thrtab
int thread_spawn(
    const char *name,
    void (*entry)(void),
    ...)
{
    struct thread *child;
    va_list ap;
    int pie;
    int i;

    child = create_thread(name);

    if (child == NULL)
        return -EMTHR;

    set_thread_state(child, THREAD_READY);

    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);

    //  FIXME your code goes here
    //  filling in entry function arguments is given below, the rest is up to you

    // Set up child context
    child->ctx.sp = child->stack_anchor;
    child->ctx.ra = &_thread_startup;

    // fill arguments
    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.s[i] = va_arg(ap, uint64_t);
    va_end(ap);

    // set pointer to entry function
    child->ctx.s[8] = (uint64_t)entry;

    return child->id;
}

// void thread_exit(void)
// Inputs: None
// Outputs: None
// Description: exits the current thread
// Side Effects: free the current thread's stack if the thread is not main
void thread_exit(void)
{
    //  FIXME your code goes here
    if (running_thread() == MAIN_TID)
    {
        // halt main thread
        halt_success();
    }
    else
    {
        // set thread to exited and broadcast to parent that child has exited
        TP->state = THREAD_EXITED;
        condition_broadcast(&TP->parent->child_exit);

        struct lock* curr = TP->lock_list;
        while(curr){
            struct lock *next = curr->next;
            lock_release(curr);
            curr = next;
        }

        // suspend thread
        running_thread_suspend();
        halt_failure();
    }
}

void thread_yield(void)
{
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}

// int thread_join(int tid)
// Inputs: int tid - thread id of the child to join
// Outputs: int - thread id of the joined child
// Description: waits for the specified child to exit and reclaims its resources
// Side Effects: reclaims thread resources and condition waits
int thread_join(int tid)
{
    //  FIXME your code goes here
    int pie = disable_interrupts();
    if (tid != 0)
    {
        // check for valid thread child
        if (thrtab[tid] == NULL || thrtab[tid]->parent->id != running_thread())
        {
            restore_interrupts(pie);
            return -EINVAL;
        }
        if (thrtab[tid]->state == THREAD_EXITED)
        {
            restore_interrupts(pie);
            return tid;
        }

        // condition wait until child exit
        while (thrtab[tid]->state != THREAD_EXITED)
        {
            condition_wait(&TP->child_exit);
        }

        // reclaim thread resources
        thread_reclaim(tid);
        restore_interrupts(pie);
        return tid;
    }
    else
    {
        // search for first valid thread child
        for (int i = 1; i < NTHR; i++)
        {
            if (thrtab[i] != NULL && thrtab[i]->parent == TP)
            {
                // condition wait until child exit
                while (thrtab[i]->state != THREAD_EXITED)
                {
                    condition_wait(&TP->child_exit);
                }

                // reclaim thread resources
                thread_reclaim(i);
                restore_interrupts(pie);
                return i;
            }
        }
        restore_interrupts(pie);
        return -EINVAL;
    }
}

const char *thread_name(int tid)
{
    assert(0 <= tid && tid < NTHR);
    assert(thrtab[tid] != NULL);
    return thrtab[tid]->name;
}

const char *running_thread_name(void)
{
    return TP->name;
}

void condition_init(struct condition *cond, const char *name)
{
    tlclear(&cond->wait_list);
    cond->name = name;
}

void condition_wait(struct condition *cond)
{
    int pie;

    trace("%s(cond=<%s>) in <%s:%d>", __func__,
          cond->name, TP->name, TP->id);
    assert(TP->state == THREAD_RUNNING);

    //  Insert current thread into condition wait list

    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;

    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);

    running_thread_suspend();
}

// void condition_broadcast(struct condition *cond)
// Inputs: struct condition *cond - pointer to a condition variable
// Outputs: None
// Description: wakes up all threads waiting on the condition variable by moving them to the ready list
// Side Effects: clears the condition's wait list and updates thread states to ready
void condition_broadcast(struct condition *cond)
{
    //  FIXME your code goes here

    // loop through wait list
    int pie = disable_interrupts();
    while (!tlempty(&cond->wait_list))
    {
        // remove threads from waitlist and set state to ready
        struct thread *curr = tlremove(&cond->wait_list);
        curr->state = THREAD_READY;
        // insert thread to ready list
        tlinsert(&ready_list, curr);
    }

    restore_interrupts(pie);
}

//  INTERNAL FUNCTION DEFINITIONS
//

void init_main_thread(void)
{
    //  Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
    main_thread.lock_list = NULL;
}

void init_idle_thread(void)
{
    //  Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}

static void set_running_thread(struct thread *thr)
{
    asm inline("mv tp, %0" ::"r"(thr) : "tp");
}

const char *thread_state_name(enum thread_state state)
{
    static const char *const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_RUNNING] = "RUNNING",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"};

    if (0 <= (int)state && (int)state < sizeof(names) / sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};

void thread_reclaim(int tid)
{
    struct thread *const thr = thrtab[tid];
    int ctid;

    assert(0 < tid && tid < NTHR && thr != NULL);
    assert(thr->state == THREAD_EXITED);

    //  Make our parent thread the parent of our child threads. We need to scan
    //  all threads to find our children. We could keep a list of all of a
    //  thread's children to make this operation more efficient.

    for (ctid = 1; ctid < NTHR; ctid++)
    {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }

    thrtab[tid] = NULL;
    kfree(thr);
}

struct thread *create_thread(const char *name)
{
    struct thread_stack_anchor *anchor;
    void *stack_page;
    struct thread *thr;
    int tid;

    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);

    //  Find a free thread slot.

    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;

    if (tid == NTHR)
        return NULL;

    //  Allocate a struct thread and a stack

    thr = kcalloc(1, sizeof(struct thread));

    //stack_page = kmalloc(STACK_SIZE);
    stack_page = alloc_phys_page(); //CP3 CHANGE

    anchor = stack_page + STACK_SIZE;
    anchor -= 1; //  anchor is at base of stack
    thr->stack_lowest = stack_page;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;

    thrtab[tid] = thr;

    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    thr->lock_list = NULL;
    return thr;
}

// static void running_thread_suspend(void)
// Inputs: None
// Outputs: None
// Description: suspends the currently running thread, places it in the ready list if running, and switches to the next thread in the ready list
// Side Effects: updates thread states, may free the stack of an exited thread, and performs a context switch
void running_thread_suspend(void)
{
    //  FIXME your code goes here
    int pie = disable_interrupts();

    // If current thread is running change it to ready and add it to ready list
    if (TP->state == THREAD_RUNNING)
    {
        TP->state = THREAD_READY;
        tlinsert(&ready_list, TP);
    }

    // Remove first thread in ready list
    struct thread *run = tlremove(&ready_list);
    if (run == NULL)
    {
        run = &idle_thread;
    }

    // Set the thread to running and call thread switch
    run->state = THREAD_RUNNING;

    if (run->proc != NULL) {
        switch_mspace(run->proc->mtag); //switch to child memspace
    } //else if (run->id == IDLE_TID) {
    //    switch_mspace(kernel_mspace_tag());
    //}

    _thread_swtch(run);

    // Free memory for exited thread
    if (TP->state == THREAD_EXITED)
    {
        //kfree(TP->stack_lowest);
        free_phys_page(TP->stack_lowest); //CP3 CHANGE
    }
    restore_interrupts(pie);
}

void tlclear(struct thread_list *list)
{
    list->head = NULL;
    list->tail = NULL;
}

int tlempty(const struct thread_list *list)
{
    return (list->head == NULL);
}

void tlinsert(struct thread_list *list, struct thread *thr)
{
    thr->list_next = NULL;

    if (thr == NULL)
        return;

    if (list->tail != NULL)
    {
        assert(list->head != NULL);
        list->tail->list_next = thr;
    }
    else
    {
        assert(list->head == NULL);
        list->head = thr;
    }

    list->tail = thr;
}

struct thread *tlremove(struct thread_list *list)
{
    struct thread *thr;

    thr = list->head;

    if (thr == NULL)
        return NULL;

    list->head = thr->list_next;

    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;

    thr->list_next = NULL;
    return thr;
}

//  Appends elements of l1 to the end of l0 and clears l1.

// void tlappend(struct thread_list *l0, struct thread_list *l1)
// {
//     if (l0->head != NULL)
//     {
//         assert(l0->tail != NULL);

//         if (l1->head != NULL)
//         {
//             assert(l1->tail != NULL);
//             l0->tail->list_next = l1->head;
//             l0->tail = l1->tail;
//         }
//     }
//     else
//     {
//         assert(l0->tail == NULL);
//         l0->head = l1->head;
//         l0->tail = l1->tail;
//     }

//     l1->head = NULL;
//     l1->tail = NULL;
// }

void idle_thread_func(void)
{
    //  The idle thread sleeps using wfi if the ready list is empty. Note that we
    //  need to disable interrupts before checking if the thread list is empty to
    //  avoid a race condition where an ISR marks a thread ready to run between
    //  the call to tlempty() and the wfi instruction.

    for (;;)
    {
        //  If there are runnable threads, yield to them.

        while (!tlempty(&ready_list))
            thread_yield();

        //  No runnable threads. Sleep using the wfi instruction. Note that we
        //  need to disable interrupts and check the runnable thread list one
        //  more time (make sure it is empty) to avoid a race condition where an
        //  ISR marks a thread ready before we call the wfi instruction.

        disable_interrupts();
        if (tlempty(&ready_list))
            asm("wfi");
        enable_interrupts();
    }
}

void lock_init(struct lock * lock){
    condition_init(&lock->released, "lckcond");
    lock->count = 0;
    lock->owner = NULL;
    lock->next = NULL;
}

void lock_release(struct lock * lock){
    if(TP != lock->owner){
        return;
    }
    int pie = disable_interrupts();
    lock->count -= 1;
    if(lock->count > 0){
        restore_interrupts(pie);
        return;
    }
    struct lock * curr = TP->lock_list;
    struct lock * prev = NULL;
    while((lock != curr) && curr){
        prev = curr;
        curr = curr->next;
    }
    if(curr){
        if(prev){
            prev->next = curr->next;
        }else{
            TP->lock_list = curr->next;
        }
    }
    lock->owner = NULL;
    lock->next  = NULL;

    
    condition_broadcast(&lock->released);
    restore_interrupts(pie);
    
}

void lock_acquire(struct lock * lock){
    if(lock->owner == TP){
        lock->count += 1;
        return;
    }
    int pie = disable_interrupts();
    while(lock->owner != NULL){
        condition_wait(&lock->released);
        pie = disable_interrupts();
    }
    lock->owner = TP;
    lock->next = TP->lock_list;
    TP->lock_list = lock;
    lock->count = 1;
    restore_interrupts(pie);
}

//function to get thread stack anchor pointer
struct thread_stack_anchor * get_running_thread_stack_anchor(void) {
    return TP->stack_anchor;
}


//process stuff


struct process * thread_process(int tid) {
    return thrtab[tid]->proc;
}

//returns pointer to process struct of currently running thread's process
struct process * running_thread_process(void) {
    return TP->proc;
}


void thread_set_process(int tid, struct process * proc) {
    thrtab[tid]->proc = proc;
}