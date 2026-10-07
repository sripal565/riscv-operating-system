// ktfs.c - KTFS implementation
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"
#include "heap.h"
#include "assert.h"
#include "io.h"
// INTERNAL TYPE DEFINITIONS
//
#define DIRECT_BLOCKS (uint32_t)3
#define DIRECT_CAPACITY (uint32_t)DIRECT_BLOCKS * 512
#define INDIRECT_BLOCKS (uint32_t)(512 / sizeof(uint32_t))
#define INDIRECT_CAPACITY (uint32_t)INDIRECT_BLOCKS * 512

#define DENTRY_PER_BLOCK (uint32_t)(KTFS_BLKSZ / sizeof(struct ktfs_dir_entry))
#define DIRECT_DENTRIES (uint32_t)DIRECT_BLOCKS *DENTRY_PER_BLOCK
#define INDIRECT_DENTRIES (uint32_t)INDIRECT_BLOCKS *DENTRY_PER_BLOCK

struct ktfs_file
{
    // Fill to fulfill spec
    struct io io;
    uint32_t file_size;
    struct ktfs_dir_entry *dentry;
    uint32_t flags;
    struct ktfs_file *next;
};

// struct file_list
// {
//     char name[KTFS_MAX_FILENAME_LEN + sizeof(uint8_t)];
//     struct ktfs_file *next;
// };

struct filesystem
{
    struct ktfs_superblock superblock;
    uint8_t padding[KTFS_BLKSZ - 14];
    // struct ktfs_bitmap *bitmaps;
    uint32_t bitmap_blk_cnt;
    // struct ktfs_inode *inodes;
    uint32_t inode_blk_cnt;
    // struct ktfs_data_block *data_blocks;
    uint32_t data_blk_cnt;
    struct ktfs_file *head;
};

// struct inode_list
// {
//     uint16_t used_inode;
//     struct inode_list *next;
// };

struct filesystem *system;

struct cache *fcache;

// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_mount(struct io *io);

int ktfs_open(const char *name, struct io **ioptr);
void ktfs_close(struct io *io);
long ktfs_readat(struct io *io, unsigned long long pos, void *buf, long len);
long ktfs_writeat(struct io *io, unsigned long long pos, const void *buf, long len);
int ktfs_create(const char *name);
int ktfs_delete(const char *name);
int ktfs_cntl(struct io *io, int cmd, void *arg);

int ktfs_getblksz(struct ktfs_file *fd);
int ktfs_getend(struct ktfs_file *fd, void *arg);

int ktfs_flush(void);

uint16_t find_open_inode(void);
int file_exists(const char *name);
uint32_t find_open_block(void);
struct ktfs_file *is_file_open(const char *name);
void insert_file(struct ktfs_file *file);
void remove_file(struct ktfs_file *file);
struct ktfs_dir_entry *get_file_dentry(const char *name);
int freeblock(uint32_t blocknum);
void read_dir_entry(uint32_t idx, struct ktfs_dir_entry *out);
void write_dir_entry(uint32_t idx, struct ktfs_dir_entry *in);

// FUNCTION ALIASES
//

int fsmount(struct io *io)
    __attribute__((alias("ktfs_mount")));

int fsopen(const char *name, struct io **ioptr)
    __attribute__((alias("ktfs_open")));

int fsflush(void)
    __attribute__((alias("ktfs_flush")));

int fscreate(const char *name)
    __attribute__((alias("ktfs_create")));

int fsdelete(const char *name)
    __attribute__((alias("ktfs_delete")));

// EXPORTED FUNCTION DEFINITIONS
//

int ktfs_mount(struct io *io)
{
    // long rcnt;
    // kprintf("%d \n", ioctl(io, IOCTL_GETBLKSZ, NULL));
    assert(ioctl(io, IOCTL_GETBLKSZ, NULL) == 512);

    system = kcalloc(1, sizeof(struct filesystem));

    // char tempbuf[KTFS_BLKSZ];
    // rcnt = ioreadat(io, 0, tempbuf, sizeof(tempbuf));
    // if (rcnt < 0)
    // {
    //     return rcnt;
    // }
    // if (rcnt != KTFS_BLKSZ)
    // {
    //     return -EIO;
    // }

    // !!!! create_cache() !!!!
    // What should be my bkgio argument for create cache
    create_cache(io, &fcache);
    // get suprblock from cache?
    void *super_ptr;
    int get = cache_get_block(fcache, 0, &super_ptr);
    if (get < 0)
    {
        return get;
    }
    memcpy(&system->superblock, super_ptr, sizeof(system->superblock));
    cache_release_block(fcache, super_ptr, 0);
    // memcpy(system, tempbuf, sizeof(system->superblock));
    uint32_t bitmap_count = system->superblock.bitmap_block_count;
    uint32_t inode_count = system->superblock.inode_block_count;
    uint32_t block_count = system->superblock.block_count;

    // useful to cache bitmap, superblock, and root inode to memory
    // The rest can be accessed on disk as necessary?
    // system->bitmaps = kcalloc(1, KTFS_BLKSZ * bitmap_count);
    system->bitmap_blk_cnt = bitmap_count;
    system->inode_blk_cnt = inode_count;
    system->data_blk_cnt = block_count - inode_count - bitmap_count - 1;
    system->head = NULL;

    ioaddref(io);

    return 0;
}

