//  timer.c
//
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

//  EXPORTED GLOBAL VARIABLE DEFINITIONS
//  

char timer_initialized = 0;

//  INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm *sleep_list;

//  INTERNAL FUNCTION DECLARATIONS
//

//  EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void)
{
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
    start_interrupter();
}

// void alarm_init(struct alarm *al, const char *name)
// Inputs: struct alarm *al - pointer to an alarm structure
//         const char *name - name for the alarm
// Outputs: None
// Description: initializes an alarm structure by setting its condition variable, initial wake time, and resetting sleep_list
// Side Effects: initializes condition variable and twake field of the alarm
void alarm_init(struct alarm *al, const char *name)
{
    //  FIXME your code goes here
    if (name == NULL)
    {
        name = "alarm";
    }

    // Intialize alarm condition and list
    sleep_list = NULL;
    condition_init(&al->cond, name);
    al->twake = rdtime();
}

// void alarm_sleep(struct alarm *al, unsigned long long tcnt)
// Inputs: struct alarm *al - pointer to an alarm structure
//         unsigned long long tcnt - number of ticks to sleep
// Outputs: None
// Description: puts the current thread to sleep until the alarm's wake time is reached
// Side Effects: adds the alarm to the sleep list, updates mtimecmp, and blocks the calling thread on the alarm's condition variable
void alarm_sleep(struct alarm *al, unsigned long long tcnt)
{
    unsigned long long now;
    struct alarm *prev;
    int pie;

    now = rdtime();

    //  If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;

    //  If the wake-up time has already passed, return

    if (al->twake < now)
        return;

    //  FIXME your code goes here

    pie = disable_interrupts();
    struct alarm *cur = sleep_list;
    prev = NULL;

    // Loop to find where to insert alarm
    while (cur && al->twake >= cur->twake)
    {
        prev = cur;
        cur = cur->next;
    }

    // Insert alarm
    al->next = cur;
    if (prev)
    {
        prev->next = al;
    }
    else
    {
        sleep_list = al;
    }

    // set stcmp
    if (sleep_list)
    {
        set_stcmp(sleep_list->twake);
    }
    else
    {
        set_stcmp(UINT64_MAX);
    }

    // enable timer interrupt and condition wait
    csrs_sie(RISCV_SIE_STIE);
    condition_wait(&al->cond);
    restore_interrupts(pie);
}

//  Resets the alarm so that the next sleep increment is relative to the time
//  alarm_reset is called.

void alarm_reset(struct alarm *al)
{
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm *al, unsigned int sec)
{
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm *al, unsigned long ms)
{
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm *al, unsigned long us)
{
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec)
{
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms)
{
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us)
{
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

//  handle_timer_interrupt() is dispatched from intr_handler in intr.c

// void handle_timer_interrupt(void)
// Inputs: None
// Outputs: None
// Description: handles timer interrupts by waking up all alarms whose wake time has passed
// Side Effects: removes expired alarms from the sleep list, broadcasts condition variables, and updates mtimecmp or disables timer interrupt
void handle_timer_interrupt(void)
{
    // struct alarm *head = sleep_list;
    // struct alarm *next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    //  FIXME your code goes here
    int pie = disable_interrupts();

    // Remove alarms with twake before now, and broadcasts their condition
    while (sleep_list && sleep_list->twake <= now)
    {
        struct alarm *prev = sleep_list;
        sleep_list = prev->next;
        condition_broadcast(&prev->cond);
    }

    // Set stcmp or disable timer interrupts if nothing in sleep_list
    if (sleep_list)
    {
        set_stcmp(sleep_list->twake);
    }
    else
    {
        csrc_sie(RISCV_SIE_STIE);
    }
    restore_interrupts(pie);
}


void interrupter(void){
    struct alarm al;
    alarm_init(&al, "interrupter");
    for(;;){
        alarm_sleep_ms(&al, 10);
    }
}

void start_interrupter(void){
    thread_spawn("interrupter", &interrupter);
}