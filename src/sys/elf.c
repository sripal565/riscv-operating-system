// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "assert.h"
#include "error.h"

#include <stdint.h>

// Offsets into e_ident

#define EI_CLASS        4   
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8   
#define EI_PAD          9  


// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE     0
#define EV_CURRENT  1

// ELF header e_type values

enum elf_et {
    ET_NONE = 0,
    ET_REL,
    ET_EXEC,
    ET_DYN,
    ET_CORE
};

struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff; 
    uint32_t e_flags; 
    uint16_t e_ehsize; 
    uint16_t e_phentsize; 
    uint16_t e_phnum; 
    uint16_t e_shentsize; 
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

enum elf_pt {
	PT_NULL = 0, 
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR,
	PT_TLS
};

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define  EM_RISCV   243

int elf_load(struct io * elfio, void (**eptr)(void)) {
    // FIX ME
    struct elf64_ehdr e_head;
    
    //change to just ioreadat
    int error = ioreadat(elfio, 0, &e_head, sizeof(e_head)); 
    if(error != sizeof(e_head)){
        return -EINVAL;
    }
    //check if all macros are defined properly
    if(e_head.e_ident[0] != 0x7f || e_head.e_ident[1] != 'E' || e_head.e_ident[2] != 'L' || e_head.e_ident[3] != 'F'){
        return -1;
    }
    //check if class is valid
    //do we only deal with elfclass64?
    if(e_head.e_ident[EI_CLASS] != ELFCLASS64){
        return -1;
    }
    //check for known data format
    //do we support MSB and LSB?
    if(e_head.e_ident[EI_DATA] != ELFDATA2LSB){
        return -1;
    }
    //check for valid version
    //is this not the same as e_version?
    if(e_head.e_ident[EI_VERSION] != EV_CURRENT){
        return -1;
    }
    //do we do anything with OSABI, ABIVERSION, or PAD?
    //do we only support executables?
    if(e_head.e_type != ET_EXEC){
        return -1;
    }
    //do we need to check machine
    if(e_head.e_machine != EM_RISCV){
        return -1;
    }
    //load in all program headers
    for(int i = 0; i<e_head.e_phnum; i++){
        struct elf64_phdr p_head; 
        //read the program header 
        int pherror = ioreadat(elfio, (e_head.e_phentsize * i) + e_head.e_phoff, &p_head, e_head.e_phentsize);
        if(pherror != e_head.e_phentsize){
            return -EINVAL;
        }
        //check if we can load in segment
        //do we account for other ptypes?
        if(p_head.p_type == PT_LOAD){
            //check if headers in valid mem range, should it continue or return error if one outside range is found?
            if(p_head.p_vaddr < 0x0C0000000UL || p_head.p_vaddr + p_head.p_memsz > 0x100000000UL){
                return -1;
            }
            int flags = PTE_U;
            if(p_head.p_flags & PF_R){
                flags |= PTE_R;
            }
            if(p_head.p_flags & PF_W){
                flags |= PTE_W;
            }
            if(p_head.p_flags & PF_X){
                flags |= PTE_X;
            }
            uintptr_t start_vma = p_head.p_vaddr & ~(PAGE_SIZE-1);
            uintptr_t end_vma   = ROUND_UP(p_head.p_vaddr + p_head.p_memsz, PAGE_SIZE);
            size_t    total_sz  = end_vma - start_vma;
            unsigned  num_pages = total_sz / PAGE_SIZE;

            void *pp_base = alloc_phys_pages(num_pages);

            map_range(start_vma, total_sz, pp_base, flags);

            int bytes_read = 0;
            uintptr_t page_offset = p_head.p_vaddr - start_vma;
            for (size_t done = 0; done < p_head.p_filesz; done += PAGE_SIZE) {
                
                // size_t chunk = p_head.p_filesz - done < PAGE_SIZE ? p_head.p_filesz - done : PAGE_SIZE;
                size_t chunk;
                if (p_head.p_filesz - done < PAGE_SIZE) {
                    chunk = p_head.p_filesz - done;
                } else {
                    chunk = PAGE_SIZE;
                }
        
                int rc = ioreadat(elfio,p_head.p_offset + done,pp_base + page_offset + done,chunk);
                bytes_read += rc;
                if (rc != (int)chunk){
                    return -EINVAL;
                }
            }
            size_t tail = total_sz - page_offset - p_head.p_filesz;
            if (tail > 0) {
                memset(pp_base + page_offset + p_head.p_filesz, 0, tail);
            }

        }
    }
    *eptr = (void (*)(void))e_head.e_entry;
    return 0;
}