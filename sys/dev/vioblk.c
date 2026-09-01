// vioblk.c - VirtIO block device
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "device.h"
#include "thread.h"
#include "error.h"
#include "string.h"
#include "conf.h"
#include "misc.h"
#include "error.h"
#include "console.h"
#include "ioimpl.h"

#include <limits.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif


#ifndef VIOBLK_QID
#define VIOBLK_QID 0
#endif


// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX       1
#define VIRTIO_BLK_F_SEG_MAX        2
#define VIRTIO_BLK_F_GEOMETRY       4
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_FLUSH          9
#define VIRTIO_BLK_F_TOPOLOGY       10
#define VIRTIO_BLK_F_CONFIG_WCE     11
#define VIRTIO_BLK_F_MQ             12
#define VIRTIO_BLK_F_DISCARD        13
#define VIRTIO_BLK_F_WRITE_ZEROES   14

// INTERNAL TYPE DEFINITIONS
//

// All VirtIO block device requests consist of a request header, defined below,
// followed by data, followed by a status byte. The header is device-read-only,
// the data may be device-read-only or device-written (depending on request
// type), and the status byte is device-written.

struct vioblk_request_header {
    uint32_t type; 
    uint32_t reserved; 
    uint64_t sector; 
};

// Request type (for vioblk_request_header)

#define VIRTIO_BLK_T_IN             0
#define VIRTIO_BLK_T_OUT            1

// Status byte values

#define VIRTIO_BLK_S_OK         0
#define VIRTIO_BLK_S_IOERR      1
#define VIRTIO_BLK_S_UNSUPP     2

// Virt queue size for Block Device 
#define VIOBLK_QUEUE_SIZE       4

/**
 * @brief VirtIO Block Device with virtqueues and condition variables
 */ 

struct vioblk_device {
    volatile struct virtio_mmio_regs* regs; 
    int irqno; 

    struct io io; 

    // virtqueues for virtio interface 
    uint8_t virtq_len; 
    struct virtq_desc * desc; 
    volatile struct virtq_avail * avail; 
    volatile struct virtq_used * used;

    struct condition vioblk_finished;
    struct rwlock vioblk_lock; 
};

// INTERNAL FUNCTION DECLARATIONS
//

static int vioblk_open(struct io ** ioptr, void * aux);

static void vioblk_reclaim(struct io * io);

static long vioblk_fetch (
    struct io * io, unsigned long long pos, void * buf, long len);

static long vioblk_store (
    struct io * io, unsigned long long pos, const void * buf, long len);

static int vioblk_ioctl(struct io * io, int op, void * arg);

static void vioblk_isr(int srcno, void * aux);
        
        
// INTERNAL GLOBAL VARIABLES
//

static const struct iointf vioblk_intf = {
    .implname = "vioblk",
    .reclaim = &vioblk_reclaim,
    .fetch = &vioblk_fetch,
    .store = &vioblk_store,
    .ioctl = &vioblk_ioctl
};

// EXPORTED FUNCTION DEFINITIONS
//

// The vioblk_attach function is declared and called from virtio_attach() in
// virtio.c when a VirtIO block device is found.

void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    static unsigned short instcnt = 0; // number of vioblk devices
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_device * vb;
    unsigned int blksz;
    int result;
    
	trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);
    assert (regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize(); // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.
    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert (((blksz - 1) & blksz) == 0);
    
    // initialize VIOBLK struct data 
    vb = kcalloc(1, sizeof(*vb)); 
    vb->regs = regs; 
    vb->irqno = irqno; 

    // allocate space for descriptor table, avail_table, and used table 
    vb->virtq_len = VIOBLK_QUEUE_SIZE; 
    vb->desc = kcalloc(1, vb->virtq_len * sizeof(*vb->desc)); 
    vb->avail = kcalloc(1, sizeof(*vb->avail) + vb->virtq_len * sizeof(*vb->avail->ring) + 2); 
    vb->used = kcalloc(1, sizeof(*vb->used) + vb->virtq_len * sizeof(*vb->used->ring) + 2);

    // initialize locks and conditions variables
    rwlock_init(&vb->vioblk_lock, "vioblk_lock"); 
    condition_init(&vb->vioblk_finished, "vioblk condition"); 

    // Tells the device where they are in RAM
    virtio_attach_virtq(regs, VIOBLK_QID, VIOBLK_QUEUE_SIZE, (uint64_t) vb->desc, (uint64_t) vb->used, (uint64_t) vb->avail);   
    virtio_enable_virtq(vb->regs, VIOBLK_QID);

    // Device is now live 
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();

    register_device(VIOBLK_NAME, instcnt++, &vioblk_open, vb);
    ioinit(&vb->io, &vioblk_intf, blksz, 0);
}

int vioblk_open(struct io ** ioptr, void * aux) {
    // grab pointer to vb device 
    struct vioblk_device * vb = aux; 

    trace("\nVIOBLK OPENED!!!!\n");

    // enable interrupts for vb device 
    enable_intr_source(vb->irqno, VIOBLK_INTR_PRIO, &vioblk_isr, vb); 
    *ioptr = ioaddref(&vb->io);
    return 0;
}

void vioblk_reclaim(struct io * io) {
    struct vioblk_device * vb = (void *) io - offsetof(struct vioblk_device, io); 
    disable_intr_source(vb->irqno); 
}

