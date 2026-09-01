// memory.c - Physical and virtual memory manager
//
// Copyright (c) 2024-2026 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "board-conf.h"
#include "console.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "process.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

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

struct page_chunk {
    struct page_chunk * next;   // next page chunk in list
    unsigned long pagecnt;      // number of pages in chunk
};

struct pte {
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;
    uint64_t reserved : 7;
    uint64_t pbmt : 2;
    uint64_t n : 1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) \
    (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))

// CONSTANTS
//
#define CAT_SIZE 15
const char * art = 
" _\0"
" \\`*-.\0"
"  )  _`-.\0"
" .  : `. .\0"
" : _   '  \\ \0"
" ; *` _.   `*-._\0"
" `-.-'          `-.\0"
"   ;       `       `.\0"
"   :.       .        \\ \0"
"   . \\  .   :   .-'   .\0"
"   '  `+.;  ;  '      :\0"
"   :  '  |    ;       ;-.\0"
"   ; '   : :`-:     _.`* ;\0"
".*' /  .*' ; .*`- +'  `*'\0"
"`*-*   `*-*  `*-*'\0";


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


static int print_memory_info(struct mregion * mmio_imm, unsigned long mmiocnt,
                             struct mregion * ram_imm, unsigned long ramcnt,
                             struct mregion * resv_imm, unsigned long resvcnt);


// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT] __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
__attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
__attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80001[PTE_CNT]
__attribute__((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk * free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
//

void memory_init (
    const struct mregion * ram_og, unsigned long ramcnt,
    const struct mregion * mmio_og, unsigned long mmiocnt,
    const struct mregion * resv_og, unsigned long resvcnt)
{

    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;

    // All parameters are used for running on real hardware and printing memory info; 
    // you can ignore them in this simplified memory_init() implementation


    struct mregion mmio[mmiocnt];
    struct mregion ram[ramcnt];
    struct mregion resv[resvcnt+1];

    memcpy(mmio, mmio_og, mmiocnt * sizeof(struct mregion));
    memcpy(ram, ram_og, ramcnt * sizeof(struct mregion));
    memcpy(resv, resv_og, resvcnt * sizeof(struct mregion));

    //The memory print function expects the kernel to be mapped as a reserved
    //region and the array to be sorted. For the qvirt target, the second 
    //reserved region is in high memory, so we move it right one and insert our
    //kernel as the second to last entry.
    resvcnt++;
    resv[resvcnt-1].pma = resv[resvcnt-2].pma;
    resv[resvcnt-1].size = resv[resvcnt-2].size;
    resv[resvcnt-2].pma = 
        ROUND_DOWN((uintptr_t)(void *)_kimg_start, PAGE_SIZE);
    resv[resvcnt-2].size = 
        ROUND_UP((uintptr_t)(void *)_kimg_end, PAGE_SIZE) - 
        ROUND_DOWN((uintptr_t)(void *)_kimg_start, PAGE_SIZE);

    kprintfluffy(40, &art, "");   
    kprintfluffy(40, &art, "System Memory Map:");
    kprintfluffy(40, &art, "");

    int fluffy = print_memory_info(mmio, mmiocnt, ram, ramcnt, resv, resvcnt);

    for(int i = 0; i < CAT_SIZE - 5 - fluffy; i++){ 
        kprintfluffy(40, &art, "");
    }

    void * heap_start;
    void * heap_end;

    uintptr_t pma;
    const void * pp;


    // Kernel must fit inside 2MB megapage (one level 1 PTE)

    if (MEGA_SIZE < _kimg_end - _kimg_start) panic(NULL);

    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // _kimg_start to _kimg_end:         RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB

    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void * )pma, PTE_R | PTE_W | PTE_G);

    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First two physical megaranges of RAM are mapped as individual pages with
    // permissions based on kernel image region.
    //
    // This also means that the kernel must be smaller than 4 MB
    
    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);
    main_pt1_0x80000[VPN1(RAM_START_PMA + MEGA_SIZE)] = ptab_pte(main_pt0_0x80001, PTE_G);

    //Map the reserved region before the kernel
    for (pp = RAM_START; pp < text_start; pp+=PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        assert(PTE_VALID(main_pt1_0x80000[VPN1((uintptr_t)pp)])); //Kernel too big.
        struct pte * pt1 = pageptr(main_pt1_0x80000[VPN1((uintptr_t)pp)].ppn);
        pt1[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        assert(PTE_VALID(main_pt1_0x80000[VPN1((uintptr_t)pp)]));
        struct pte * pt1 = pageptr(main_pt1_0x80000[VPN1((uintptr_t)pp)].ppn);
        pt1[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        assert(PTE_VALID(main_pt1_0x80000[VPN1((uintptr_t)pp)]));
        struct pte * pt1 = pageptr(main_pt1_0x80000[VPN1((uintptr_t)pp)].ppn);
        pt1[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages - dtb area for QEMU

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END - MEGA_SIZE; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    //Map DTB as read only
    main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);
    kprintf("Enabled Address Translation.\n");
    sfence_vma();

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void * )ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP(HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");

    // Initialize heap memory manager

    heap_init(heap_start, heap_end - heap_start);

    debug("Heap allocator: [%p,%p): %zu KB free", heap_start, heap_end,
            (heap_end - heap_start) / 1024);

    debug("Heap allocator: [%p,%p): %zu KB free",
            heap_start, heap_end, (heap_end - heap_start) / 1024);

    // YOUR CODE HERE
    // Initialize free chunk list
    free_chunk_list = heap_end;
    free_chunk_list->pagecnt = ((RAM_END - MEGA_SIZE) - heap_end) / PAGE_SIZE;
    free_chunk_list->next = NULL;
    
    kprintfluffy(40, &art, "Free Chunk List Initialized!");
    kprintfluffy(40, &art, "%d Free Pages.", free_phys_page_count());
    kprintfluffy(40, &art, "");

    debug("Page allocator: [%p,%p): %u pages free",
            heap_end, RAM_END, free_chunk_list->pagecnt);

    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) {
    // YOUR CODE HERE
    return csrr_satp();
    
}

mtag_t switch_mspace(mtag_t mtag) {
    // YOUR CODE HERE
    unsigned long prev_mspace = csrr_satp();
    csrw_satp(mtag);
    sfence_vma();
    return prev_mspace;
    
}
int clone_walk_helper(struct pte* root, struct pte* res){

    for (int i = 0; i < PTE_CNT; i++){

        if (!PTE_VALID(root[i])){
            res[i] = null_pte();
            continue;
        }

        if (PTE_GLOBAL(root[i])){
            res[i] = root[i];
            continue;
        }

        if (PTE_LEAF(root[i])){

            void * pp = alloc_phys_page();
            if (pp == NULL){
                return -EFAULT;
            }

            void * copy_addr = pageptr(root[i].ppn);
            for (int j = 0; j < PAGE_SIZE; j++){
                ((char *)pp)[j] = ((char *)copy_addr)[j];
            }
            res[i] = leaf_pte(pp, root[i].flags & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_G));
            continue;
        }
        
        struct pte* child = alloc_phys_page();
        if (child == NULL){
            return -EFAULT;
        }
        int call = clone_walk_helper(pageptr(root[i].ppn), child);
        if (call < 0){
            free_phys_page(child);
            return call;
        }
        res[i] = ptab_pte(child, root[i].flags & PTE_G);

    }
    return 0;
}

mtag_t clone_active_mspace(void) {
    // YOUR CODE HERE

    struct pte* clone = alloc_phys_page();

    if (clone == NULL){
        return 0;
    }

    if (clone_walk_helper(active_space_ptab(), clone) < 0) {
        free_phys_page(clone);
        return 0;
    }
    return ptab_to_mtag(clone, 0);
}

int reset_walk_helper(struct pte* root){

    for (int i = 0; i < PTE_CNT; i++){

        if (!PTE_VALID(root[i])) {
            continue;
        }

        if (PTE_LEAF(root[i]) && PTE_GLOBAL(root[i])){
            continue;
        }

        if (PTE_LEAF(root[i])){
            uint64_t leaf_ppn = root[i].ppn;
            root[i] = null_pte();
            free_phys_page(pageptr(leaf_ppn));
            continue;
        }
        
        struct pte* child = pageptr(root[i].ppn);
        if (child == NULL){
            return -EFAULT;
        }
        int call = reset_walk_helper(child);
        if (call < 0){
            return call;
        }  

        int free_intermediate = 1;
        for (int j = 0; j < PTE_CNT; j++){
            if (PTE_VALID(child[j])) {
                free_intermediate = 0;
                break;
            }
        }
        if (free_intermediate && !PTE_GLOBAL(root[i])) {
            uint64_t intermediate_ppn = root[i].ppn;
            root[i] = null_pte();
            free_phys_page(pageptr(intermediate_ppn));
        }

    }
    return 0;
}

void reset_active_mspace(void) {
    
    reset_walk_helper(active_space_ptab());
    sfence_vma();
}

int discard_walk_helper(struct pte* root){

    for (int i = 0; i < PTE_CNT; i++){

        if (!PTE_VALID(root[i]) || PTE_GLOBAL(root[i])){
            continue;
        }

        if (PTE_LEAF(root[i])){
            uint64_t leaf_ppn = root[i].ppn;
            root[i] = null_pte();
            free_phys_page(pageptr(leaf_ppn));
            continue;
        }
        
        struct pte* child = pageptr(root[i].ppn);
        if (child == NULL){
            return -EFAULT;
        }
        int call = discard_walk_helper(child);
        if (call < 0){
            return call;
        }  

        uint64_t intermediate_ppn = root[i].ppn;
        root[i] = null_pte();
        free_phys_page(pageptr(intermediate_ppn));
        

    }
    return 0;
}


mtag_t discard_active_mspace(void) {
    // YOUR CODE HERE

    struct pte * discard_root = active_space_ptab();
    if (active_space_mtag() == main_mtag){
        return main_mtag;
    }

    switch_mspace(main_mtag);

    if (discard_walk_helper(discard_root) < 0){
        panic("discard failed");
    }
    free_phys_page(discard_root);
    return active_space_mtag();
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be desirable to support
// mapping megapages and gigapages.

void * map_page(uintptr_t vma, void * pp, int rwxug_flags) {
    // YOUR CODE HERE
    struct pte* pt2 = active_space_ptab();
    if (!PTE_VALID(pt2[VPN2(vma)])) {
        void * pma = alloc_phys_page();
        if(pma == NULL){
            return NULL;
        }
        memset(pma, 0, PAGE_SIZE);
        pt2[VPN2(vma)].ppn = pagenum(pma);
        pt2[VPN2(vma)].flags = PTE_V | (rwxug_flags & PTE_G);
    }
    
    uintptr_t pt1_ppn = pt2[VPN2(vma)].ppn;
    uintptr_t pt1_pma = pt1_ppn << 12;
    struct pte* pt1 = (struct pte*)pt1_pma;

    if (!PTE_VALID(pt1[VPN1(vma)])) {
        void * pma = alloc_phys_page();
        if (pma == NULL){
            return NULL;
        }
        memset(pma, 0, PAGE_SIZE);
        pt1[VPN1(vma)].ppn = pagenum(pma);
        pt1[VPN1(vma)].flags = PTE_V | (rwxug_flags & PTE_G);
    }

    uintptr_t pt0_ppn = pt1[VPN1(vma)].ppn;
    uintptr_t pt0_pma = pt0_ppn << 12;
    struct pte* pt0 = (struct pte*)pt0_pma;

    //implement remapping case if necessary

    pt0[VPN0(vma)].ppn = pagenum(pp);
    pt0[VPN0(vma)].flags = PTE_V | rwxug_flags;
    
    return (void*)vma;
}

void * map_range(uintptr_t vma, size_t size, void * pp, int rwxug_flags) {
    // YOUR CODE HERE
    for (size_t i = 0; i < size; i+= PAGE_SIZE){
        if (map_page(vma+i, (uint8_t *)pp+i, rwxug_flags) == NULL){
            return NULL;
    }
    }
    return (void*)vma;

}

void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // YOUR CODE HERE

    void * pp = alloc_phys_pages(CEIL(size, PAGE_SIZE));
    if (pp == NULL){
        return NULL;
    }
    void * map_res = map_range(vma, size, pp, rwxug_flags);
    if (map_res == NULL){
        free_phys_pages(pp, CEIL(size, PAGE_SIZE));
        return NULL;
    }
    return (void*)vma;
}

void set_range_flags(const void * vp, size_t size, int rwxug_flags) {
    // YOUR CODE HERE

    struct pte * pt2 = active_space_ptab();

    for (uintptr_t i = ROUND_DOWN((uintptr_t)vp, PAGE_SIZE); i < (uintptr_t)vp+size; i+= PAGE_SIZE){
        
        if (PTE_VALID(pt2[VPN2(i)])){
            struct pte * pt1 = pageptr(pt2[VPN2(i)].ppn);
            if (PTE_VALID(pt1[VPN1(i)])){
                struct pte * pt0 = pageptr(pt1[VPN1(i)].ppn);
                if (PTE_VALID(pt0[VPN0(i)])){
                    pt0[VPN0(i)].flags = PTE_V | rwxug_flags;
                }
            }
        }
    }

    sfence_vma();
    return;
}

void unmap_and_free_range(void * vp, size_t size) {
    // YOUR CODE HERE
    struct pte * pt2 = active_space_ptab();

    for (uintptr_t i = ROUND_DOWN((uintptr_t)vp, PAGE_SIZE); i < ROUND_UP((uintptr_t)vp+size, PAGE_SIZE); i+= PAGE_SIZE){
        
        uint8_t clear_pt1 = 1;
        uint8_t clear_pt2 = 1;

        if (PTE_VALID(pt2[VPN2(i)])){
            struct pte * pt1 = pageptr(pt2[VPN2(i)].ppn);
            
            if (PTE_VALID(pt1[VPN1(i)])){
                struct pte * pt0 = pageptr(pt1[VPN1(i)].ppn);

                if (PTE_VALID(pt0[VPN0(i)])){
                    uint64_t leaf_ppn = pt0[VPN0(i)].ppn;
                    pt0[VPN0(i)] = null_pte();
                    free_phys_page(pageptr(leaf_ppn));

                    for (uintptr_t j = 0; j < PTE_CNT; j++){
                        if (PTE_VALID(pt0[j])){
                            clear_pt1 = 0;
                            break;
                        }
                    }
                    if (clear_pt1){
                        uint64_t pt0_ppn = pt1[VPN1(i)].ppn;
                        pt1[VPN1(i)] = null_pte();
                        free_phys_page(pageptr(pt0_ppn));
                    }
                    for (uintptr_t j = 0; j < PTE_CNT; j++){
                        if (PTE_VALID(pt1[j])){
                            clear_pt2 = 0;
                            break;
                        }
                    }
                    if (clear_pt2){
                        uint64_t pt1_ppn = pt2[VPN2(i)].ppn;
                        pt2[VPN2(i)] = null_pte();
                        free_phys_page(pageptr(pt1_ppn));
                    }
                }
            }
        }
    }

    sfence_vma();
    return;
}


int enforce_vptr(const void * vp, size_t size, int rwxug_flags) {
    // YOUR CODE HERE

    if (vp == NULL){
        return -EFAULT;
    }
     
    struct pte * pt2 = active_space_ptab();

    for (uintptr_t i = ROUND_DOWN((uintptr_t)vp, PAGE_SIZE); i < ROUND_UP((uintptr_t)vp+size, PAGE_SIZE); i+= PAGE_SIZE){

        if (!wellformed(i)){
            return -EFAULT;
        }
        if (i >= UMEM_END_VMA || i < UMEM_START_VMA){
            return -EFAULT;
        }

        if (!PTE_VALID(pt2[VPN2(i)])){
            if (!(rwxug_flags & PTE_X)) {
                void* alloc_res = alloc_and_map_range(i, PAGE_SIZE, rwxug_flags);
                if (alloc_res == NULL){
                    return -EFAULT;
                }
            }
            else{
                return -EFAULT;
            }
            continue;
        }
        
        struct pte * pt1 = pageptr(pt2[VPN2(i)].ppn);
            
        if (!PTE_VALID(pt1[VPN1(i)])){
            if (!(rwxug_flags & PTE_X)) {
                void* alloc_res = alloc_and_map_range(i, PAGE_SIZE, rwxug_flags);
                if (alloc_res == NULL){
                    return -EFAULT;
                }
            }
            else{
                return -EFAULT;
            }
            continue;
        }
        struct pte * pt0 = pageptr(pt1[VPN1(i)].ppn);
                
        if (!PTE_VALID(pt0[VPN0(i)])){
            if (!(rwxug_flags & PTE_X)) {
                void* alloc_res = alloc_and_map_range(i, PAGE_SIZE, rwxug_flags);
                if (alloc_res == NULL){
                    return -EFAULT;
                }
            }
            else{
                return -EFAULT;
            }
            continue;
        }
        if (((pt0[VPN0(i)].flags & (PTE_R | PTE_W | PTE_U | PTE_G | PTE_X)) & rwxug_flags) != rwxug_flags) {
            return -EFAULT;
        }
        }

    sfence_vma();
    return 0;
}

int validate_vstr(const char * vs, int rwxug_flags) {
    // YOUR CODE HERE

    //add valid checks?

    if (vs == NULL){
        return -EFAULT;
    }

    struct pte * pt2 = active_space_ptab();
    const char * cur = vs;
    while (1 == 1){


        if (!PTE_VALID(pt2[VPN2((uintptr_t)cur)])){
            return -EFAULT;
        }
        struct pte * pt1 = pageptr(pt2[VPN2((uintptr_t)cur)].ppn);
        
        if (!PTE_VALID(pt1[VPN1((uintptr_t)cur)])){
            return -EFAULT;
        }
        struct pte * pt0 = pageptr(pt1[VPN1((uintptr_t)cur)].ppn);

        if (!PTE_VALID(pt0[VPN0((uintptr_t)cur)])){
            return -EFAULT;
        }

        if (((pt0[VPN0((uintptr_t)cur)].flags & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_G)) & (rwxug_flags)) != rwxug_flags) {
            return -EFAULT;
        }

        if (*((char*)((pt0[VPN0((uintptr_t)cur)].ppn << 12) | ((uintptr_t)cur & 0xFFF))) == '\0'){
            break;
        }
        cur += 1;
        
    }

    return 0;
}