int ktfs_open(const char *name, struct io **ioptr)
{
    // struct ktfs_file *file;
    struct ktfs_file *file = is_file_open(name);
    if (file != NULL)
    {
        return -EBUSY;
    }

    static const struct iointf ktfs_iointf = {
        .close = &ktfs_close,
        .readat = &ktfs_readat,
        .writeat = &ktfs_writeat,
        .cntl = &ktfs_cntl};

    if (name == NULL || *name == '\0')
    {
        return -ENOENT;
    }

    // Access root_inode using cache code
    void *inode_ptr;
    struct ktfs_inode *inode_blk;

    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;

    int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    if (get < 0)
    {
        return get;
    }

    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];
    // cache_release_block(fcache, inode_ptr, 0);
    // calculate dentry counts
    uint32_t total_dentries = root_inode->size / sizeof(struct ktfs_dir_entry);
    uint32_t dentries_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    cache_release_block(fcache, inode_ptr, 0);
    // int entry_found = 0;
    // struct ktfs_dir_entry found_entry;
    uint32_t entries_scanned = 0;

    // depending on number of inodes_used, iterate through block[3], then indirect, then dindirect[2]
    // one dentry is 16 bytes, one block contains 512/16 = 32 dentries, so if i am accessing dentry 33, go to block[1]. But dentry 127, would be in the 0th block in the block of indirect blocks, and so on.
    //^^^^works similarly for accesing a position in a file
    for (int i = 0; i < 3 && entries_scanned < total_dentries; i++)
    {
        // if (root_inode->block[i] == 0)
        //     continue;
        // Compute absolute block number for the data block:
        uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[i];
        void *dentry_ptr;
        struct ktfs_dir_entry *dentries;
        get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &dentry_ptr);
        dentries = (struct ktfs_dir_entry *)dentry_ptr;
        if (get < 0)
        {
            cache_release_block(fcache, dentry_ptr, 0);
            return get;
        }
        // struct ktfs_dir_entry *dentries = (struct ktfs_dir_entry *)data_blk;
        for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
        {
            if (strncmp(name, dentries[j].name, KTFS_MAX_FILENAME_LEN) == 0)
            {
                // found_entry = dentries[j];
                // entry_found = 1;
                // break;
                struct ktfs_file *file = kcalloc(1, sizeof(struct ktfs_file));
                file->dentry = kcalloc(1, sizeof(struct ktfs_dir_entry));
                memcpy(file->dentry, &dentries[j], sizeof(struct ktfs_dir_entry));
                file->flags = KTFS_FILE_IN_USE;
                // Optionally, you could now retrieve the inode for the file (using found_entry.inode)
                // to set file->file_size and other fields.
                cache_release_block(fcache, dentry_ptr, 0);

                struct io *original = ioinit0(&file->io, &ktfs_iointf);
                *ioptr = create_seekable_io(original);
                insert_file(file);
                return 0;
            }
        }
        // if (entry_found)
        //     break;
        cache_release_block(fcache, dentry_ptr, 0);
    }

    // repeat for indirect block
    if (/*!entry_found && */ root_inode->indirect != 0 && entries_scanned < total_dentries)
    {
        uint32_t indirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->indirect;
        void *indirect_ptr;
        // uint32_t *indirect_blk;
        get = cache_get_block(fcache, KTFS_BLKSZ * indirect_blk_pos, &indirect_ptr);
        // indirect_blk = indirect_ptr;
        if (get < 0)
            return get;
        uint32_t *indirect_blk = (uint32_t *)indirect_ptr;
        int num_indirect = KTFS_BLKSZ / sizeof(uint32_t);
        for (int i = 0; i < num_indirect && entries_scanned < total_dentries; i++)
        {
            // if (indirect_blk[i] == 0)
            //     break;

            uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_blk[i];
            void *data_blk;
            get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &data_blk);
            // dentries = dentry_ptr;
            if (get < 0)
                return get;
            struct ktfs_dir_entry *dentries = (struct ktfs_dir_entry *)data_blk;

            for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
            {
                if (strncmp(name, dentries[j].name, KTFS_MAX_FILENAME_LEN) == 0)
                {
                    // found_entry = dentries[j];
                    // entry_found = 1;
                    // break;
                    struct ktfs_file *file = kcalloc(1, sizeof(struct ktfs_file));
                    insert_file(file);
                    file->dentry = kcalloc(1, sizeof(struct ktfs_dir_entry));
                    memcpy(file->dentry, &dentries[j], sizeof(struct ktfs_dir_entry));
                    file->flags = KTFS_FILE_IN_USE;
                    // Optionally, you could now retrieve the inode for the file (using found_entry.inode)
                    // to set file->file_size and other fields.
                    cache_release_block(fcache, dentries, 0);
                    cache_release_block(fcache, indirect_blk, 0);
                    struct io *original = ioinit1(&file->io, &ktfs_iointf);
                    *ioptr = create_seekable_io(original);
                    return 0;
                }
            }
            // if (entry_found)
            //     break;
            cache_release_block(fcache, dentries, 0);
        }

        cache_release_block(fcache, indirect_blk, 0);
    }

    // repeat for dindirect block
    if (/*!entry_found && */ (root_inode->dindirect[0] || root_inode->dindirect[1]) && entries_scanned < total_dentries)
    {
        int num_dindirect = 2;
        int num_indirect_per_blk = KTFS_BLKSZ / sizeof(uint32_t);
        for (int d = 0; d < num_dindirect && entries_scanned < total_dentries; d++)
        {
            // if (root_inode->dindirect[d] == 0)
            //     continue;

            uint32_t dindirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->dindirect[d];
            void *dindirect_blk;
            // uint32_t *dindirect_blk_nums;
            get = cache_get_block(fcache, KTFS_BLKSZ * dindirect_blk_pos, &dindirect_blk);
            if (get < 0)
                return get;
            uint32_t *dindirect_blk_nums = (uint32_t *)dindirect_blk;

            for (int k = 0; k < num_indirect_per_blk && entries_scanned < total_dentries; k++)
            {
                // if (dindirect_blk_nums[k] == 0)
                //     break;

                // uint32_t indirect_blk_abs = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect_blk_nums[k];
                // void *indirect_blk;
                // get = cache_get_block(fcache, indirect_blk_abs, &indirect_blk);
                // if (get < 0)
                //     return get;
                // uint32_t *data_blk_nums = (uint32_t *)indirect_blk;
                uint32_t indirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect_blk_nums[k];
                void *indirect_ptr;
                get = cache_get_block(fcache, KTFS_BLKSZ * indirect_blk_pos, &indirect_ptr);
                if (get < 0)
                    return get;
                uint32_t *indirect_blk = (uint32_t *)indirect_ptr;

                for (int j = 0; j < num_indirect_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
                {
                    // if (indirect_blk[j] == 0)
                    //     break;

                    // uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + data_blk_nums[j];
                    // void *data_blk;
                    // get = cache_get_block(fcache, data_blk_abs, &data_blk);
                    // if (get < 0)
                    //     return get;
                    // struct ktfs_dir_entry *dentries = (struct ktfs_dir_entry *)data_blk;
                    uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_blk[j];
                    void *data_blk;
                    // get = cache_get_block(fcache, data_blk_abs, &data_blk);
                    struct ktfs_dir_entry *dentries;
                    get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &data_blk);
                    dentries = (struct ktfs_dir_entry *)data_blk;
                    if (get < 0)
                        return get;

                    for (int m = 0; m < dentries_per_blk && entries_scanned < total_dentries; m++, entries_scanned++)
                    {
                        // found_entry = dentries[j];
                        // entry_found = 1;
                        // break;
                        struct ktfs_file *file = kcalloc(1, sizeof(struct ktfs_file));
                        insert_file(file);
                        file->dentry = kcalloc(1, sizeof(struct ktfs_dir_entry));
                        memcpy(file->dentry, &dentries[m], sizeof(struct ktfs_dir_entry));
                        file->flags = KTFS_FILE_IN_USE;
                        // Optionally, you could now retrieve the inode for the file (using found_entry.inode)
                        // to set file->file_size and other fields.
                        cache_release_block(fcache, dentries, 0);
                        cache_release_block(fcache, indirect_blk, 0);
                        cache_release_block(fcache, dindirect_blk_nums, 0);
                        struct io *original = ioinit1(&file->io, &ktfs_iointf);
                        *ioptr = create_seekable_io(original);
                        return 0;
                    }
                    // if (entry_found)
                    //     break;
                    cache_release_block(fcache, dentries, 0);
                }
                // if (entry_found)
                //     break;
                cache_release_block(fcache, indirect_blk, 0);
            }
            // if (entry_found)
            //     break;
            cache_release_block(fcache, dindirect_blk_nums, 0);
        }
    }

    return -ENOENT;
}

void ktfs_close(struct io *io)
{
    struct ktfs_file *fio = (void *)io - offsetof(struct ktfs_file, io);
    remove_file(fio);
    kfree(fio->dentry);
    kfree(fio);
    return;
}

