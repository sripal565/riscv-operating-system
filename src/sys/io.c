// io.c - Unified I/O object
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "io.h"
#include "ioimpl.h"
#include "assert.h"
#include "string.h"
#include "heap.h"
#include "error.h"
#include "thread.h"
#include "memory.h"
#include "intr.h"

#include <stddef.h>
#include <limits.h>
// INTERNAL TYPE DEFINITIONS
//

struct memio
{
    struct io io; // I/O struct of memory I/O
    void *buf;    // Block of memory
    size_t size;  // Size of memory block
};

// #define PIPE_BUFSZ PAGE_SIZE
struct pipeio
{
    struct io wio;
    struct io rio;

    unsigned long hpos;
    unsigned long tpos;

    void *buf;

    struct condition write_block;
    struct condition read_block;
    struct lock *pipelock;
};

struct seekio
{
    struct io io;           // I/O struct of seek I/O
    struct io *bkgio;       // Backing I/O supporting _readat_ and _writeat_
    unsigned long long pos; // Current position within backing endpoint
    unsigned long long end; // End position in backing endpoint
    int blksz;              // Block size of backing endpoint
};

// INTERNAL FUNCTION DEFINITIONS
//
static long pipe_read(struct io *io, void *buf, long bufsz);

static long pipe_write(struct io *io, const void *buf, long bufsz);

static void pipe_rclose(struct io *io);

static void pipe_wclose(struct io *io);

static int memio_cntl(struct io *io, int cmd, void *arg);

static long memio_readat(
    struct io *io, unsigned long long pos, void *buf, long bufsz);

static long memio_writeat(
    struct io *io, unsigned long long pos, const void *buf, long len);

static void seekio_close(struct io *io);

static int seekio_cntl(struct io *io, int cmd, void *arg);

static long seekio_read(struct io *io, void *buf, long bufsz);

static long seekio_write(struct io *io, const void *buf, long len);

static long seekio_readat(
    struct io *io, unsigned long long pos, void *buf, long bufsz);

static long seekio_writeat(
    struct io *io, unsigned long long pos, const void *buf, long len);

// INTERNAL GLOBAL CONSTANTS
static const struct iointf seekio_iointf = {
    .close = &seekio_close,
    .cntl = &seekio_cntl,
    .read = &seekio_read,
    .write = &seekio_write,
    .readat = &seekio_readat,
    .writeat = &seekio_writeat};

static const struct iointf memio_iointf = {
    .cntl = &memio_cntl,
    .readat = &memio_readat,
    .writeat = &memio_writeat};

static const struct iointf pipeio_riointf = {
    // .cntl = &pipe_cntl,
    .read = &pipe_read,
    // .write = &pipe_write,
    .close = &pipe_rclose};

static const struct iointf pipeio_wiointf = {
    // .cntl = &pipe_cntl,
    // .read = &pipe_read,
    .write = &pipe_write,
    .close = &pipe_wclose};

// EXPORTED FUNCTION DEFINITIONS
//

struct io *ioinit0(struct io *io, const struct iointf *intf)
{
    assert(io != NULL);
    assert(intf != NULL);
    io->intf = intf;
    io->refcnt = 0;
    return io;
}

struct io *ioinit1(struct io *io, const struct iointf *intf)
{
    assert(io != NULL);
    io->intf = intf;
    io->refcnt = 1;
    return io;
}

unsigned long iorefcnt(const struct io *io)
{
    assert(io != NULL);
    return io->refcnt;
}

struct io *ioaddref(struct io *io)
{
    assert(io != NULL);
    io->refcnt += 1;
    return io;
}

void ioclose(struct io *io)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    assert(io->refcnt != 0);
    io->refcnt -= 1;

    if (io->refcnt == 0 && io->intf->close != NULL)
        io->intf->close(io);
}

long ioread(struct io *io, void *buf, long bufsz)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    return io->intf->read(io, buf, bufsz);
}

