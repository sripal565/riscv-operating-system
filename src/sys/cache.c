#include "io.h"
#include "cache.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
// #include "dev/virtio.h"
#include "heap.h"
// Definitions
#define CAPACITY 64

// struct
struct cache_block
{
    uint64_t pos;
    uint8_t *data;
    int dirty;
    int used;
    struct cache_block *prev;
    struct cache_block *next;
    struct lock *b_lock;
};

struct cache
{
    struct io *bkgio;
    struct cache_block cached_blocks[64];
    struct cache_block *head;
    struct cache_block *tail;
    struct lock *cache_lock;
};

// void lru_insert(struct cache *cache, struct cache_block *block);

int create_cache(struct io *bkgio, struct cache **cptr)
{
    struct cache *cache_device;
    cache_device = kcalloc(1, sizeof(struct cache));
    cache_device->bkgio = bkgio;

    cache_device->cache_lock = kcalloc(1, sizeof(struct lock));
    lock_init(cache_device->cache_lock);

    for (int i = 0; i < CAPACITY; i++)
    {
        cache_device->cached_blocks[i].used = 0;
        cache_device->cached_blocks[i].dirty = CACHE_CLEAN;
        cache_device->cached_blocks[i].pos = 0;
        cache_device->cached_blocks[i].b_lock = kcalloc(1, sizeof(struct lock));
        lock_init(cache_device->cached_blocks[i].b_lock);
        if (i > 0)
        {
            cache_device->cached_blocks[i].prev = &cache_device->cached_blocks[i - 1];
        }
        else
        {
            cache_device->cached_blocks[i].prev = NULL;
        }
        if (i < CAPACITY - 1)
        {
            cache_device->cached_blocks[i].next = &cache_device->cached_blocks[i + 1];
        }
        else
        {
            cache_device->cached_blocks[i].next = NULL;
        }
    }

    cache_device->head = &cache_device->cached_blocks[0];
    cache_device->tail = &cache_device->cached_blocks[CAPACITY - 1];
    *cptr = cache_device;
    return 0;
}
int cache_get_block(struct cache *cache, unsigned long long pos, void **pptr)
{
    struct cache_block *cur;

    lock_acquire(cache->cache_lock);
    for (int i = 0; i < CAPACITY; i++)
    {
        cur = &cache->cached_blocks[i];
        if (cur->used && cur->pos == pos)
        {
            // block is in cache so we need to acquire lock to use
            lock_acquire(cur->b_lock);
            lru_remove(cache, cur);
            lru_insert(cache, cur);
            *pptr = cur->data;
            lock_release(cache->cache_lock);
            return 0;
        }
    }

    for (int i = 0; i < CAPACITY; i++)
    {
        cur = &cache->cached_blocks[i];
        if (cur->used == 0)
        {
            if (cur->dirty)
            {
                lock_acquire(cur->b_lock);
                int flush = flush_block(cache, cur);
                if (flush < 0)
                {
                    lock_release(cur->b_lock);
                    lock_release(cache->cache_lock);
                    return flush;
                }
                lock_release(cur->b_lock);
            }

            // int read = vioblk_readat(cache->bkgio, pos, cur->data, CACHE_BLKSZ);
            // add lock?
            cache->cached_blocks[i].data = kcalloc(CACHE_BLKSZ, sizeof(uint8_t));
            int read = ioreadat(cache->bkgio, pos, cur->data, CACHE_BLKSZ);
            if (read < 0)
            {
                lock_release(cache->cache_lock);
                return read;
            }
            // added new block so initialize it's lock
            // cur->b_lock = kcalloc(1, sizeof(struct lock));
            // lock_init(cur->b_lock);
            lock_acquire(cur->b_lock);
            cur->used = 1;
            cur->pos = pos;
            cur->dirty = CACHE_CLEAN;
            lru_remove(cache, cur);
            lru_insert(cache, cur);
            *pptr = cur->data;
            lock_release(cache->cache_lock);
            return 0;
        }
    }

    cur = cache->head;
    // lock_acquire(cache->cache_lock);
    lock_acquire(cur->b_lock);
    if (cur->dirty)
    {
        int flush = flush_block(cache, cur);
        if (flush < 0)
        {
            lock_release(cur->b_lock);
            lock_release(cache->cache_lock);
            return flush;
        }
    }
    // int read = vioblk_readat(cache->bkgio, pos, cur->data, CACHE_BLKSZ);
    // cache->head->data = kcalloc(CACHE_BLKSZ, sizeof(uint8_t));
    int read = ioreadat(cache->bkgio, pos, cur->data, CACHE_BLKSZ);
    if (read < 0)
    {
        lock_release(cur->b_lock);
        lock_release(cache->cache_lock);
        return read;
    }
    // added new block so initialize it's lock
    // cur->b_lock = kcalloc(1, sizeof(struct lock));
    // lock_init(cur->b_lock);
    // lock_acquire(cur->b_lock);
    cur->used = 1;
    cur->pos = pos;
    cur->dirty = CACHE_CLEAN;
    lru_remove(cache, cur);
    lru_insert(cache, cur);
    *pptr = cur->data;
    lock_release(cache->cache_lock);
    return 0;
}

void cache_release_block(struct cache *cache, void *pblk, int dirty)
{
    struct cache_block *cur;

    lock_acquire(cache->cache_lock);
    for (int i = 0; i < CAPACITY; i++)
    {
        cur = &cache->cached_blocks[i];
        if (cur->data == pblk)
        {
            // thread that used cache block is done so release lock now
            lock_release(cur->b_lock);
            lru_remove(cache, cur);
            lru_insert(cache, cur);
            cur->dirty = dirty;
        }
    }
    lock_release(cache->cache_lock);
}

int cache_flush(struct cache *cache)
{
    struct cache_block *cur;

    lock_acquire(cache->cache_lock);
    for (int i = 0; i < CAPACITY; i++)
    {
        cur = &cache->cached_blocks[i];
        if (cur->used && cur->dirty)
        {
            lock_acquire(cur->b_lock);
            lru_remove(cache, cur);
            lru_insert(cache, cur);
            int flush = flush_block(cache, cur);
            if (flush < 0)
            {
                lock_release(cur->b_lock);
                lock_release(cache->cache_lock);
                return flush;
            }
            lock_release(cur->b_lock);
        }
    }
    lock_release(cache->cache_lock);
    return 0;
}

void lru_remove(struct cache *cache, struct cache_block *block)
{
    if (cache->head == block)
    {
        cache->head = block->next;
        if (cache->head)
            cache->head->prev = NULL;
    }
    else
    {
        if (block->prev)
            block->prev->next = block->next;
    }
    if (cache->tail == block)
    {
        cache->tail = block->prev;
        if (cache->tail)
            cache->tail->next = NULL;
    }
    else
    {
        if (block->next)
            block->next->prev = block->prev;
    }
    block->prev = NULL;
    block->next = NULL;
}

void lru_insert(struct cache *cache, struct cache_block *block)
{
    if (cache->tail == NULL)
    {
        cache->head = block;
        cache->tail = block;
    }
    else
    {
        block->prev = cache->tail;
        cache->tail->next = block;
        cache->tail = block;
    }
    block->next = NULL;
}

int flush_block(struct cache *cache, struct cache_block *blk)
{
    // int write = vioblk_writeat(cache->bkgio, blk->pos, blk->data, CACHE_BLKSZ);
    int write = iowriteat(cache->bkgio, blk->pos, blk->data, CACHE_BLKSZ);
    if (write < 0)
    {
        return write;
    }
    blk->dirty = CACHE_CLEAN;
    return 0;
}