long ktfs_readat(struct io *io, unsigned long long pos, void *buf, long len)
{
    struct ktfs_file *fio = (void *)io - offsetof(struct ktfs_file, io);
    uint32_t inode_idx = fio->dentry->inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (inode_idx / inodes_per_blk);
    uint32_t inode_idx_in_blk = inode_idx % inodes_per_blk;

    struct ktfs_inode *inode_blk;
    void *inode_ptr;
    int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    struct ktfs_inode *inode = &inode_blk[inode_idx_in_blk];

    uint32_t indirect_number = inode->indirect;
    uint32_t dindirect[2];
    dindirect[0] = inode->dindirect[0];
    dindirect[1] = inode->dindirect[1];
    cache_release_block(fcache, inode_ptr, 0);
    if (get < 0)
    {
        return get;
    }

    fio->file_size = inode->size;

    unsigned long long bytes_read = 0;

    uint32_t size = fio->file_size;

    // int get;

    if (pos > size)
    {
        return 0;
    }

    char *rdbuf = (char *)buf;

    while (pos + bytes_read < size && bytes_read < len)
    {
        int f_block_idx = (pos + bytes_read) / KTFS_BLKSZ;

        if (f_block_idx < DIRECT_BLOCKS)
        {
            uint32_t block_pos = (pos + bytes_read) % KTFS_BLKSZ;
            uint32_t direct_blk = inode->block[f_block_idx];

            uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

            void *block_ptr;
            struct ktfs_data_block *block;
            get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
            block = (struct ktfs_data_block *)block_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, block_ptr, 0);
                return get;
            }

            while (bytes_read < len && pos + bytes_read < size && block_pos < KTFS_BLKSZ)
            {
                rdbuf[bytes_read] = block->data[block_pos];
                bytes_read++;
                block_pos++;
            }
            cache_release_block(fcache, block_ptr, 0);
        }
        else if (f_block_idx < DIRECT_BLOCKS + INDIRECT_BLOCKS)
        {
            uint32_t indirect_block_idx = f_block_idx - DIRECT_BLOCKS;

            uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_number;

            void *indirect_ptr;
            uint32_t *indirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
            indirect_block = (uint32_t *)indirect_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, indirect_ptr, 0);
                return get;
            }

            uint32_t block_pos = (pos + bytes_read) % KTFS_BLKSZ;
            uint32_t direct_blk = indirect_block[indirect_block_idx];

            uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

            void *block_ptr;
            struct ktfs_data_block *block;
            get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
            block = (struct ktfs_data_block *)block_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, block_ptr, 0);
                return get;
            }

            while (bytes_read < len && pos + bytes_read < size && block_pos < KTFS_BLKSZ)
            {
                rdbuf[bytes_read] = block->data[block_pos];
                bytes_read++;
                block_pos++;
            }
            cache_release_block(fcache, indirect_ptr, 0);
            cache_release_block(fcache, block_ptr, 0);
        }
        else
        {
            // Direct: 0 to (512*3 - 1); Indirect: 512*3 to (128*512 - 1); Dindirect: 128*512 to (2*(128*128*512) - 1);
            // Direct: 0 to 2: 3 to 130; 131 to
            uint32_t dindirect_block_idx = f_block_idx - (DIRECT_BLOCKS + INDIRECT_BLOCKS);
            uint32_t dindirect_oblock_idx = dindirect_block_idx / (INDIRECT_BLOCKS * INDIRECT_BLOCKS);
            uint32_t dindirect_iblock_idx = (dindirect_block_idx % (INDIRECT_BLOCKS * INDIRECT_BLOCKS)) / INDIRECT_BLOCKS;
            // uint32_t dindirect_iblock_idx = dindirect_block_idx % INDIRECT_BLOCKS;

            uint32_t dindirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect[dindirect_oblock_idx];

            void *dindirect_ptr;
            uint32_t *dindirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * dindirect_data_blk, &dindirect_ptr);
            dindirect_block = (uint32_t *)dindirect_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, dindirect_block, 0);
                return get;
            }

            uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect_block[dindirect_iblock_idx];

            uint32_t indirect_block_idx = dindirect_block_idx % INDIRECT_BLOCKS;

            void *indirect_ptr;
            uint32_t *indirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
            indirect_block = (uint32_t *)indirect_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, indirect_block, 0);
                return get;
            }

            uint32_t block_pos = (pos + bytes_read) % KTFS_BLKSZ;
            uint32_t direct_blk = indirect_block[indirect_block_idx];

            uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

            void *block_ptr;
            struct ktfs_data_block *block;
            get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
            block = (struct ktfs_data_block *)block_ptr;

            if (get < 0)
            {
                // cache_release_block(fcache, block, 0);
                return get;
            }

            while (bytes_read < len && pos + bytes_read < size && block_pos < KTFS_BLKSZ)
            {
                rdbuf[bytes_read] = block->data[block_pos];
                bytes_read++;
                block_pos++;
            }
            cache_release_block(fcache, dindirect_block, 0);
            cache_release_block(fcache, indirect_block, 0);
            cache_release_block(fcache, block, 0);
        }
    }
    cache_release_block(fcache, inode_ptr, 0);
    return bytes_read;
}

long ktfs_writeat(struct io *io, unsigned long long pos, const void *buf, long len)
{
    // write len bytes starting at pos in file. Truncate write at end of file
    struct ktfs_file *fio = (void *)io - offsetof(struct ktfs_file, io);
    uint32_t inode_idx = fio->dentry->inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (inode_idx / inodes_per_blk);
    uint32_t inode_idx_in_blk = inode_idx % inodes_per_blk;

    struct ktfs_inode *inode_blk;
    void *inode_ptr;
    int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    struct ktfs_inode *inode = &inode_blk[inode_idx_in_blk];

    if (get < 0)
    {
        return get;
    }

    fio->file_size = inode->size;

    unsigned long long bytes_written = 0;

    uint32_t size = fio->file_size;

    uint32_t indirect_number = inode->indirect;
    uint32_t dindirect[2];
    dindirect[0] = inode->dindirect[0];
    dindirect[1] = inode->dindirect[1];
    cache_release_block(fcache, inode_ptr, 0);
    // int get;

    if (pos > size)
    {
        return 0;
    }

    char *rdbuf = (char *)buf;

    while (pos + bytes_written < size && bytes_written < len)
    {
        int f_block_idx = (pos + bytes_written) / KTFS_BLKSZ;

        if (f_block_idx < DIRECT_BLOCKS)
        {
            uint32_t block_pos = (pos + bytes_written) % KTFS_BLKSZ;
            uint32_t direct_blk = inode->block[f_block_idx];

            uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

            void *block_ptr;
            struct ktfs_data_block *block;
            get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
            block = (struct ktfs_data_block *)block_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, block, 0);
                return get;
            }

            while (bytes_written < len && pos + bytes_written < size && block_pos < KTFS_BLKSZ)
            {
                block->data[block_pos] = rdbuf[bytes_written];
                bytes_written++;
                block_pos++;
            }
            cache_release_block(fcache, block_ptr, 1);
        }
        else if (f_block_idx < DIRECT_BLOCKS + INDIRECT_BLOCKS)
        {
            uint32_t indirect_block_idx = f_block_idx - DIRECT_BLOCKS;

            uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_number;

            void *indirect_ptr;
            uint32_t *indirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
            indirect_block = (uint32_t *)indirect_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, indirect_block, 0);
                return get;
            }

            uint32_t block_pos = (pos + bytes_written) % KTFS_BLKSZ;
            uint32_t direct_blk = indirect_block[indirect_block_idx];

            uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

            void *block_ptr;
            struct ktfs_data_block *block;
            get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
            block = (struct ktfs_data_block *)block_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, block, 0);
                return get;
            }

            while (bytes_written < len && pos + bytes_written < size && block_pos < KTFS_BLKSZ)
            {
                block->data[block_pos] = rdbuf[bytes_written];
                bytes_written++;
                block_pos++;
            }
            cache_release_block(fcache, indirect_block, 0);
            cache_release_block(fcache, block, 1);
        }
        else
        {
            uint32_t dindirect_block_idx = f_block_idx - (DIRECT_BLOCKS + INDIRECT_BLOCKS);
            uint32_t dindirect_oblock_idx = dindirect_block_idx / (INDIRECT_BLOCKS * INDIRECT_BLOCKS);
            uint32_t dindirect_iblock_idx = (dindirect_block_idx % (INDIRECT_BLOCKS * INDIRECT_BLOCKS)) / INDIRECT_BLOCKS;

            uint32_t dindirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect[dindirect_oblock_idx];

            void *dindirect_ptr;
            uint32_t *dindirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * dindirect_data_blk, &dindirect_ptr);
            dindirect_block = (uint32_t *)dindirect_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, dindirect_block, 0);
                return get;
            }

            uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect_block[dindirect_iblock_idx];

            uint32_t indirect_block_idx = dindirect_block_idx % INDIRECT_BLOCKS;

            void *indirect_ptr;
            uint32_t *indirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
            indirect_block = (uint32_t *)indirect_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, indirect_block, 0);
                return get;
            }

            uint32_t block_pos = (pos + bytes_written) % KTFS_BLKSZ;
            uint32_t direct_blk = indirect_block[indirect_block_idx];

            uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

            void *block_ptr;
            struct ktfs_data_block *block;
            get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
            block = (struct ktfs_data_block *)block_ptr;

            if (get < 0)
            {
                cache_release_block(fcache, block, 0);
                return get;
            }

            while (bytes_written < len && pos + bytes_written < size && block_pos < KTFS_BLKSZ)
            {
                block->data[block_pos] = rdbuf[bytes_written];
                bytes_written++;
                block_pos++;
            }
            cache_release_block(fcache, dindirect_block, 0);
            cache_release_block(fcache, indirect_block, 0);
            cache_release_block(fcache, block, 1);
        }
    }
    // cache_release_block(fcache, inode_ptr, 0);
    ktfs_flush();
    return bytes_written;
}