long iofill(struct io *io, void *buf, long bufsz)
{
    long bufpos = 0; // position in buffer for next read
    long nread;      // result of last read

    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    while (bufpos < bufsz)
    {
        nread = io->intf->read(io, buf + bufpos, bufsz - bufpos);

        if (nread <= 0)
            return (nread < 0) ? nread : bufpos;

        bufpos += nread;
    }

    return bufpos;
}

long iowrite(struct io *io, const void *buf, long len)
{
    long bufpos = 0; // position in buffer for next write
    long n;          // result of last write

    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->write == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;

    do
    {
        n = io->intf->write(io, buf + bufpos, len - bufpos);

        if (n <= 0)
            return (n < 0) ? n : bufpos;

        bufpos += n;
    } while (bufpos < len);

    return bufpos;
}

long ioreadat(
    struct io *io, unsigned long long pos, void *buf, long bufsz)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->readat == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    return io->intf->readat(io, pos, buf, bufsz);
}

long iowriteat(
    struct io *io, unsigned long long pos, const void *buf, long len)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->writeat == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;

    return io->intf->writeat(io, pos, buf, len);
}

int ioctl(struct io *io, int cmd, void *arg)
{
    assert(io != NULL);
    assert(io->intf != NULL);

    if (io->intf->cntl != NULL)
        return io->intf->cntl(io, cmd, arg);
    else if (cmd == IOCTL_GETBLKSZ)
        return 1; // default block size
    else
        return -ENOTSUP;
}

int ioblksz(struct io *io)
{
    return ioctl(io, IOCTL_GETBLKSZ, NULL);
}

int ioseek(struct io *io, unsigned long long pos)
{
    return ioctl(io, IOCTL_SETPOS, &pos);
}

struct io *create_memory_io(void *buf, size_t size)
{
    // FIX ME
    struct memio *mio;

    // ensure valid arguments
    if (buf == NULL)
    {
        return NULL;
    }
    if (size == 0)
    {
        return NULL;
    }

    // allocate memory for memio struct
    mio = kcalloc(1, sizeof(struct memio));
    mio->buf = buf;
    mio->size = size;

    // should we use ioinit 1 or ioinit 0?
    return ioinit1(&mio->io, &memio_iointf);
}

struct io *create_seekable_io(struct io *io)
{
    struct seekio *sio;
    unsigned long end;
    int result;
    int blksz;

    blksz = ioblksz(io);
    assert(0 < blksz);

    // block size must be power of two
    assert((blksz & (blksz - 1)) == 0);

    result = ioctl(io, IOCTL_GETEND, &end);
    assert(result == 0);

    sio = kcalloc(1, sizeof(struct seekio));

    sio->pos = 0;
    sio->end = end;
    sio->blksz = blksz;
    sio->bkgio = ioaddref(io);

    return ioinit1(&sio->io, &seekio_iointf);
};

void create_pipe(struct io **wioptr, struct io **rioptr)
{
    struct pipeio *pipe;

    pipe = kcalloc(1, sizeof(struct pipeio));
    pipe->buf = alloc_phys_page();
    *wioptr = &pipe->wio;
    *rioptr = &pipe->rio;
    pipe->hpos = 0;
    pipe->tpos = 0;
    condition_init(&pipe->read_block, "readblock");
    condition_init(&pipe->write_block, "writeblock");
    pipe->pipelock = kcalloc(1, sizeof(struct lock));
    lock_init(pipe->pipelock);
    // should we use ioinit 1 or ioinit 0?
    ioinit1(&pipe->wio, &pipeio_wiointf);
    ioinit1(&pipe->rio, &pipeio_riointf);
}

// INTERNAL FUNCTION DEFINITIONS
//

