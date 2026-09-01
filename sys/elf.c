// elf.c - ELF executable loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef ELF_TRACE
#define TRACE
#endif

#ifdef ELF_DEBUG
#define DEBUG
#endif

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "error.h"
#include "misc.h"

#include <stdint.h>

// Offsets into e_ident

#define EI_CLASS        4   
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8   
#define EI_PAD          9  


// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE     0
#define EV_CURRENT  1

// ELF header e_type values

enum elf_et {
    ET_NONE = 0,
    ET_REL,
    ET_EXEC,
    ET_DYN,
    ET_CORE
};

// The /elf64_ehdr/ structure.
// ELF header struct
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


// The /elf_pt/ enumeration.
// Program header p_type values
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

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

// The /elf64_phdr/ structure.
// Program header struct
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

// ELF header e_machine values (short list)

#define  EM_RISCV   243

int elf_load(struct io * io, void (**eptr)(void)) {
    // YOUR CODE HERE

    struct elf64_ehdr ehdr;
    struct elf64_phdr phdr;

    int res = iofetch(io, 0, &ehdr, sizeof(ehdr));
    if (res < 0) {
        return res;
    }

    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' ) {
        return -EBADFMT;
    }
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_ident[EI_DATA] != ELFDATA2LSB || ehdr.e_ident[EI_VERSION] != EV_CURRENT){
        return -EBADFMT;
    }
    if (ehdr.e_type != ET_EXEC){
        return -EBADFMT;
    }
    if (ehdr.e_machine != EM_RISCV){
        return -EBADFMT;
    }
    if (ehdr.e_version != EV_CURRENT){
        return -EBADFMT;
    }
    if (ehdr.e_ehsize != sizeof(ehdr) || ehdr.e_phentsize != sizeof(phdr) || ehdr.e_phnum == 0){
        return -EBADFMT;
    }
    if (ehdr.e_entry < UMEM_START_VMA || ehdr.e_entry >= UMEM_END_VMA){
        return -EBADFMT;
    }

    for (int i = 0; i < ehdr.e_phnum; i++){
        uint64_t offset = ehdr.e_phoff + (uint64_t)i*ehdr.e_phentsize;
        int res = iofetch(io, offset, &phdr, sizeof(phdr));
        if (res < 0) {
            return res;
        }

        if (phdr.p_type == PT_LOAD) {

            if (phdr.p_filesz > phdr.p_memsz){
                return -EBADFMT;
            }
            if (phdr.p_vaddr == 0 || phdr.p_vaddr < UMEM_START_VMA || phdr.p_vaddr + phdr.p_memsz < UMEM_START_VMA || phdr.p_vaddr >= UMEM_END_VMA || phdr.p_vaddr + phdr.p_memsz >= UMEM_END_VMA){
                return -EBADFMT;
            }
            if (phdr.p_memsz > 0){
                
                if (alloc_and_map_range(ROUND_DOWN(phdr.p_vaddr, PAGE_SIZE), ROUND_UP(phdr.p_memsz + phdr.p_vaddr, PAGE_SIZE) - ROUND_DOWN(phdr.p_vaddr, PAGE_SIZE), PTE_R | PTE_W | PTE_U) == NULL){
                    return -ENOMEM;
                }
                int res = iofetch(io, phdr.p_offset, (void *)phdr.p_vaddr, phdr.p_filesz);
                if (res < 0){
                    return res;
                }
                if (res != phdr.p_filesz){
                    return -EBADFMT;
                }

            }
            if (phdr.p_filesz < phdr.p_memsz){
                memset((void*)(phdr.p_vaddr+phdr.p_filesz), 0, phdr.p_memsz - phdr.p_filesz);
            }
            int flags = 0;
            if (phdr.p_flags & PF_R){
                flags |= PTE_R;
            }
            if (phdr.p_flags & PF_W){
                flags |= PTE_W;
            }
            if (phdr.p_flags & PF_X){
                flags |= PTE_X;
            }     
            
            set_range_flags((void*)phdr.p_vaddr, phdr.p_memsz, flags | PTE_U);
        } 
        else{
            continue;
        }
    }
    
    *eptr = (void (*)(void))ehdr.e_entry;
    return 0;
    }