int ktfs_create(const char *name)
{
    // check if name is valid and is not already in the dentry list
    if (name == NULL)
    {
        return -EINVAL;
    }

    if (file_exists(name) != 0)
    {
        return -EINVAL;
    }

    void *inode_ptr;
    struct ktfs_inode *inode_blk;

    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;

    int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    if (get < 0)
    {
        return get;
    }

    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];
    // cache_release_block(fcache, inode_blk, 0);
    // calculate dentry counts
    uint32_t total_dentries = root_inode->size / sizeof(struct ktfs_dir_entry);
    // uint32_t dentries_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);

    // int entry_found = 0;
    // struct ktfs_dir_entry found_entry;
    // uint32_t entries_scanned = 0;

    if (total_dentries < DIRECT_DENTRIES)
    {
        int dentryblock = total_dentries / DENTRY_PER_BLOCK;
        int dentryidx = total_dentries % DENTRY_PER_BLOCK;
        if (dentryidx == 0)
        {
            root_inode->block[dentryblock] = find_open_block();
        }

        struct ktfs_dir_entry *dentries;
        void *dentry_ptr;
        cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[dentryblock]), &dentry_ptr);
        dentries = (struct ktfs_dir_entry *)dentry_ptr;
        for (int j = 0; j < KTFS_MAX_FILENAME_LEN + sizeof(uint8_t); j++)
        {
            dentries[dentryidx].name[j] = name[j];
            if (name[j] == '\0')
            {
                break;
            }
        }
        dentries[dentryidx].inode = find_open_inode();
        root_inode->size += sizeof(struct ktfs_dir_entry);
        cache_release_block(fcache, inode_ptr, 1);
        uint32_t inode_idx_in_block = dentries[dentryidx].inode % inodes_per_blk;
        struct ktfs_inode *newinode;
        void *newinodeptr;
        cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + (dentries[dentryidx].inode / inodes_per_blk)), &newinodeptr);
        newinode = (struct ktfs_inode *)newinodeptr;
        newinode[inode_idx_in_block].size = 0;
        for (int i = 0; i < 3; i++)
        {
            newinode[inode_idx_in_block].block[i] = 0;
        }
        for (int i = 0; i < 2; i++)
        {
            newinode[inode_idx_in_block].dindirect[i] = 0;
        }
        newinode[inode_idx_in_block].flags = 0;
        newinode[inode_idx_in_block].size = 0;

        cache_release_block(fcache, dentry_ptr, 1);
        cache_release_block(fcache, newinodeptr, 1);
        cache_flush(fcache);
        return 0;
    }
    else if (total_dentries < DIRECT_DENTRIES + INDIRECT_DENTRIES)
    {
        uint32_t indirect_block_idx = total_dentries - DIRECT_DENTRIES;
        if (root_inode->indirect == 0)
        {
            root_inode->indirect = find_open_block();
        }
        uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->indirect;
        int dentryidx = total_dentries % DENTRY_PER_BLOCK;

        void *indirect_ptr;
        uint32_t *indirect_block;
        get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
        indirect_block = (uint32_t *)indirect_ptr;

        if (get < 0)
        {
            return get;
        }

        uint32_t direct_blk_idx = indirect_block_idx / DENTRY_PER_BLOCK;
        if (indirect_block[direct_blk_idx] == 0)
        {
            indirect_block[direct_blk_idx] = find_open_block();
        }
        uint32_t direct_blk = indirect_block[direct_blk_idx];

        uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

        void *block_ptr;
        struct ktfs_dir_entry *dentries;
        get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
        dentries = (struct ktfs_dir_entry *)block_ptr;

        if (get < 0)
        {
            return get;
        }

        for (int j = 0; j < KTFS_MAX_FILENAME_LEN + sizeof(uint8_t); j++)
        {
            dentries[dentryidx].name[j] = name[j];
            if (name[j] == '\0')
            {
                break;
            }
        }
        dentries[dentryidx].inode = find_open_inode();
        root_inode->size += sizeof(struct ktfs_dir_entry);
        cache_release_block(fcache, inode_ptr, 1);
        uint32_t inode_idx_in_block = dentries[dentryidx].inode % inodes_per_blk;
        struct ktfs_inode *newinode;
        void *newinodeptr;
        cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + (dentries[dentryidx].inode / inodes_per_blk)), &newinodeptr);
        newinode = (struct ktfs_inode *)newinodeptr;
        newinode[inode_idx_in_block].size = 0;
        for (int i = 0; i < 3; i++)
        {
            newinode[inode_idx_in_block].block[i] = 0;
        }
        for (int i = 0; i < 2; i++)
        {
            newinode[inode_idx_in_block].dindirect[i] = 0;
        }
        newinode[inode_idx_in_block].flags = 0;
        newinode[inode_idx_in_block].size = 0;

        cache_release_block(fcache, dentries, 1);
        cache_release_block(fcache, newinodeptr, 1);
        cache_release_block(fcache, indirect_ptr, 1);
        cache_release_block(fcache, block_ptr, 1);
        cache_flush(fcache);
        return 0;
    }
    else
    {
        uint32_t dindirect_block_idx = total_dentries - (DIRECT_BLOCKS + INDIRECT_BLOCKS);
        uint32_t dindirect_oblock_idx = dindirect_block_idx / (INDIRECT_BLOCKS * INDIRECT_BLOCKS);
        uint32_t dindirect_iblock_idx = (dindirect_block_idx % (INDIRECT_BLOCKS * INDIRECT_BLOCKS)) / INDIRECT_BLOCKS;

        uint32_t dindirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->dindirect[dindirect_oblock_idx];

        void *dindirect_ptr;
        uint32_t *dindirect_block;
        get = cache_get_block(fcache, KTFS_BLKSZ * dindirect_data_blk, &dindirect_ptr);
        dindirect_block = (uint32_t *)dindirect_ptr;

        if (get < 0)
        {
            // cache_release_block(fcache, dindirect_block, 0);
            return get;
        }
        uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect_block[dindirect_iblock_idx];
        uint32_t indirect_block_idx = dindirect_block_idx % INDIRECT_DENTRIES;
        int dentryidx = total_dentries % DENTRY_PER_BLOCK;

        void *indirect_ptr;
        uint32_t *indirect_block;
        get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
        indirect_block = (uint32_t *)indirect_ptr;

        if (get < 0)
        {
            return get;
        }

        uint32_t direct_blk_idx = indirect_block_idx / DENTRY_PER_BLOCK;
        uint32_t direct_blk = indirect_block[direct_blk_idx];
        uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

        void *block_ptr;
        struct ktfs_dir_entry *dentries;
        get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
        dentries = (struct ktfs_dir_entry *)block_ptr;

        if (get < 0)
        {
            return get;
        }

        for (int j = 0; j < KTFS_MAX_FILENAME_LEN + sizeof(uint8_t); j++)
        {
            dentries[dentryidx].name[j] = name[j];
            if (name[j] == '\0')
            {
                break;
            }
        }

        dentries[dentryidx].inode = find_open_inode();
        root_inode->size += sizeof(struct ktfs_dir_entry);
        cache_release_block(fcache, inode_blk, 1);
        uint32_t inode_idx_in_block = dentries[dentryidx].inode % inodes_per_blk;
        struct ktfs_inode *newinode;
        void *newinodeptr;
        cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + (dentries[dentryidx].inode / inodes_per_blk)), &newinodeptr);
        newinode = (struct ktfs_inode *)newinodeptr;
        newinode[inode_idx_in_block].size = 0;
        for (int i = 0; i < 3; i++)
        {
            newinode[inode_idx_in_block].block[i] = 0;
        }
        for (int i = 0; i < 2; i++)
        {
            newinode[inode_idx_in_block].dindirect[i] = 0;
        }
        newinode[inode_idx_in_block].flags = 0;
        newinode[inode_idx_in_block].size = 0;

        cache_release_block(fcache, dentries, 1);
        cache_release_block(fcache, newinode, 1);
        cache_release_block(fcache, indirect_ptr, 1);
        cache_release_block(fcache, block_ptr, 1);
        cache_release_block(fcache, dindirect_ptr, 1);
        cache_flush(fcache);
        return 0;
    }

    return -ENOENT;
    // add dentry and inode for file of length 0
    // change must immediatly reflect on disk
    // do not open file
    // update bitmap
}