long pipe_read(struct io *io, void *buf, long bufsz)
{
    struct pipeio *pipe = (void *)io - offsetof(struct pipeio, rio);
    // while (pipe->tpos == pipe->hpos)
    // {
    //     condition_wait(&pipe->read_block);
    // }
    int read = 0;
    char *rdbuf = (char *)buf;
    char *dbuf = (char *)pipe->buf;

    if (pipe->wio.refcnt == 0)
    {

        while ((pipe->tpos - pipe->hpos) > 0 && read < bufsz)
        {
            // dont need to block if no writer
            lock_acquire(pipe->pipelock);
            rdbuf[read] = dbuf[pipe->hpos % PAGE_SIZE];
            pipe->hpos++;
            read++;
            lock_release(pipe->pipelock);
        }

        // return read or EOF? is EOF = 0?
        return read;
        // return 0;
    }
    // lock_acquire(pipe->pipelock);

    while (read < bufsz)
    {
        int pie = disable_interrupts();
        while ((pipe->tpos - pipe->hpos) == 0)
        {
            condition_wait(&pipe->read_block);
            disable_interrupts();
        }
        restore_interrupts(pie);

        lock_acquire(pipe->pipelock);
        rdbuf[read] = dbuf[pipe->hpos % PAGE_SIZE];
        pipe->hpos++;
        read++;
        lock_release(pipe->pipelock);
        condition_broadcast(&pipe->write_block);
    }
    // lock_release(pipe->pipelock);
    return read;
}

long pipe_write(struct io *io, const void *buf, long bufsz)
{
    struct pipeio *pipe = (void *)io - offsetof(struct pipeio, wio);
    // while ((pipe->tpos - pipe->hpos) == PAGE_SIZE)
    // {
    //     condition_wait(&pipe->write_block);
    // }

    if (pipe->rio.refcnt == 0)
    {
        return -EPIPE;
    }
    // lock_acquire(pipe->pipelock);
    int write = 0;
    char *wbuf = (char *)buf;
    char *dbuf = (char *)pipe->buf;
    while (write < bufsz)
    {
        int pie = disable_interrupts();
        while ((pipe->tpos - pipe->hpos) >= PAGE_SIZE)
        {
            condition_wait(&pipe->write_block);
            disable_interrupts();
        }
        restore_interrupts(pie);

        lock_acquire(pipe->pipelock);
        dbuf[pipe->tpos % PAGE_SIZE] = wbuf[write];
        pipe->tpos++;
        write++;
        lock_release(pipe->pipelock);
        condition_broadcast(&pipe->read_block);
    }
    // lock_release(pipe->pipelock);
    return write;
}

void pipe_rclose(struct io *io)
{
    struct pipeio *pipe = (void *)io - offsetof(struct pipeio, rio);
    condition_broadcast(&pipe->write_block);
    condition_broadcast(&pipe->read_block);
    if (pipe->wio.refcnt == 0 && pipe->rio.refcnt == 0)
    {
        free_phys_page(pipe->buf);
        kfree(pipe->pipelock);
        kfree(pipe);
    }
    else
    {
        lock_release(pipe->pipelock);
    }
}

void pipe_wclose(struct io *io)
{
    struct pipeio *pipe = (void *)io - offsetof(struct pipeio, wio);
    condition_broadcast(&pipe->write_block);
    condition_broadcast(&pipe->read_block);
    if (pipe->wio.refcnt == 0 && pipe->rio.refcnt == 0)
    {
        free_phys_page(pipe->buf);
        kfree(pipe->pipelock);
        kfree(pipe);
    }
    else
    {
        lock_release(pipe->pipelock);
    }
}

long memio_readat(
    struct io *io,
    unsigned long long pos,
    void *buf, long bufsz)
{
    // FIX ME
    struct memio *mio = (void *)io - offsetof(struct memio, io);
    // ensure valid arguments
    assert(io != NULL);

    // perform boundary checks
    if (mio->size <= pos)
    {
        return 0;
    }
    if (bufsz < 0)
    {
        return -EINVAL;
    }
    // if bufsz is larger than space left in buffer truncate read
    if (pos + bufsz > mio->size)
    {
        bufsz = mio->size - pos;
    }
    if (0 < bufsz)
    {
        memcpy(buf, mio->buf + pos, bufsz);
    }
    return bufsz;
}

