//  rtc.c - Goldfish RTC driver
//  
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

//  INTERNAL TYPE DEFINITIONS
//  

//RTC structs
struct rtc_regs
{
    //  TODO
    uint32_t low_time;
    uint32_t high_time;
};

struct rtc_device
{
    //  TODO
    volatile struct rtc_regs *regs;
    struct io io;
    int instno;
};

//  INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct io **ioptr, void *aux);
static void rtc_close(struct io *io);
static int rtc_cntl(struct io *io, int cmd, void *arg);
static long rtc_read(struct io *io, void *buf, long bufsz);

static uint64_t read_real_time(volatile struct rtc_regs *regs);

//  EXPORTED FUNCTION DEFINITIONS
//  

// void rtc_attach(void *mmio_base)
// Inputs: void *mmio_base - MMIO address for rtc
// Outputs: None
// Description: Sets up the rtc device and it's io interface
// Side Effects: Allocates memory and calls other functions
void rtc_attach(void *mmio_base)
{
    //  TODO
    static const struct iointf rtc_iointf = { //Initialize the rtc io interface
        .close = &rtc_close,
        .cntl = &rtc_cntl,
        .read = &rtc_read};

    struct rtc_device *rtc = kcalloc(1, sizeof(struct rtc_device)); //Allocate memory for rtc
    rtc->regs = mmio_base; //assign the mmio
    //Initialize io interface and register device
    ioinit0(&rtc->io, &rtc_iointf);
    rtc->instno = register_device("rtc", rtc_open, rtc);
}

// int rtc_open(struct io **ioptr, void *aux)
// Inputs: struct io **ioptr - ioptr for rtc device
//         void *aux - Aux pointer for rtc
// Outputs: 0
// Description: Opens the rtc device to be used
// Side Effects: Adds a refernce to the rtc device
int rtc_open(struct io **ioptr, void *aux)
{
    //  TODO
    struct rtc_device *rtc_dev = (struct rtc_device *)aux; //Set rtc device pointer
    if (ioptr == NULL || rtc_dev == NULL) //Null Check
    {
        return -EINVAL;
    }

    //Set ioptr and add reference
    *ioptr = &rtc_dev->io;
    rtc_dev->io.refcnt += 1;
    return 0;
}

// void rtc_close(struct io *io)
// Inputs: struct io *io - pointer to the rtc device interface
// Outputs: None
// Description: Closes the rtc device and frees its allocated resources
// Side Effects: Deallocates memory associated with the rtc device
void rtc_close(struct io *io)
{
    //  TODO
    //Null check
    if (io == NULL)
    {
        panic("Bad arguments in to rtc_close");
    }
    //Assign rtc device
    // struct rtc_device *rtc_dev = (void *)io - offsetof(struct rtc_device, io);
    assert(io->refcnt == 0);
    //Free memory
    // kfree(rtc_dev);
}

// int rtc_cntl(struct io *io, int cmd, void *arg)
// Inputs: struct io *io - pointer to the rtc io
//         int cmd - Control command
//         void *arg - Argument
// Outputs: Returns RTC_BLKSZ for IOCTL_GETBLKSZ command or -ENOTSUP for unsupported commands
// Description: Allows rtc to take commands
// Side Effects: None
int rtc_cntl(struct io *io, int cmd, void *arg)
{
    //  TODO
    if (io == NULL) //Null check
    {
        panic("Bad arguments in to rtc_cntl");
    }
    if (cmd == IOCTL_GETBLKSZ) //Command
    {
        return RTC_BLKSZ;
    }
    else
    {
        return -ENOTSUP;
    }
    return -ENOTSUP;
}

// long rtc_read(struct io *io, void *buf, long bufsz)
// Inputs: struct io *io - Pointer to the rtc device interface
//         void *buf - Buffer to store read data
//         long bufsz - Size of buffer
// Outputs: Number of bytes read or error code
// Description: Reads the rtc device time into the buffer
// Side Effects: None
long rtc_read(struct io *io, void *buf, long bufsz)
{
    //  TODO
    struct rtc_device *rtc = (void *)io - offsetof(struct rtc_device, io); //Assign rtc device

    trace("%s(bufsz=%ld)", __func__, bufsz);

    if (bufsz == 0) //invalid bufsz check
    {
        return 0;
    }

    if (bufsz < sizeof(uint64_t)) //buffer should be full length of time
    {
        return -EINVAL;
    }

    uint64_t time = read_real_time(rtc->regs); //read time
    memcpy(buf, &time, sizeof(uint64_t)); //copy time into buffer
    return sizeof(uint64_t); 
}

// uint64_t read_real_time(volatile struct rtc_regs *regs)
// Inputs: volatile struct rtc_regs *regs - Pointer to the rtc regs
// Outputs: Current time as a 64-bit integer
// Description: Reads low and high time registers and combines them to form the full time value
// Side Effects: None
uint64_t read_real_time(volatile struct rtc_regs *regs)
{
    //  TODO
    uint32_t low = regs->low_time; //read low time
    uint32_t high = regs->high_time; //read high time
    return ((uint64_t)high << 32) + low; //combine times
}