int ktfs_delete(const char *name)
{
    // close file if open
    struct ktfs_file *file = is_file_open(name);
    if (file != NULL)
    {
        remove_file(file);
        ktfs_close(&file->io);
    }

    // struct ktfs_dir_entry *dentry = get_file_dentry(name);
    struct ktfs_dir_entry *dentry;

    void *inode_ptr;
    struct ktfs_inode *inode_blk;
    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;

    int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    if (get < 0)
    {
        return get;
    }

    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];

    // calculate dentry counts
    uint32_t total_dentries = root_inode->size / sizeof(struct ktfs_dir_entry);
    uint32_t dentries_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    uint32_t entries_scanned = 0;
    void *dentry_ptr;
    struct ktfs_dir_entry *dentries;
    int dentryblk = -1;
    int dentryidx = -1;
    // for direct blocks
    for (int i = 0; i < 3 && entries_scanned < total_dentries; i++)
    {
        uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[i];

        get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &dentry_ptr);
        dentries = (struct ktfs_dir_entry *)dentry_ptr;
        // if (get < 0)
        // {
        //     return get;
        // }
        for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
        {
            if (strncmp(name, dentries[j].name, KTFS_MAX_FILENAME_LEN) == 0)
            {
                dentry = dentries + j;
                dentryblk = i;
                dentryidx = j;
                break;
            }
        }
        if (dentryblk != -1 || dentryidx != -1)
        {
            break;
        }
        cache_release_block(fcache, dentry_ptr, 0);
    }
    if (dentryblk == -1 || dentryidx == -1)
    {
        return -EINVAL;
    }
    root_inode->size -= sizeof(struct ktfs_dir_entry);
    cache_release_block(fcache, inode_ptr, 1);
    // cache_release_block(fcache, dentries, 0);
    // free all data blocks and inode of file
    uint32_t inode_idx = dentry->inode;
    inode_blk_num = 1 + system->bitmap_blk_cnt + (inode_idx / inodes_per_blk);
    uint32_t inode_idx_in_blk = inode_idx % inodes_per_blk;

    if (inode_idx == system->superblock.root_directory_inode)
    {
        return -EINVAL;
    }
    // struct ktfs_inode *inode_blk;
    // void *inode_ptr;
    get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    struct ktfs_inode *inode = &inode_blk[inode_idx_in_blk];
    for (int i = 0; i < DIRECT_BLOCKS; i++)
    {
        if (inode->block[i])
        {
            freeblock(inode->block[i]);
            inode->block[i] = 0;
        }
    }

    if (inode->indirect)
    {
        void *block_ptr;
        cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + system->inode_blk_cnt + inode->indirect), &block_ptr);
        uint32_t *indirect_block = block_ptr;
        for (int i = 0; i < INDIRECT_BLOCKS; i++)
        {
            if (indirect_block[i])
            {
                freeblock(indirect_block[i]);
            }
        }
        cache_release_block(fcache, block_ptr, 0);
        if (inode->indirect)
        {
            freeblock(inode->indirect);
        }
        inode->indirect = 0;
    }
    // KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + system->inode_blk_cnt + inode->indirect)
    for (int d = 0; d < 2; d++)
    {
        uint32_t dindirect_blk = inode->dindirect[d];
        if (!dindirect_blk)
            continue;

        // load the doubly-indirect block (array of INDIRECT_BLOCKS pointers)
        void *dindirect_ptr;
        cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect_blk), &dindirect_ptr);
        uint32_t *dindirect = dindirect_ptr;

        for (int i = 0; i < INDIRECT_BLOCKS; i++)
        {
            uint32_t indirect_blk = dindirect[i];
            if (!indirect_blk)
                continue;

            // for each indirect block, load it and free its data blocks
            void *indirect_ptr;
            cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_blk), &indirect_ptr);
            uint32_t *indirect = indirect_ptr;
            for (int j = 0; j < INDIRECT_BLOCKS; j++)
            {
                if (indirect[j])
                {
                    freeblock(indirect[j]);
                }
            }
            cache_release_block(fcache, indirect_ptr, 0);
            // then free that indirect block
            if (indirect_blk)
            {
                freeblock(indirect_blk);
            }
        }

        cache_release_block(fcache, dindirect_ptr, 0);
        // finally free the doubly-indirect block itself
        if (dindirect_blk)
        {
            freeblock(dindirect_blk);
        }
        inode->dindirect[d] = 0;
    }

    memset(inode, 0, sizeof(struct ktfs_inode));
    // remove dentry associated with file from root directory
    struct ktfs_dir_entry del;
    for (uint32_t i = (DENTRY_PER_BLOCK * dentryblk + dentryidx) + 1; i < total_dentries; i++)
    {
        read_dir_entry(i, &del);
        write_dir_entry(i - 1, &del);
    }

    memset(&del, 0, sizeof(del));
    write_dir_entry(total_dentries - 1, &del);
    // changes must immediately reflect on disk
    // update bitmap
    cache_release_block(fcache, inode_ptr, 1);
    cache_release_block(fcache, dentry_ptr, 1);
    ktfs_flush();
    return 0;
}