long vioblk_fetch (
    struct io * io,
    unsigned long long bytepos,
    void * buf,
    long bytecnt)
{
    if(bytecnt == 0) {
        return 0; 
    } 

    // If pos + bytecnt > max, then don't perform any op
    struct vioblk_device * vb = (void *) io - offsetof(struct vioblk_device, io); 
    unsigned long long maximum = vb->regs->config.blk.capacity * 512; 
    if (bytepos >= maximum || maximum - bytepos < bytecnt) {
        return -EINVAL; 
    }

    // acquire lock to modify the virtqs
    rwlock_acquire(&vb->vioblk_lock, 1);

    // create the header to send 
    struct vioblk_request_header hd = {
        .type = VIRTIO_BLK_T_IN,
        .reserved = 0,
        .sector = bytepos / 512 
    }; 
    
    uint8_t status;

    // hardcode the descriptor addresses since we lock this operation
    vb->desc[0].addr = (uint64_t) &hd; 
    vb->desc[0].len = sizeof(hd); 
    vb->desc[0].flags = VIRTQ_DESC_F_NEXT; 
    vb->desc[0].next = 1; 

    vb->desc[1].addr = (uint64_t) buf; 
    vb->desc[1].len = bytecnt; 
    vb->desc[1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    vb->desc[1].next = 2; 

    vb->desc[2].addr = (uint64_t) &status; 
    vb->desc[2].flags = VIRTQ_DESC_F_WRITE;
    vb->desc[2].len = 1; 

    // get the previous used index 
    uint16_t used_idx_prev = vb->used->idx; 

    // fence, ensure that previous memory is saved first
    __sync_synchronize(); 

    vb->avail->ring[vb->avail->idx % VIOBLK_QUEUE_SIZE] = 0; 
    vb->avail->idx++; 

    // ensure this data is ready before we notify device 
    __sync_synchronize(); 

    // disable interrupts to prevent ISR between while and condition wait
    int pie = disable_interrupts(); 
    vb->regs->queue_notify = VIOBLK_QID; 
    while(used_idx_prev == vb->used->idx) {
        condition_wait(&vb->vioblk_finished); 
    }
    restore_interrupts(pie); 

    rwlock_release(&vb->vioblk_lock); 

    if(status == VIRTIO_BLK_S_OK) {
        return bytecnt; 
    } else if(status == VIRTIO_BLK_S_IOERR) {
        return -EIO;
    } else {
        return -ENOTSUP; 
    }
}


long vioblk_store (
    struct io * io,
    unsigned long long bytepos,
    const void * buf,
    long bytecnt)
{
    trace("%s", __func__); 
    if(bytecnt == 0) {
        return 0; 
    } 

    // If pos + bytecnt >= max, then don't perform any op
    struct vioblk_device * vb = (void *) io - offsetof(struct vioblk_device, io); 
    unsigned long long maximum = vb->regs->config.blk.capacity * 512; 
    trace("maximum: %d", maximum); 
    if (bytepos >= maximum || maximum - bytepos < bytecnt) {
        return -EINVAL; 
    }

    trace("%s", __func__); 
    // acquire lock to modify the virtqs
    rwlock_acquire(&vb->vioblk_lock, 1);
    trace("%s", __func__); 
    // create the header to send 
    struct vioblk_request_header hd = {
        .type = VIRTIO_BLK_T_OUT,
        .reserved = 0, 
        .sector = bytepos / 512
    }; 
    trace("%s", __func__); 

    uint8_t status;

    // hardcode the descriptor addresses since we lock this operation
    vb->desc[0].addr = (uint64_t) &hd; 
    vb->desc[0].len = sizeof(hd); 
    vb->desc[0].flags = VIRTQ_DESC_F_NEXT; 
    vb->desc[0].next = 1; 

    vb->desc[1].addr = (uint64_t) buf; 
    vb->desc[1].len = bytecnt; 
    vb->desc[1].flags = VIRTQ_DESC_F_NEXT;
    vb->desc[1].next = 2; 

    vb->desc[2].addr = (uint64_t) &status; 
    vb->desc[2].flags = VIRTQ_DESC_F_WRITE;
    vb->desc[2].len = 1; 
    trace("%s", __func__); 

    // get the previous used index 
    uint16_t used_idx_prev = vb->used->idx; 

    // fence, ensure that previous memory is saved first
    __sync_synchronize(); 

    vb->avail->ring[vb->avail->idx % VIOBLK_QUEUE_SIZE] = 0; 
    vb->avail->idx++; 

    // ensure this data is ready before we notify device 
    __sync_synchronize(); 
    trace("%s", __func__); 

    // disable interrupts to prevent ISR between while and condition wait
    int pie = disable_interrupts(); 
    vb->regs->queue_notify = VIOBLK_QID; 
    while(used_idx_prev == vb->used->idx) {
        condition_wait(&vb->vioblk_finished); 
    }
    restore_interrupts(pie); 
    trace("%s", __func__); 

    rwlock_release(&vb->vioblk_lock); 

    if(status == VIRTIO_BLK_S_OK) {
        return bytecnt; 
    } else if(status == VIRTIO_BLK_S_IOERR) {
        return -EIO;
    } else {
        return -ENOTSUP; 
    }
}

int vioblk_ioctl(struct io * io, int op, void * arg) {
    // get capacity of device during IOC_GETEND in bytes
    struct vioblk_device * vb = (void *) io - offsetof(struct vioblk_device, io); 
    if(op == IOC_GETEND) {  
        *(uint64_t *) arg = vb->regs->config.blk.capacity * 512; 
        return 0; 
    } else {
        return -ENOTSUP;
    }
}

void vioblk_isr(int irqno, void * aux) {
    struct vioblk_device * vb = (struct vioblk_device * ) aux; 
    trace("vioblk isr reached\n");

    // check for interrupt that virtq has been used
    if(vb->regs->interrupt_status & 1) {
        condition_broadcast(&vb->vioblk_finished); 
    }
    vb->regs->interrupt_ack = vb->regs->interrupt_status; 
}