void * alloc_phys_page(void) {
    // YOUR CODE HERE
    
    struct page_chunk * cur = free_chunk_list;
    struct page_chunk * prev = NULL;
    struct page_chunk * best_fit = free_chunk_list;
    unsigned long fit = UINT64_MAX;
    while (cur != NULL){

        if (cur->pagecnt == 1){
            void* pp = cur;
            if (prev != NULL){
                prev->next = cur->next;
            }
            else{
                free_chunk_list = cur->next;
            }
            return pp;
        }
        if (cur->pagecnt - 1 < fit){
            best_fit = cur;
            fit = cur->pagecnt - 1;
        }

        prev = cur;
        cur = cur->next;
    }

    void* pp = (uint8_t *)best_fit + (best_fit->pagecnt - 1) * PAGE_SIZE;
    best_fit->pagecnt -= 1;
    return pp;
}

void free_phys_page(void * pp) {
    // YOUR CODE HERE

    struct page_chunk* cur = pp;
    cur->pagecnt = 1;
    cur->next = free_chunk_list;
    free_chunk_list = cur;

}

void * alloc_phys_pages(unsigned int cnt) {
    // YOUR CODE HERE

     struct page_chunk * cur = free_chunk_list;
    struct page_chunk * prev = NULL;
    struct page_chunk * best_fit = free_chunk_list;
    unsigned long fit = UINT64_MAX;
    while (cur != NULL){

        if (cur->pagecnt == cnt){
            void* pp = cur;
            if (prev != NULL){
                prev->next = cur->next;
            }
            else{
                free_chunk_list = cur->next;
            }
            return pp;
        }
        if (cur->pagecnt > cnt && cur->pagecnt - cnt < fit){
            best_fit = cur;
            fit = cur->pagecnt - cnt;
        }

        prev = cur;
        cur = cur->next;
    }

    void* pp = (uint8_t *)best_fit + (best_fit->pagecnt - cnt) * PAGE_SIZE;
    best_fit->pagecnt -= cnt;
    return pp;
}