int ktfs_cntl(struct io *io, int cmd, void *arg)
{
    if (io == NULL) // Null check
    {
        // panic("Bad arguments in to ktfs_cntl");
        return -EINVAL;
    }

    if (cmd == IOCTL_GETBLKSZ) // Command
    {
        return 1;
    }
    else if (cmd == IOCTL_GETEND)
    {
        if (arg == NULL)
        {
            return -EINVAL;
        }
        struct ktfs_file *fio = (void *)io - offsetof(struct ktfs_file, io);
        unsigned long long *newarg = arg;
        // struct ktfs_file *file = (struct ktfs_file *)arg;
        // struct ktfs_inode *inode;
        // inode = fio->dentry->inode;
        uint32_t inode_idx = fio->dentry->inode;
        uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
        uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (inode_idx / inodes_per_blk);
        uint32_t inode_idx_in_blk = inode_idx % inodes_per_blk;

        // struct ktfs_inode *inode;
        struct ktfs_inode *inode_blk;
        void *inode_ptr;
        cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
        inode_blk = (struct ktfs_inode *)inode_ptr;
        struct ktfs_inode *inode = &inode_blk[inode_idx_in_blk];
        *newarg = inode->size;
        cache_release_block(fcache, inode_ptr, 0);
        return 0;
    }
    else if (cmd == IOCTL_SETEND)
    {
        // extend length of file and add additional data blocks as necessary
        if (arg == NULL)
        {
            return -EINVAL;
        }
        uint64_t new_size = *(uint64_t *)arg;
        // uintptr_t new_size = (uintptr_t)arg;
        struct ktfs_file *fio = (void *)io - offsetof(struct ktfs_file, io);
        // // struct ktfs_file *file = (struct ktfs_file *)arg;
        // // struct ktfs_inode *inode;
        // // inode = fio->dentry->inode;
        uint32_t inode_idx = fio->dentry->inode;
        uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
        uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (inode_idx / inodes_per_blk);
        uint32_t inode_idx_in_blk = inode_idx % inodes_per_blk;

        struct ktfs_inode *inode_blk;
        void *inode_ptr;
        int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
        inode_blk = (struct ktfs_inode *)inode_ptr;
        struct ktfs_inode *inode = &inode_blk[inode_idx_in_blk];

        // struct ktfs_inode *inode;
        // void *inode_ptr;
        // cache_get_block(fcache, KTFS_BLKSZ * (1 + system->bitmap_blk_cnt + fio->dentry->inode), &inode_ptr);
        // inode = inode_ptr;

        uint32_t max_file_size = DIRECT_CAPACITY + INDIRECT_CAPACITY + (2 * (INDIRECT_BLOCKS * INDIRECT_CAPACITY));

        if (new_size > max_file_size)
        {
            return -EINVAL;
        }

        inode->size = new_size;
        fio->file_size = new_size;
        // int get;
        int f_block_idx = inode->size / KTFS_BLKSZ;

        if (f_block_idx < DIRECT_BLOCKS)
        {
            for (int i = 0; i < f_block_idx + 1; i++)
            {
                if (inode->block[i] == 0)
                {
                    inode->block[i] = find_open_block();
                }
            }
        }
        else if (f_block_idx < DIRECT_BLOCKS + INDIRECT_BLOCKS)
        {
            f_block_idx -= DIRECT_BLOCKS;
            for (int i = 0; i < 3; i++)
            {
                if (inode->block[i] == 0)
                {
                    inode->block[i] = find_open_block();
                }
            }
            if (inode->indirect == 0)
            {
                inode->indirect = find_open_block();
            }

            uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + inode->indirect;
            void *indirect_ptr;
            uint32_t *indirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
            indirect_block = (uint32_t *)indirect_ptr;

            for (int i = 0; i < f_block_idx + 1; i++)
            {
                if (indirect_block[i] == 0)
                {
                    indirect_block[i] = find_open_block();
                }
            }

            cache_release_block(fcache, indirect_ptr, 1);
        }
        else
        {
            f_block_idx -= DIRECT_BLOCKS + INDIRECT_BLOCKS;
            for (int i = 0; i < 3; i++)
            {
                if (inode->block[i] == 0)
                {
                    inode->block[i] = find_open_block();
                }
            }
            if (inode->indirect == 0)
            {
                inode->indirect = find_open_block();
            }

            uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + inode->indirect;
            void *indirect_ptr;
            uint32_t *indirect_block;
            get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
            indirect_block = (uint32_t *)indirect_ptr;

            for (int i = 0; i < INDIRECT_BLOCKS; i++)
            {
                if (indirect_block[i] == 0)
                {
                    indirect_block[i] = find_open_block();
                }
            }

            cache_release_block(fcache, indirect_ptr, 1);

            uint32_t dindirect_oblock_idx = f_block_idx / (INDIRECT_BLOCKS * INDIRECT_BLOCKS); // uint32_t dindirect_iblock_idx = f_block_idx % INDIRECT_BLOCKS;
            uint32_t dindirect_iblock_idx = (f_block_idx % (INDIRECT_BLOCKS * INDIRECT_BLOCKS)) / INDIRECT_BLOCKS;

            for (int i = 0; i < dindirect_oblock_idx + 1; i++)
            {
                if (inode->dindirect[i] == 0)
                {
                    inode->dindirect[i] = find_open_block();
                }

                uint32_t dindirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + inode->dindirect[i];

                void *dindirect_ptr;
                uint32_t *dindirect_block;
                get = cache_get_block(fcache, KTFS_BLKSZ * dindirect_data_blk, &dindirect_ptr);
                dindirect_block = (uint32_t *)dindirect_ptr;

                if (get < 0)
                {
                    cache_release_block(fcache, dindirect_block, 0);
                    return get;
                }

                for (int j = 0; j < dindirect_iblock_idx + 1; j++)
                {
                    if (dindirect_block[j] == 0)
                    {
                        dindirect_block[j] = find_open_block();
                    }

                    uint32_t indirect_data_blk = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + dindirect_block[j];

                    uint32_t indirect_block_idx = f_block_idx % INDIRECT_BLOCKS;

                    void *indirect_ptr;
                    uint32_t *indirect_block;
                    get = cache_get_block(fcache, KTFS_BLKSZ * indirect_data_blk, &indirect_ptr);
                    indirect_block = (uint32_t *)indirect_ptr;

                    if (get < 0)
                    {
                        cache_release_block(fcache, indirect_ptr, 0);
                        cache_release_block(fcache, dindirect_ptr, 0);
                        return get;
                    }
                    for (int k = 0; k < indirect_block_idx + 1; k++)
                    {
                        if (indirect_block[k] == 0)
                        {
                            indirect_block[k] = find_open_block();
                        }
                    }
                    cache_release_block(fcache, indirect_ptr, 1);
                    cache_release_block(fcache, dindirect_ptr, 1);
                    // uint32_t block_pos = (pos + bytes_read) % KTFS_BLKSZ;
                    // uint32_t direct_blk = indirect_block[indirect_block_idx];

                    // uint32_t block_idx = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + direct_blk;

                    // void *block_ptr;
                    // struct ktfs_data_block *block;
                    // get = cache_get_block(fcache, KTFS_BLKSZ * block_idx, &block_ptr);
                    // block = (struct ktfs_data_block *)block_ptr;

                    // if (get < 0)
                    // {
                    //     cache_release_block(fcache, block, 0);
                    //     return get;
                    // }
                }
            }
        }

        cache_release_block(fcache, inode_ptr, 1);
        ktfs_flush();
        return 0;
    }
    else if (cmd == IOCTL_GETPOS)
    {
        return 0;
    }
    else if (cmd == IOCTL_SETPOS)
    {
        return 0;
    }
    else
    {
        return -ENOTSUP;
    }

    return -ENOTSUP;
}

