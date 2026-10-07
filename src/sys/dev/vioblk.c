// vioblk.c - VirtIO serial port (console)
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "assert.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "thread.h"
#include "error.h"
#include "string.h"
#include "assert.h"
#include "ioimpl.h"
#include "io.h"
#include "conf.h"

#include <limits.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1

//  INTERNAL TYPE DEFINITIONS
//

struct vioblk_device
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

    uint32_t blksz;

    struct virtq_desc desc_table[3];

    struct condition vioblk_cond;
};

struct virtio_blk_req
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t data[512];
    uint8_t status;
};

// INTERNAL FUNCTION DECLARATIONS
//

static int vioblk_open(struct io **ioptr, void *aux);
static void vioblk_close(struct io *io);

static long vioblk_readat(
    struct io *io,
    unsigned long long pos,
    void *buf,
    long bufsz);

static long vioblk_writeat(
    struct io *io,
    unsigned long long pos,
    const void *buf,
    long len);

static int vioblk_cntl(
    struct io *io, int cmd, void *arg);

static void vioblk_isr(int srcno, void *aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.

void vioblk_attach(volatile struct virtio_mmio_regs *regs, int irqno)
{

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.
    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    uint32_t blksz;

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs,
                                       enabled_features, wanted_features, needed_features);

    if (result != 0)
    {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIX ME

    // io interface for blk
    static const struct iointf blk_iointf = {
        .close = &vioblk_close,
        .readat = &vioblk_readat,
        .writeat = &vioblk_writeat,
        .cntl = &vioblk_cntl};

    regs->status |= VIRTIO_STAT_FEATURES_OK; // signal features ok
    // allocate memory for vioblk_device
    struct vioblk_device *blk = kcalloc(1, sizeof(struct vioblk_device));
    // assign regs and irqno
    blk->regs = regs;
    blk->irqno = irqno;
    blk->blksz = blksz;
    // initialize device io
    ioinit0(&blk->io, &blk_iointf);
    // set up blk descriptor table
    blk->vq.desc[0].addr = 0;
    blk->vq.desc[0].len = 0;
    blk->vq.desc[0].flags = 0;
    blk->vq.desc[0].next = 0;

    // assign queue_num to 1 because only 1 at a time
    regs->queue_num = 1;
    // attach the virtq_avail and virtq_used structs
    virtio_attach_virtq(regs, 0, 1, (uint64_t)&blk->vq.desc, (uint64_t)&blk->vq.used, (uint64_t)&blk->vq.avail);
    blk->instno = register_device(VIOBLK_NAME, vioblk_open, blk);
    //  fence o,oi
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    //            fence o,oi
    __sync_synchronize();
}

int vioblk_open(struct io **ioptr, void *aux)
{
    struct vioblk_device *const blk = aux;
    if (iorefcnt(&blk->io) != 0)
        return -EBUSY;
    // initialize avail ring
    blk->vq.avail.flags = 0;
    blk->vq.avail.idx = 0;
    blk->vq.avail.ring[0] = 0;

    // initialize used ring
    blk->vq.used.flags = 0;
    blk->vq.used.idx = 0;

    // enable device regs and interrupt source
    virtio_enable_virtq(blk->regs, 0);
    enable_intr_source(blk->irqno, VIORNG_INTR_PRIO, vioblk_isr, blk);

    // initialize vioblk condition variable
    condition_init(&blk->vioblk_cond, "vioblk_cond");

    // set up io
    *ioptr = &blk->io;
    ioaddref(&blk->io);
    return 0;
}

void vioblk_close(struct io *io)
{
    struct vioblk_device *const blk = (void *)io - offsetof(struct vioblk_device, io);
    assert(iorefcnt(io) == 0);
    // Signal status
    blk->regs->status |= VIRTIO_STAT_DEVICE_NEEDS_RESET;
    // Reset and disable vioblk device
    virtio_reset_virtq(blk->regs, 0);
    disable_intr_source(blk->irqno);
}

long vioblk_readat(struct io *io, unsigned long long pos, void *buf, long bufsz)
{
    struct vioblk_device *const blk = (void *)io - offsetof(struct vioblk_device, io);


    if (pos + bufsz > blk->regs->config.blk.capacity * blk->blksz) {
        return -EINVAL; //return invalid if request would be too big
    }

    if (bufsz % blk->blksz != 0 || pos % blk->blksz != 0) {
        return -EINVAL; //bufsz and pos must be multiple of 512

    }
    uint8_t *buf_ptr = (uint8_t *)buf; // pointer to buffer
    // calculating number of sectors to read and starting position
    uint64_t num_sectors = bufsz / blk->blksz;
    uint64_t start_sector = pos / blk->blksz;
    // allocate 1 request struct
    struct virtio_blk_req *req = kcalloc(1, sizeof(struct virtio_blk_req));

    // set up indirect descriptor that points to descriptor table
    blk->vq.desc[0].addr = (uint64_t)blk->desc_table;
    blk->vq.desc[0].len = 3 * sizeof(struct virtq_desc);
    blk->vq.desc[0].flags = VIRTQ_DESC_F_INDIRECT;

    // descriptor table 3 descriptors
    // first descriptor header must read type and sector
    blk->desc_table[0].addr = (uint64_t)req; // length to read type, reserved, and sector values
    blk->desc_table[0].len = sizeof(req->type) + sizeof(req->reserved) + sizeof(req->sector);
    blk->desc_table[0].flags = VIRTQ_DESC_F_NEXT; // next flag and no write flag so that it reads
    blk->desc_table[0].next = 1;                  // points to next desc

    // second descriptor writes 512 bytes to request data buffer
    blk->desc_table[1].addr = (uint64_t)(req->data); // points to data buffer to write to
    blk->desc_table[1].len = blk->blksz;             // 512 bytes
    blk->desc_table[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
    blk->desc_table[1].next = 2;

    // last descriptor writes status to status value
    blk->desc_table[2].addr = (uint64_t)&(req->status); // points to status value
    blk->desc_table[2].len = sizeof(req->status);
    blk->desc_table[2].flags = VIRTQ_DESC_F_WRITE; // last desc so no next flag

    // loop through each sector
    for (uint64_t sector_count = 0; sector_count < num_sectors; sector_count++)
    {
        uint64_t curr_sector = start_sector + sector_count;

        req->type = VIRTIO_BLK_T_IN; // to read from block device
        req->sector = curr_sector;

        long pie = disable_interrupts();
        blk->vq.avail.ring[0] = 0;
        blk->vq.avail.idx++; // lets device know that new descriptor has been added
        virtio_notify_avail(blk->regs, 0);
        while (blk->vq.used.idx != blk->vq.avail.idx)
        { // used and avail is same meaning device didnt update used idx
            // kprintf("test1");
            condition_wait(&blk->vioblk_cond);
            restore_interrupts(pie);
        }
        blk->vq.last_used_idx++; // have last used idx catch up meaning descriptor has been read by device

        uint8_t *buf_offset = buf_ptr + sector_count * blk->blksz; // address of specific part of buffer to write to
        memcpy(buf_offset, req->data, blk->blksz);
    }
    kfree(req);
    return bufsz;
}

long vioblk_writeat(struct io *io, unsigned long long pos, const void *buf, long len)
{
    struct vioblk_device *const blk = (void *)io - offsetof(struct vioblk_device, io);


    if (pos + len > blk->regs->config.blk.capacity * blk->blksz) {
        return -EINVAL; //return invalid if request would be too big
    }

    if (len % blk->blksz != 0 || pos % blk->blksz != 0) {
        return -EINVAL; //len and pos must be multiple of 512

    }
    uint8_t *buf_ptr = (uint8_t *)buf; // pointer to buffer
    // calculating number of sectors to write and starting position
    uint64_t num_sectors = len / blk->blksz;
    uint64_t start_sector = pos / blk->blksz;
    // allocate 1 request struct
    struct virtio_blk_req *req = kcalloc(1, sizeof(struct virtio_blk_req));

    // set up indirect descriptor that points to descriptor table
    blk->vq.desc[0].addr = (uint64_t)blk->desc_table;
    blk->vq.desc[0].len = 3 * sizeof(struct virtq_desc);
    blk->vq.desc[0].flags = VIRTQ_DESC_F_INDIRECT;

    // descriptor table 3 descriptors
    // first descriptor header must read type and sector
    blk->desc_table[0].addr = (uint64_t)req; // length to read type, reserved, and sector values
    blk->desc_table[0].len = sizeof(req->type) + sizeof(req->reserved) + sizeof(req->sector);
    blk->desc_table[0].flags = VIRTQ_DESC_F_NEXT; // next flag and no write flag so that it reads
    blk->desc_table[0].next = 1;                  // points to next desc

    // second descriptor reads 512 bytes from request data buffer
    blk->desc_table[1].addr = (uint64_t)(req->data); // points to data buffer to read from
    blk->desc_table[1].len = blk->blksz;             // 512 bytes
    blk->desc_table[1].flags = VIRTQ_DESC_F_NEXT;
    blk->desc_table[1].next = 2;

    // last descriptor writes status to status value
    blk->desc_table[2].addr = (uint64_t)&(req->status); // points to status value
    blk->desc_table[2].len = sizeof(req->status);
    blk->desc_table[2].flags = VIRTQ_DESC_F_WRITE; // last desc so no next flag

    // loop through each sector
    for (uint64_t sector_count = 0; sector_count < num_sectors; sector_count++)
    {
        uint64_t curr_sector = start_sector + sector_count;

        req->type = VIRTIO_BLK_T_OUT; // to write to block device
        req->sector = curr_sector;

        uint8_t *buf_offset = buf_ptr + sector_count * blk->blksz; // address of specific part of buffer to read from
        memcpy(req->data, buf_offset, blk->blksz);

        blk->vq.avail.ring[0] = 0;
        blk->vq.avail.idx++; // lets device know that new descriptor has been added
        virtio_notify_avail(blk->regs, 0);
        long pie = disable_interrupts();

        while (blk->vq.used.idx != blk->vq.avail.idx) { //used and avail is same meaning device didnt update used idx
            //kprintf("test2\n");

            condition_wait(&blk->vioblk_cond);
            restore_interrupts(pie);
        }
        blk->vq.last_used_idx++; // have last used idx catch up meaning descriptor has been read by device
    }
    kfree(req);
    return len;
}

int vioblk_cntl(struct io *io, int cmd, void *arg)
{
    struct vioblk_device *const blk = (void *)io - offsetof(struct vioblk_device, io);
    if (cmd == IOCTL_GETEND)
    {
        uint64_t *end = (uint64_t *)arg;
        *end = blk->regs->config.blk.capacity * blk->blksz;
        return 0;

    } else if (cmd == IOCTL_GETBLKSZ) {
        //uint32_t *blksz = (uint32_t *)arg;
        //*blksz = blk->blksz;
        return 512;
    } else {

        return -ENOTSUP;
    }
}

void vioblk_isr(int srcno, void *aux)
{
    struct vioblk_device *const blk = aux;
    // acknowledge interrupt
    blk->regs->interrupt_ack = blk->regs->interrupt_status;
    // Set bufcnt and broadcast condition if necessary

    while (blk->vq.used.idx != blk->vq.last_used_idx)
    {
        // blk->bufcnt = VIORNG_BUFSZ;
        blk->vq.last_used_idx++;
    }
    condition_broadcast(&blk->vioblk_cond);
}