void free_phys_pages(void * pp, unsigned int cnt) {
    // YOUR CODE HERE
    struct page_chunk* cur = pp;
    cur->pagecnt = cnt;
    cur->next = free_chunk_list;
    free_chunk_list = cur;
    return;
}

unsigned long free_phys_page_count(void) {
    // YOUR CODE HERE
    struct page_chunk* cur = free_chunk_list;
    unsigned long count = 0;
    while (cur != NULL) {
        count += cur->pagecnt;
        cur = cur->next;
    }
    return count;
}

int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma) {
    // YOUR CODE HERE
    if (vma < UMEM_START_VMA || vma >= UMEM_END_VMA){
        return 0;
    }

    vma = ROUND_DOWN(vma, PAGE_SIZE);

    void* vma_res = alloc_and_map_range(vma, PAGE_SIZE, PTE_U | PTE_W | PTE_R);
    if (vma_res == NULL){
        return -EFAULT;
    }
    return 1;
}

// INTERNAL FUNCTION DEFINITIONS
//


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
    return (struct pte * )((mtag << 20) >> 8);
}

static inline struct pte * active_space_ptab(void) {
    return mtag_to_ptab(active_space_mtag());
}

static inline void * pageptr(uintptr_t n) {
    return (void *)(n << PAGE_ORDER);
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

//This function assumes:
//- resv strictly overlaps with RAM
//- MMIO and RAM do not overlap
//- Arrays are sorted
//- 0 size entries are invalid
//- That I'm very sorry to whoever has to read this.
//
// * THIS FUNCTION IS DESTRUCTIVE TO MREGION ARRAYS.
//
//...returns how many times kprintfluffy was called :innocent:
static int print_memory_info(struct mregion * mmio_imm, unsigned long mmiocnt,
                             struct mregion * ram_imm, unsigned long ramcnt,
                             struct mregion * resv_imm, unsigned long resvcnt) {
    unsigned long mmio_idx = 0;
    unsigned long ram_idx = 0;
    unsigned long resv_idx = 0;

    int fluffy = 0;

    //The first thing we do is copy out the arguments to prevent destruction of
    //the arguments.

    struct mregion mmio[mmiocnt];
    struct mregion ram[ramcnt];
    struct mregion resv[resvcnt];

    memcpy(mmio, mmio_imm, mmiocnt * sizeof(struct mregion));
    memcpy(ram, ram_imm, ramcnt * sizeof(struct mregion));
    memcpy(resv, resv_imm, resvcnt * sizeof(struct mregion));

    while (mmio_idx < mmiocnt || ram_idx < ramcnt) {
        uintptr_t mmio_loc = mmio[mmio_idx].pma;
        uintptr_t ram_loc = ram[ram_idx].pma;
        uintptr_t resv_loc = resv[resv_idx].pma;
        
        if (mmio_loc < ram_loc && mmio_idx < mmiocnt) {
            kprintfluffy(40, &art, "%08p - %08p - MMIO", 
                         mmio_loc, 
                         mmio_loc + mmio[mmio_idx].size - 1);
            fluffy++;
            mmio_idx++;
        } else {

            if (ram_loc <= resv_loc && 
               ram_loc + ram[ram_idx].size > resv_loc && resv_idx < resvcnt) {

                if (ram_loc < resv_loc) {
                    kprintfluffy(40, 
                                 &art, 
                                 "%p - %p - Free RAM", 
                                 ram_loc, 
                                 resv_loc - 1);
                    ram[ram_idx].size -= resv_loc - ram_loc;
                    ram[ram_idx].pma = resv_loc;
                    fluffy++;
                } else {

                    if (resv_loc <= (uintptr_t)_kimg_start && 
                        resv_loc + resv[resv_idx].size > (uintptr_t)_kimg_start) {

                        if (resv_loc < (uintptr_t)_kimg_start) {
                            kprintfluffy(40, 
                                         &art, 
                                         "%p - %p - SBI Reserved", 
                                         resv_loc, 
                                         (uintptr_t)_kimg_start - 1);

                            resv[resv_idx].size -= 
                                (uintptr_t)_kimg_start - resv_loc;
                            resv[resv_idx].pma = (uintptr_t)_kimg_start;
                            ram[ram_idx].pma = (uintptr_t)_kimg_start;
                            ram[ram_idx].size -= 
                                (uintptr_t)_kimg_start - resv_loc;
                            fluffy++;
                        } else {
                            kprintfluffy(40, 
                                         &art, 
                                         "%p - %p - Kernel TEXT", 
                                         _kimg_text_start, 
                                         ROUND_UP((uintptr_t)_kimg_text_end, 
                                                  PAGE_SIZE) - 1);

                            kprintfluffy(40, 
                                         &art, 
                                         "%p - %p - Kernel RODATA", 
                                         _kimg_rodata_start, 
                                         ROUND_UP((uintptr_t)_kimg_rodata_end, 
                                                  PAGE_SIZE) - 1);

                            kprintfluffy(40, 
                                         &art, 
                                         "%p - %p - Kernel DATA", 
                                         _kimg_data_start, 
                                         ROUND_UP((uintptr_t)_kimg_data_end, 
                                                  PAGE_SIZE) - 1);

                            resv[resv_idx].pma = 
                                ROUND_UP((uintptr_t)_kimg_end, PAGE_SIZE);
                            resv[resv_idx].size -= 
                                ROUND_UP((uintptr_t)_kimg_end, PAGE_SIZE) - resv_loc;
                            ram[ram_idx].pma = 
                                ROUND_UP((uintptr_t)_kimg_end, PAGE_SIZE);
                            ram[ram_idx].size -= 
                                ROUND_UP((uintptr_t)_kimg_end, PAGE_SIZE) - resv_loc;
                            fluffy+=3;
                        }
                    } else {
                        kprintfluffy(40, 
                                     &art, 
                                     "%p - %p - SBI Reserved", 
                                     resv_loc, 
                                     resv_loc + resv[resv_idx].size - 1);
                        ram[ram_idx].pma = resv_loc + resv[resv_idx].size;
                        ram[ram_idx].size -= resv[resv_idx].size;
                        resv_idx++;
                        fluffy++;
                    }
                }
            } else {
                kprintfluffy(40, 
                             &art, 
                             "%p - %p - Free RAM", 
                             ram_loc, 
                             ram_loc + ram[ram_idx].size - 1);
                fluffy++;
                ram_idx++;
            }
        }

        //Ignore NULL entries (don't need to do this first because first entries 
        //are guaranteed to be populated)
        while(mmio[mmio_idx].size == 0 && mmio_idx < mmiocnt)
                mmio_idx++;
        while(ram[ram_idx].size == 0 && ram_idx < ramcnt)
                ram_idx++;
        while(resv[resv_idx].size == 0 && resv_idx < resvcnt)
                resv_idx++;

    }
    return fluffy;
}