int ktfs_flush(void)
{
    return cache_flush(fcache);
}

int file_exists(const char *name)
{
    // root directory indexing
    void *inode_ptr;
    struct ktfs_inode *inode_blk;
    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;

    int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    if (get < 0)
    {
        return get;
    }

    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];

    // calculate dentry counts
    uint32_t total_dentries = root_inode->size / sizeof(struct ktfs_dir_entry);
    uint32_t dentries_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    uint32_t entries_scanned = 0;
    cache_release_block(fcache, inode_ptr, 0);
    // for direct blocks
    for (int i = 0; i < 3 && entries_scanned < total_dentries; i++)
    {
        uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[i];
        void *dentry_ptr;
        struct ktfs_dir_entry *dentries;
        get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &dentry_ptr);
        dentries = (struct ktfs_dir_entry *)dentry_ptr;
        if (get < 0)
        {
            return get;
        }
        for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
        {
            if (strncmp(name, dentries[j].name, KTFS_MAX_FILENAME_LEN) == 0)
            {
                return entries_scanned;
            }
        }
        cache_release_block(fcache, dentry_ptr, 0);
    }

    // for indirect blocks
    if (root_inode->indirect != 0 && entries_scanned < total_dentries)
    {
        uint32_t indirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->indirect;
        void *indirect_ptr;
        get = cache_get_block(fcache, KTFS_BLKSZ * indirect_blk_pos, &indirect_ptr);
        if (get < 0)
            return get;
        uint32_t *indirect_blk = (uint32_t *)indirect_ptr;
        int num_indirect = KTFS_BLKSZ / sizeof(uint32_t);
        for (int i = 0; i < num_indirect && entries_scanned < total_dentries; i++)
        {
            uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_blk[i];
            void *data_blk;
            get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &data_blk);
            if (get < 0)
                return get;
            struct ktfs_dir_entry *dentries = (struct ktfs_dir_entry *)data_blk;

            for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
            {
                if (strncmp(name, dentries[j].name, KTFS_MAX_FILENAME_LEN) == 0)
                {
                    return entries_scanned;
                }
            }
            cache_release_block(fcache, dentries, 0);
        }

        cache_release_block(fcache, indirect_blk, 0);
    }

    // repeat for dindirect block
    if ((root_inode->dindirect[0] || root_inode->dindirect[1]) && entries_scanned < total_dentries)
    {
        int num_dindirect = 2;
        int num_indirect_per_blk = KTFS_BLKSZ / sizeof(uint32_t);
        for (int d = 0; d < num_dindirect && entries_scanned < total_dentries; d++)
        {
            uint32_t dindirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->dindirect[d];
            void *dindirect_blk;
            get = cache_get_block(fcache, KTFS_BLKSZ * dindirect_blk_pos, &dindirect_blk);
            if (get < 0)
                return get;
            uint32_t *dindirect_blk_nums = (uint32_t *)dindirect_blk;

            for (int k = 0; k < num_indirect_per_blk && entries_scanned < total_dentries; k++)
            {
                uint32_t indirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->indirect;
                void *indirect_ptr;
                get = cache_get_block(fcache, KTFS_BLKSZ * indirect_blk_pos, &indirect_ptr);
                if (get < 0)
                    return get;
                uint32_t *indirect_blk = (uint32_t *)indirect_ptr;

                for (int j = 0; j < num_indirect_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
                {
                    uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_blk[j];
                    void *data_blk;
                    struct ktfs_dir_entry *dentries;
                    get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &data_blk);
                    dentries = (struct ktfs_dir_entry *)data_blk;
                    if (get < 0)
                        return get;

                    for (int m = 0; m < dentries_per_blk && entries_scanned < total_dentries; m++, entries_scanned++)
                    {
                        if (strncmp(name, dentries[m].name, KTFS_MAX_FILENAME_LEN) == 0)
                        {
                            return entries_scanned;
                        }
                    }
                    cache_release_block(fcache, dentries, 0);
                }
                cache_release_block(fcache, indirect_blk, 0);
            }
            cache_release_block(fcache, dindirect_blk_nums, 0);
        }
    }
    return 0;
}

uint16_t find_open_inode(void)
{
    void *inode_ptr;
    struct ktfs_inode *inode_blk;
    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;

    int get = cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    if (get < 0)
    {
        return get;
    }

    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];
    cache_release_block(fcache, inode_blk, 0);

    uint32_t total_inodes = system->inode_blk_cnt * inodes_per_blk;
    int *used_inodes = kcalloc(total_inodes, sizeof *used_inodes);
    // int used_inodes[112] = {0};

    // calculate dentry counts
    uint32_t total_dentries = root_inode->size / sizeof(struct ktfs_dir_entry);
    uint32_t dentries_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    uint32_t entries_scanned = 0;

    // for direct blocks
    for (int i = 0; i < 3 && entries_scanned < total_dentries; i++)
    {
        uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[i];
        void *dentry_ptr;
        struct ktfs_dir_entry *dentries;
        get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &dentry_ptr);
        dentries = (struct ktfs_dir_entry *)dentry_ptr;
        if (get < 0)
        {
            return get;
        }
        for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
        {
            used_inodes[dentries[j].inode] = 1;
        }
        cache_release_block(fcache, dentries, 0);
    }

    // for indirect blocks
    if (root_inode->indirect != 0 && entries_scanned < total_dentries)
    {
        uint32_t indirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->indirect;
        void *indirect_ptr;
        get = cache_get_block(fcache, KTFS_BLKSZ * indirect_blk_pos, &indirect_ptr);
        if (get < 0)
            return get;
        uint32_t *indirect_blk = (uint32_t *)indirect_ptr;
        int num_indirect = KTFS_BLKSZ / sizeof(uint32_t);
        for (int i = 0; i < num_indirect && entries_scanned < total_dentries; i++)
        {
            uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_blk[i];
            void *data_blk;
            get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &data_blk);
            if (get < 0)
            {
                cache_release_block(fcache, indirect_ptr, 0);
                cache_release_block(fcache, data_blk, 0);
                return get;
            }
            struct ktfs_dir_entry *dentries = (struct ktfs_dir_entry *)data_blk;

            for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
            {
                used_inodes[dentries[j].inode] = 1;
            }
            cache_release_block(fcache, dentries, 0);
        }

        cache_release_block(fcache, indirect_blk, 0);
    }

    // repeat for dindirect block
    if ((root_inode->dindirect[0] || root_inode->dindirect[1]) && entries_scanned < total_dentries)
    {
        int num_dindirect = 2;
        int num_indirect_per_blk = KTFS_BLKSZ / sizeof(uint32_t);
        for (int d = 0; d < num_dindirect && entries_scanned < total_dentries; d++)
        {
            uint32_t dindirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->dindirect[d];
            void *dindirect_blk;
            get = cache_get_block(fcache, KTFS_BLKSZ * dindirect_blk_pos, &dindirect_blk);
            if (get < 0)
                return get;
            uint32_t *dindirect_blk_nums = (uint32_t *)dindirect_blk;

            for (int k = 0; k < num_indirect_per_blk && entries_scanned < total_dentries; k++)
            {
                uint32_t indirect_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->indirect;
                void *indirect_ptr;
                get = cache_get_block(fcache, KTFS_BLKSZ * indirect_blk_pos, &indirect_ptr);
                if (get < 0)
                    return get;
                uint32_t *indirect_blk = (uint32_t *)indirect_ptr;

                for (int j = 0; j < num_indirect_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
                {
                    uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + indirect_blk[j];
                    void *data_blk;
                    struct ktfs_dir_entry *dentries;
                    get = cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &data_blk);
                    dentries = (struct ktfs_dir_entry *)data_blk;
                    if (get < 0)
                        return get;

                    for (int m = 0; m < dentries_per_blk && entries_scanned < total_dentries; m++, entries_scanned++)
                    {
                        used_inodes[dentries[m].inode] = 1;
                    }
                    cache_release_block(fcache, data_blk, 0);
                }
                cache_release_block(fcache, indirect_blk, 0);
            }
            cache_release_block(fcache, dindirect_blk_nums, 0);
        }
    }

    uint32_t free_inode = -EINVAL;
    for (int i = 1; i < system->inode_blk_cnt * inodes_per_blk; i++)
    {
        if (used_inodes[i] == 0)
        {
            free_inode = i;
            break;
        }
    }
    kfree(used_inodes);
    return free_inode;
}

