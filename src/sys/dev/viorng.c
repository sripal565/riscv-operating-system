//  viorng.c - VirtIO rng device
//  
//  Copyright (c) 2024-2025 University of Illinois
//  SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "assert.h"
#include "conf.h"
#include "intr.h"
#include "console.h"
#include "thread.h"

//  INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

//  INTERNAL TYPE DEFINITIONS
//

struct viorng_device
{
    volatile struct virtio_mmio_regs *regs;
    int irqno;
    int instno;

    struct io io;

    struct
    {
        uint16_t last_used_idx;

        union
        {
            struct virtq_avail avail; // driver to device
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union
        {
            volatile struct virtq_used used; // device to driver
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        //  The first descriptor is a regular descriptor and is the one used in
        //  the avail and used rings.

        struct virtq_desc desc[1]; // describes buffers
    } vq;

    //  bufcnt is the number of bytes left in buffer. The usable bytes are
    //  between buf+0 and buf+bufcnt. (We read from the end of the buffer.)

    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];

    struct condition viorng_cond;
};

//  INTERNAL FUNCTION DECLARATIONS
//

static int viorng_open(struct io **ioptr, void *aux);
static void viorng_close(struct io *io);
static long viorng_read(struct io *io, void *buf, long bufsz);
static void viorng_isr(int irqno, void *aux);

//  EXPORTED FUNCTION DEFINITIONS
//

//  Attaches a VirtIO rng device. Declared and called directly from virtio.c.

// void viorng_attach(volatile struct virtio_mmio_regs *regs, int irqno)
// Inputs: volatile struct virtio_mmio_regs *regs - pointer to the virtio mmio registers
//         int irqno - interrupt number for viorng
// Outputs: None
// Description: attaches and initializes viorng device, its features, sets up the virtqueue, initializes device registers and registers the device
// Side Effects: allocates memory, initializes device data structures, registers device, and enables driver status
void viorng_attach(volatile struct virtio_mmio_regs *regs, int irqno)
{
    //            FIXME add additional declarations here if needed

    // io interface for viorng
    static const struct iointf viorng_iointf = {
        .close = &viorng_close,
        .read = &viorng_read};

    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;

    assert(regs->device_id == VIRTIO_ID_RNG);

    //  Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;

    //  fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
                                       enabled_features, wanted_features, needed_features);

    if (result != 0)
    {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    //            FIXME Finish viorng initialization here! 
    // Signal features ok
    regs->status |= VIRTIO_STAT_FEATURES_OK;

    // allocate memory for viorng_device
    struct viorng_device *viorng = kcalloc(1, sizeof(struct viorng_device));

    // assigns regs and irqno
    viorng->regs = regs;
    viorng->irqno = irqno;

    // initialize device
    ioinit0(&viorng->io, &viorng_iointf);

    // set up viorng descriptor table
    viorng->bufcnt = 0;
    viorng->vq.desc[0].addr = (uint64_t)&viorng->buf;
    viorng->vq.desc[0].len = VIORNG_BUFSZ;
    viorng->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;
    viorng->vq.desc[0].next = 0;

    // assign queue_num
    regs->queue_num = 1;

    // attach and register device
    virtio_attach_virtq(regs, 0, 1, (uint64_t)&viorng->vq.desc, (uint64_t)&viorng->vq.used, (uint64_t)&viorng->vq.avail);
    viorng->instno = register_device(VIORNG_NAME, viorng_open, viorng);

    //  fence o,oi
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    //            fence o,oi
    __sync_synchronize();
}

// static int viorng_open(struct io **ioptr, void *aux)
// Inputs: struct io **ioptr - pointer to store viorng io interface
//         void *aux - pointer to viorng device structure
// Outputs: int - 0 on success or error code
// Description: opens the viorng device for use
// Side Effects: initializes available and used rings, enables virtqueue and interrupts, and initializes condition variable
int viorng_open(struct io **ioptr, void *aux)
{
    //            FIXME your code here

    struct viorng_device *const viorng = aux;

    if (iorefcnt(&viorng->io) != 0)
        return -EBUSY;

    // initialize available ring
    viorng->vq.avail.flags = 0;
    viorng->vq.avail.idx = 0;
    viorng->vq.avail.ring[0] = 0;

    // initialize used ring
    viorng->vq.used.flags = 0;
    viorng->vq.used.idx = 0;

    // enable device regs and interrupt source
    virtio_enable_virtq(viorng->regs, 0);
    enable_intr_source(viorng->irqno, VIORNG_INTR_PRIO, viorng_isr, viorng);

    // initialize viorng condition variable
    condition_init(&viorng->viorng_cond, "vioring_cond");

    // set up io
    *ioptr = &viorng->io;
    ioaddref(&viorng->io);
    return 0;
}

// static void viorng_close(struct io *io)
// Inputs: struct io *io - pointer to viorng device interface
// Outputs: None
// Description: closes the viorng device
// Side Effects: resets device status, resets virtqueue, disables interrupt source
void viorng_close(struct io *io)
{
    //            FIXME your code here
    struct viorng_device *const viorng =
        (void *)io - offsetof(struct viorng_device, io);
    assert(iorefcnt(io) == 0);

    // Signal status
    viorng->regs->status |= VIRTIO_STAT_DEVICE_NEEDS_RESET;

    // Reset and disable viorng device
    virtio_reset_virtq(viorng->regs, 0);
    disable_intr_source(viorng->irqno);
}

// static long viorng_read(struct io *io, void *buf, long bufsz)
// Inputs: struct io *io - pointer to viorng device interface
//         void *buf - buffer to store random bytes
//         long bufsz - size of buffer
// Outputs: long - number of bytes read or error code
// Description: reads random bytes from the viorng device into the buffer
// Side Effects: may wait on condition variable until buffer is filled with randomness
long viorng_read(struct io *io, void *buf, long bufsz)
{
    //            FIXME your code here
    struct viorng_device *const viorng =
        (void *)io - offsetof(struct viorng_device, io);

    if (bufsz <= 0)
    {
        return 0;
    }

    // Short read if bufsz too large
    if (bufsz > VIORNG_BUFSZ)
    {
        bufsz = VIORNG_BUFSZ;
    }

    // Increment available index and notify
    viorng->vq.avail.idx++;
    virtio_notify_avail(viorng->regs, 0);

    // condition wait until bufcnt set
    long pie = disable_interrupts();
    while (viorng->bufcnt == 0)
    {
        condition_wait(&viorng->viorng_cond);
    }
    restore_interrupts(pie);

    // read viorng buffer
    char *rbuf = (char *)buf;
    int read = 0;
    while (read < bufsz && viorng->bufcnt > 0)
    {
        rbuf[read] = viorng->buf[read];
        read++;
        viorng->bufcnt--;
    }

    return read;
}

// static void viorng_isr(int irqno, void *aux)
// Inputs: int irqno - interrupt source number
//         void *aux - pointer to viorng device structure
// Outputs: None
// Description: services interrupts from the viorng device, updates buffer count and signals waiting threads
// Side Effects: broadcasts condition variable when random data becomes available
void viorng_isr(int irqno, void *aux)
{
    //            FIXME your code here
    struct viorng_device *const viorng = aux;

    // acknowledge interrupt
    viorng->regs->interrupt_ack = viorng->regs->interrupt_status;

    // Set bufcnt and broadcast condition if necessary
    while (viorng->vq.used.idx != viorng->vq.last_used_idx)
    {
        viorng->bufcnt = VIORNG_BUFSZ;

        viorng->vq.last_used_idx++;
        condition_broadcast(&viorng->viorng_cond);
    }
}