long memio_writeat(
    struct io *io,
    unsigned long long pos,
    const void *buf, long len)
{
    // FIX ME
    struct memio *mio = (void *)io - offsetof(struct memio, io);
    // ensure valid arguments
    assert(io != NULL);
    if (len < 0)
    {
        return -EINVAL;
    }

    // perform boundary checks
    if (mio->size <= pos)
    {
        return 0;
    }
    // if bufsz is larger than space left in buffer truncate write
    if (pos + len > mio->size)
    {
        len = mio->size - pos;
    }
    if (0 < len)
    {
        memcpy(mio->buf + pos, buf, len);
    }
    return len;
}

int memio_cntl(struct io *io, int cmd, void *arg)
{
    // FIX ME
    unsigned long long *ullarg = arg;
    struct memio *mio = (void *)io - offsetof(struct memio, io);
    switch (cmd)
    {
    case IOCTL_GETBLKSZ:
        return 1;
    case IOCTL_GETEND:
        *ullarg = mio->size;
        return 0;
    default:
        return -ENOTSUP;
    }
}

void seekio_close(struct io *io)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    ioclose(sio->bkgio);
    kfree(sio);
}

int seekio_cntl(struct io *io, int cmd, void *arg)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    unsigned long long *ullarg = arg;
    int result;

    switch (cmd)
    {
    case IOCTL_GETBLKSZ:
        return sio->blksz;
    case IOCTL_GETPOS:
        *ullarg = sio->pos;
        return 0;
    case IOCTL_SETPOS:
        // New position must be multiple of block size
        if ((*ullarg & (sio->blksz - 1)) != 0)
            return -EINVAL;

        // New position must not be past end
        if (*ullarg > sio->end)
            return -EINVAL;

        sio->pos = *ullarg;
        return 0;
    case IOCTL_GETEND:
        *ullarg = sio->end;
        return 0;
    case IOCTL_SETEND:
        // Call backing endpoint ioctl and save result
        result = ioctl(sio->bkgio, IOCTL_SETEND, ullarg);
        if (result == 0)
            sio->end = *ullarg;
        return result;
    default:
        return ioctl(sio->bkgio, cmd, arg);
    }
}

long seekio_read(struct io *io, void *buf, long bufsz)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long const end = sio->end;
    long rcnt;

    // Cannot read past end
    if (end - pos < bufsz)
        bufsz = end - pos;

    if (bufsz == 0)
        return 0;

    // Request must be for at least blksz bytes if not zero
    if (bufsz < sio->blksz)
        return -EINVAL;

    // Truncate buffer size to multiple of blksz
    bufsz &= ~(sio->blksz - 1);

    rcnt = ioreadat(sio->bkgio, pos, buf, bufsz);
    sio->pos = pos + ((rcnt < 0) ? 0 : rcnt);
    return rcnt;
}

long seekio_write(struct io *io, const void *buf, long len)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long end = sio->end;
    int result;
    long wcnt;

    if (len == 0)
        return 0;

    // Request must be for at least blksz bytes
    if (len < sio->blksz)
        return -EINVAL;

    // Truncate length to multiple of blksz
    len &= ~(sio->blksz - 1);

    // Check if write is past end. If it is, we need to change end position.

    if (end - pos < len)
    {
        if (ULLONG_MAX - pos < len)
            return -EINVAL;

        end = pos + len;

        result = ioctl(sio->bkgio, IOCTL_SETEND, &end);

        if (result != 0)
            return result;

        sio->end = end;
    }

    wcnt = iowriteat(sio->bkgio, sio->pos, buf, len);
    sio->pos = pos + ((wcnt < 0) ? 0 : wcnt);
    return wcnt;
}

long seekio_readat(
    struct io *io, unsigned long long pos, void *buf, long bufsz)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    return ioreadat(sio->bkgio, pos, buf, bufsz);
}

long seekio_writeat(
    struct io *io, unsigned long long pos, const void *buf, long len)
{
    struct seekio *const sio = (void *)io - offsetof(struct seekio, io);
    return iowriteat(sio->bkgio, pos, buf, len);
}