uint32_t find_open_block(void)
{
    uint32_t bitmaps = system->superblock.bitmap_block_count;
    int freeblock = 0;
    for (int i = 0; i < bitmaps; i++)
    {
        void *bitmap_ptr;
        struct ktfs_data_block *bitmap;
        int get = cache_get_block(fcache, KTFS_BLKSZ * (1 + i), &bitmap_ptr);
        bitmap = (struct ktfs_data_block *)bitmap_ptr;
        if (get < 0)
        {
            cache_release_block(fcache, bitmap_ptr, 0);
            return get;
        }

        for (size_t byte_index = 0; byte_index < KTFS_BLKSZ; byte_index++)
        {
            uint8_t byte = bitmap->data[byte_index];
            for (int bit_index = 7; bit_index >= 0; bit_index--)
            {
                int bit = (byte >> bit_index) & 1;
                if (bit == 0)
                {
                    freeblock = byte_index * 8 + (7 - bit_index);
                    if (freeblock == 0)
                    {
                        return -ENOENT;
                    }
                    bitmap->data[byte_index] |= (1 << bit_index);
                    cache_release_block(fcache, bitmap_ptr, 1);
                    return freeblock;
                }
            }
        }
    }

    return -ENOENT;
}

struct ktfs_file *is_file_open(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }

    struct ktfs_file *cur = system->head;
    while (cur != NULL)
    {
        if (strcmp(cur->dentry->name, name) == 0)
        {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

void insert_file(struct ktfs_file *file)
{
    file->next = NULL;

    if (system->head == NULL)
    {
        system->head = file;
    }
    else
    {
        struct ktfs_file *cur = system->head;
        while (cur->next)
        {
            cur = cur->next;
        }
        cur->next = file;
    }
}

void remove_file(struct ktfs_file *file)
{
    if (system->head == NULL)
        return;

    if (system->head == file)
    {
        system->head = file->next;
        file->next = NULL;
        return;
    }

    struct ktfs_file *cur = system->head;
    while (cur->next && cur->next != file)
    {
        cur = cur->next;
    }
    if (cur->next == file)
    {
        cur->next = file->next;
        file->next = NULL;
    }
}

struct ktfs_dir_entry *get_file_dentry(const char *name)
{
    // root directory indexing
    void *inode_ptr;
    struct ktfs_inode *inode_blk;
    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;

    cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;

    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];
    cache_release_block(fcache, inode_blk, 0);

    // calculate dentry counts
    uint32_t total_dentries = root_inode->size / sizeof(struct ktfs_dir_entry);
    uint32_t dentries_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_dir_entry);
    uint32_t entries_scanned = 0;

    // for direct blocks
    for (int i = 0; i < 3 && entries_scanned < total_dentries; i++)
    {
        uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[i];
        void *dentry_ptr;
        struct ktfs_dir_entry *dentries;
        cache_get_block(fcache, KTFS_BLKSZ * data_blk_pos, &dentry_ptr);
        dentries = (struct ktfs_dir_entry *)dentry_ptr;
        // if (get < 0)
        // {
        //     return get;
        // }
        for (int j = 0; j < dentries_per_blk && entries_scanned < total_dentries; j++, entries_scanned++)
        {
            if (strncmp(name, dentries[j].name, KTFS_MAX_FILENAME_LEN) == 0)
            {
                return dentries + j;
            }
        }
        cache_release_block(fcache, dentries, 0);
    }
    return 0;
}

int freeblock(uint32_t blocknum)
{
    if (blocknum >= system->data_blk_cnt)
    {
        return -EINVAL;
    }

    const uint32_t bits_per_block = KTFS_BLKSZ * 8;
    uint32_t bitmap_blk_idx = blocknum / bits_per_block;
    uint32_t bit = blocknum % bits_per_block;
    uint32_t bitmap_blk_pos = 1 + bitmap_blk_idx;

    void *bitmap_ptr;
    int get = cache_get_block(fcache, bitmap_blk_pos * KTFS_BLKSZ, &bitmap_ptr);
    if (get < 0)
    {
        return get;
    }
    struct ktfs_data_block *bitmap = bitmap_ptr;

    uint32_t byte_idx = bit / 8;
    uint32_t bit_idx = bit % 8;
    bitmap->data[byte_idx] &= ~(1 << bit_idx);

    cache_release_block(fcache, bitmap_ptr, 1);
    uint32_t data_blk_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + blocknum;
    void *block_ptr;
    get = cache_get_block(fcache, data_blk_pos * KTFS_BLKSZ, &block_ptr);
    if (get < 0)
    {
        return get;
    }

    memset(block_ptr, 0, KTFS_BLKSZ);
    cache_release_block(fcache, block_ptr, 1);

    return 0;
}

void read_dir_entry(uint32_t idx, struct ktfs_dir_entry *out)
{
    void *inode_ptr;
    struct ktfs_inode *inode_blk;
    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;
    cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];

    uint32_t blk_num = idx / DENTRY_PER_BLOCK;
    uint32_t block_idx = idx % DENTRY_PER_BLOCK;
    uint32_t block_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[blk_num];
    void *block_ptr;
    cache_get_block(fcache, block_pos * KTFS_BLKSZ, &block_ptr);
    *out = ((struct ktfs_dir_entry *)block_ptr)[block_idx];
    cache_release_block(fcache, block_ptr, 0);
    cache_release_block(fcache, inode_blk, 0);
}

void write_dir_entry(uint32_t idx, struct ktfs_dir_entry *in)
{
    void *inode_ptr;
    struct ktfs_inode *inode_blk;
    uint32_t root_idx = system->superblock.root_directory_inode;
    uint32_t inodes_per_blk = KTFS_BLKSZ / sizeof(struct ktfs_inode);
    uint32_t inode_blk_num = 1 + system->bitmap_blk_cnt + (root_idx / inodes_per_blk);
    uint32_t root_idx_in_blk = root_idx % inodes_per_blk;
    cache_get_block(fcache, KTFS_BLKSZ * inode_blk_num, &inode_ptr);
    inode_blk = (struct ktfs_inode *)inode_ptr;
    struct ktfs_inode *root_inode = &inode_blk[root_idx_in_blk];

    uint32_t blk_num = idx / DENTRY_PER_BLOCK;
    uint32_t block_idx = idx % DENTRY_PER_BLOCK;
    uint32_t block_pos = 1 + system->bitmap_blk_cnt + system->inode_blk_cnt + root_inode->block[blk_num];

    void *block_ptr;
    cache_get_block(fcache, block_pos * KTFS_BLKSZ, &block_ptr);
    ((struct ktfs_dir_entry *)block_ptr)[block_idx] = *in;
    cache_release_block(fcache, block_ptr, 1);
    cache_release_block(fcache, inode_blk, 0);
}