/*! @file memory.c
    @brief Physical and virtual memory manager    
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "riscv.h"
#include "heap.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "thread.h"
// #include "process.h"
#include "error.h"
#include "timer.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE) // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE) // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk * next; ///< Next page in list
    unsigned long pagecnt; ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags:8;
    uint64_t rsw:2;
    uint64_t ppn:44;
    uint64_t reserved:7;
    uint64_t pbmt:2;
    uint64_t n:1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2*9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1*9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0*9)) % PTE_CNT)

#define MIN(a,b) (((a)<(b))?(a):(b))

#define ROUND_UP(n,k) (((n)+(k)-1)/(k)*(k)) 
#define ROUND_DOWN(n,k) ((n)/(k)*(k))

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) \
                             >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//



static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte * root, unsigned int asid);
static inline struct pte * mtag_to_ptab(mtag_t mtag);
static inline struct pte * active_space_ptab(void);

static inline void * pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void * p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk * free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
// 

void memory_init(void) {
    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;
    
    void * heap_start;
    void * heap_end;

    uintptr_t pma;
    const void * pp;

    trace("%s()", __func__);

    assert (RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
        RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)
    
    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic(NULL);

    // Initialize main page table with the following direct mapping:
    // 
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB
    
    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void*)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP (
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");
    
    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
        heap_start, heap_end, (heap_end - heap_start) / 1024);
    
    // TODO: Initialize the free chunk list here
    free_chunk_list = (struct page_chunk *)heap_end;
    free_chunk_list->next = NULL;
    free_chunk_list->pagecnt = (RAM_END - heap_end) / PAGE_SIZE;
    
    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
    // start_interrupter();
}

mtag_t active_mspace(void) {
    return active_space_mtag();
}

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;
    
    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

//verify that this works 
mtag_t clone_active_mspace(void) {
    struct pte *old_root = active_space_ptab();
    struct pte *new_root = alloc_phys_page();
    memset(new_root, 0, PAGE_SIZE);

    for (unsigned vpn2 = 0; vpn2 < PTE_CNT; vpn2++) {
        struct pte e2 = old_root[vpn2];
        if (!PTE_VALID(e2) || PTE_LEAF(e2) || PTE_GLOBAL(e2)) {
            new_root[vpn2] = e2;
            continue;
        }
        struct pte *old_l1 = (struct pte *)((uintptr_t)e2.ppn << PAGE_ORDER);
        struct pte *new_l1 = alloc_phys_page();
        memset(new_l1, 0, PAGE_SIZE);
        new_root[vpn2] = ptab_pte(new_l1, e2.flags & PTE_G);

        for (unsigned vpn1 = 0; vpn1 < PTE_CNT; vpn1++) {
            struct pte e1 = old_l1[vpn1];
            if (!PTE_VALID(e1) || PTE_LEAF(e1)) {
                new_l1[vpn1] = e1;
                continue;
            }

            // Otherwise it’s a level-0 subtable
            struct pte *old_l0 = (struct pte *)((uintptr_t)e1.ppn << PAGE_ORDER);
            struct pte *new_l0 = alloc_phys_page();
            memset(new_l0, 0, PAGE_SIZE);
            new_l1[vpn1] = ptab_pte(new_l0, e1.flags & PTE_G);

            for (unsigned vpn0 = 0; vpn0 < PTE_CNT; vpn0++) {
                struct pte leaf = old_l0[vpn0];
                if (PTE_VALID(leaf) && PTE_LEAF(leaf)) {
                    void *old_pp = pageptr(leaf.ppn);
                    void *new_pp = alloc_phys_page();
                    memcpy(new_pp, old_pp, PAGE_SIZE);

                    new_l0[vpn0] = leaf_pte(new_pp, leaf.flags & (PTE_R | PTE_W | PTE_X | PTE_U));
                }else{
                    new_l0[vpn0] = leaf;
                }
            }
        }
    }
    //might not need
    sfence_vma();
    return ptab_to_mtag(new_root, 0); 
}

void reset_active_mspace(void) {
    for (uintptr_t vma = UMEM_START_VMA; vma < UMEM_END_VMA; vma += PAGE_SIZE) {
        struct pte *pt2 = active_space_ptab();
        unsigned i2 = VPN2(vma);
        struct pte e2 = pt2[i2];
        if (!PTE_VALID(e2) || PTE_LEAF(e2)) {
            continue;
        }
        struct pte *pt1 = (struct pte *)((uintptr_t)e2.ppn << PAGE_ORDER);
        unsigned i1 = VPN1(vma);
        struct pte e1 = pt1[i1];
        if (!PTE_VALID(e1) || PTE_LEAF(e1)) {
            continue; 
        }
        struct pte *pt0 = (struct pte *)((uintptr_t)e1.ppn << PAGE_ORDER);
        unsigned i0 = VPN0(vma);
        struct pte leaf = pt0[i0];
        if (!(PTE_VALID(leaf) && PTE_LEAF(leaf) && (leaf.flags & PTE_U))) {
            continue;
        }

        void *pp = pageptr(leaf.ppn);
        free_phys_page(pp);
        pt0[i0] = null_pte();

        //flush TLB
        sfence_vma();
    }
}



mtag_t discard_active_mspace(void) {
    mtag_t prev = active_space_mtag();
    reset_active_mspace();
    switch_mspace(main_mtag);
    return prev; 
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

void * map_page(uintptr_t vma, void * pp, int rwxug_flags) {
    //must be a page size increment
    if (vma & (PAGE_SIZE - 1)) {
        //not sure if we should panic on this condition
        panic("map_page: vma is not page aligned");
    }

    // Obtain the level-2 root page table.
    struct pte *pt2 = active_space_ptab();
    unsigned int index2 = VPN2(vma);
    // If the level-2 entry is not valid, allocate a new level-1 page table.
    if (!PTE_VALID(pt2[index2])) {
        void *new_pt1 = alloc_phys_page();
        //allocation fail should be dealt with in alloc_phys_page
        // if (new_l1 == NULL)
        //     panic("map_page: failed to allocate level 1 page table");
        //need to set page memory to 0?
        memset(new_pt1, 0, PAGE_SIZE);
        pt2[index2] = ptab_pte((struct pte *)new_pt1, 0);
    }

    // Get level-1 page table pointer.
    struct pte *pt1 = (struct pte *)((uintptr_t)pt2[index2].ppn << PAGE_ORDER);
    unsigned int index1 = VPN1(vma);
    // If the level-1 entry is not valid, allocate a new level-0 page table.
    if (!PTE_VALID(pt1[index1])) {
        void *new_pt0 = alloc_phys_page();
        //allocation fail should be dealt with in alloc_phys_page
        // if (new_l0 == NULL)
        //     panic("map_page: failed to allocate level 0 page table");
        //need to set page memory to 0?
        memset(new_pt0, 0, PAGE_SIZE);
        pt1[index1] = ptab_pte((struct pte *)new_pt0, 0);
    }

    struct pte *pt0 = (struct pte *)((uintptr_t)pt1[index1].ppn << PAGE_ORDER);
    unsigned int index0 = VPN0(vma);

    pt0[index0] = leaf_pte(pp, rwxug_flags);

    //Flush TLB
    sfence_vma();

    return (void *)vma;
}

void * map_range(uintptr_t vma, size_t size, void * pp, int rwxug_flags) {
    //must be a page size increment
    if ((vma & (PAGE_SIZE - 1)) != 0 || ((uintptr_t)pp & (PAGE_SIZE - 1)) != 0) {
        panic("map_range: arguments must be page-aligned");
    }
    int i = 0;
    while(i<size){
        //if it failed to allocate panic would be through from alloc phys page
        map_page(vma+i, pp+i, rwxug_flags);
        i += PAGE_SIZE;
    }
    return (void*)vma; 
}

void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    if (vma & (PAGE_SIZE - 1)) {
        panic("alloc_and_map_range: vma is not page-aligned");
    }
    int rounded_size = ROUND_UP(size, PAGE_SIZE);
    int num_pages = rounded_size/PAGE_SIZE;
    //allocate all needed pages first
    void *pp = alloc_phys_pages(num_pages);
    void *ret = map_range(vma, rounded_size, pp, rwxug_flags);
    return ret; 
}

void set_range_flags(const void * vp, size_t size, int rwxug_flags) {
    uintptr_t vma = (uintptr_t)vp;
    if (vma & (PAGE_SIZE - 1)) {
        panic("set_range_flags: vma is not page-aligned");
    }
    int rounded_size = ROUND_UP(size, PAGE_SIZE);
    int num_pages = rounded_size/PAGE_SIZE;
    for (unsigned int i = 0; i < num_pages; i++) {
        uintptr_t page_vma = vma + i * PAGE_SIZE;

        struct pte *pt2 = active_space_ptab();
        unsigned idx2 = VPN2(page_vma);
        if (!PTE_VALID(pt2[idx2])) {
            //not sure if we should panic maybe just return
            return;
            panic("set_range_flags: level-2 entry invalid");
        }

        struct pte *pt1 = (struct pte *)((uintptr_t)pt2[idx2].ppn << PAGE_ORDER);
        unsigned idx1 = VPN1(page_vma);
        if (!PTE_VALID(pt1[idx1])) {
            //not sure if we should panic maybe just return
            return;
            panic("set_range_flags: level-1 entry invalid");
        }

        struct pte *pt0 = (struct pte *)((uintptr_t)pt1[idx1].ppn << PAGE_ORDER);
        unsigned idx0 = VPN0(page_vma);
        struct pte old = pt0[idx0];
        if (!PTE_VALID(old) || !PTE_LEAF(old)) {
            //not sure if we should panic maybe just return
            return;
            panic("set_range_flags: level-0 entry not a leaf or invalid");
        }

        //add new flags to lead entry
        void *pp = pageptr(old.ppn);
        pt0[idx0] = leaf_pte(pp, rwxug_flags);

        //Flush TLB
        sfence_vma();
    }
    return; 
}

void unmap_and_free_range(void * vp, size_t size) {
    uintptr_t vma = (uintptr_t)vp;
    if (vma & (PAGE_SIZE - 1)) {
        panic("set_range_flags: vma is not page-aligned");
    }
    int rounded_size = ROUND_UP(size, PAGE_SIZE);
    int num_pages = rounded_size/PAGE_SIZE;
    for (unsigned int i = 0; i < num_pages; i++) {
        uintptr_t page_vma = vma + i * PAGE_SIZE;

        struct pte *pt2 = active_space_ptab();
        unsigned idx2 = VPN2(page_vma);
        if (!PTE_VALID(pt2[idx2])) {
            //not sure if we should panic maybe just return
            return;
            panic("level‑2 entry invalid");
        }

        struct pte *pt1 = (struct pte *)((uintptr_t)pt2[idx2].ppn << PAGE_ORDER);
        unsigned idx1 = VPN1(page_vma);
        if (!PTE_VALID(pt1[idx1])) {
            //not sure if we should panic maybe just return
            return;
            panic("level‑1 entry invalid");
        }

        struct pte *pt0 = (struct pte *)((uintptr_t)pt1[idx1].ppn << PAGE_ORDER);
        unsigned idx0 = VPN0(page_vma);
        struct pte old = pt0[idx0];
        if (!PTE_VALID(old) || !PTE_LEAF(old)) {
            //not sure if we should panic maybe just return
            return;
            panic("level‑0 entry not a leaf");
        }

        void *pp = pageptr(old.ppn);
        free_phys_page(pp);

        pt0[idx0] = null_pte();

        //Flush TLB
        sfence_vma();
    }
    return; 
}

void * alloc_phys_page(void) {
    //allocates single page by passing in cnt of 1
    return alloc_phys_pages(1);
}

void free_phys_page(void * pp) {
    free_phys_pages(pp, 1);
    return; 
}

void * alloc_phys_pages(unsigned int cnt) {
    //have prev and curr pointers for chunk list iterating
    struct page_chunk * curr = free_chunk_list;
    struct page_chunk * prev = NULL;
    while(curr){
        //if current chunk can hold all pages we want to allocate
        if(curr->pagecnt >= cnt){
            //save chunk pointer and info to restore later
            void* ret = curr;
            struct page_chunk * prev_next =  curr->next;
            unsigned long prev_cnt = curr->pagecnt;
            //increment chunk pointer by size we are allocating
            struct page_chunk * old_curr = curr;
            curr = pageptr(pagenum(curr) + cnt);

            //edge case when we fill the entire chunk 
            if(prev_cnt - cnt == 0){
                //if this is the first chunk in the list then iterate the head pointer
                if(old_curr == free_chunk_list){
                    free_chunk_list = free_chunk_list->next;
                }
                //else just remove from the list
                else{
                    prev->next = prev_next;
                }
            //if chunk not filled just update chunk data at new address
            }else{
                //also update free_chunk_list if we updated first chunk
                if(old_curr == free_chunk_list){
                    free_chunk_list = curr;
                }else{
                    prev->next = curr;
                }
                curr->next = prev_next;
                curr->pagecnt = prev_cnt - cnt;
                
            }
            //return starting address of allocated space
            return ret;
        //else move to next chunk
        }else{
            prev = curr;
            curr = curr->next;
        }
    }
    //panic if no chunks are large enough *said in instructions doc*
    panic("No chunks large enough");
    return NULL;
}   

void free_phys_pages(void * pp, unsigned int cnt) {
    //create a temp pointer to current head 
    struct page_chunk * insert = free_chunk_list;
    //update head to be passed in pages pointer
    free_chunk_list = (struct page_chunk*) pp;
    //update struct attributes 
    free_chunk_list->next = insert;
    free_chunk_list->pagecnt = cnt;
    return;
}

unsigned long free_phys_page_count(void) {
    struct page_chunk * curr = free_chunk_list;
    unsigned int total = 0;
    while(curr){
        total += curr->pagecnt;
        curr = curr->next;
    }
    return total;
}

int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma) {
    if (vma < UMEM_START_VMA || vma >= UMEM_END_VMA) {
        return 0; 
    }
    //Round down to the start of the page
    uintptr_t page_vma = vma & ~(PAGE_SIZE - 1);

    //Allocate one physical page
    void *pp = alloc_phys_page();
    //panic should happen in alloc_phys_page call
    
    if (map_page(page_vma, pp, PTE_R | PTE_W | PTE_U) == NULL) {
        return 0;
    }

    //Flush TLB
    sfence_vma();

    return 1;  // handled successfully
}


mtag_t active_space_mtag(void) {
    return csrr_satp();
}

static inline mtag_t ptab_to_mtag(struct pte * ptab, unsigned int asid) {
    return (
        ((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
        ((unsigned long)asid << RISCV_SATP_ASID_shift) |
        pagenum(ptab) << RISCV_SATP_PPN_shift);
}

static inline struct pte * mtag_to_ptab(mtag_t mtag) {
    return (struct pte *)((mtag << 20) >> 8);
}

static inline struct pte * active_space_ptab(void) {
    return mtag_to_ptab(active_space_mtag());
}

static inline void * pageptr(uintptr_t n) {
    return (void*)(n << PAGE_ORDER);
}

static inline unsigned long pagenum(const void * p) {
    return (unsigned long)p >> PAGE_ORDER;
}

static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits+1));
}

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags) {
    return (struct pte) {
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pagenum(pp)
    };
}

static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag) {
    return (struct pte) {
        .flags = g_flag | PTE_V,
        .ppn = pagenum(pt)
    };
}


static inline struct pte null_pte(void) {
    return (struct pte) { };
}