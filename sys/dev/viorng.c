/*! @file viorng.c 
    @brief VirtIO rng device
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "device.h"
#include "conf.h"
#include "intr.h"
#include "misc.h"
#include "ioimpl.h"

// INTERNAL CONSTANT DEFINITIONS
//



#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

#ifndef VIORNG_QID
#define VIORNG_QID 0
#endif

#ifndef VIORNG_QUEUE_SIZE 
#define VIORNG_QUEUE_SIZE 1
#endif 

// INTERNAL TYPE DEFINITIONS
//
struct viorng_device {
    volatile struct virtio_mmio_regs * regs; 
    int irqno; 
    struct virtq_desc * viorng_desc; 
    struct virtq_avail * viorng_avail; 
    volatile struct virtq_used * viorng_used; 
    struct condition write_finished; 
    struct rwlock vrng_lock;
    struct io io; 
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_open(struct io ** ioptr, void * aux);

static void viorng_reclaim(struct io * io);

static long viorng_read(struct io * io, void * buf, long bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct iointf viorng_intf = {
    .implname = "viorng",
    .reclaim = &viorng_reclaim,
    .read = &viorng_read
};

// EXPORTED FUNCTION DEFINITIONS
//

// The vioblk_attach function is declared and called from virtio_attach() in
// virtio.c when a VirtIO RNG device is found.

void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    static unsigned short instcnt = 0; // number of viorng devices
    trace("number of instances: %d", instcnt); 
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_device * vrng;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        debug("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    regs->status |= VIRTIO_STAT_FEATURES_OK;
    assert(regs->status & VIRTIO_STAT_FEATURES_OK);
    
    // queue size of one 
    // Initailize the struct and allocated memory in RAM for queues 
    vrng = kcalloc(1, sizeof(*vrng)); 
    vrng->regs = regs; 
    vrng->irqno = irqno; 
    vrng->viorng_desc = kcalloc(1, sizeof(*vrng->viorng_desc)); 
    vrng->viorng_avail = kcalloc(1, sizeof(*vrng->viorng_avail) + sizeof(*vrng->viorng_avail->ring) + 2); 
    vrng->viorng_used = kcalloc(1, sizeof(*vrng->viorng_used) + sizeof(*vrng->viorng_used->ring) + 2);
    vrng->viorng_avail->idx = 0;

    // attach locks and condition variables
    rwlock_init(&vrng->vrng_lock, NULL); 
    condition_init(&vrng->write_finished, NULL); 


    // Tells the device where they are in RAM
    virtio_attach_virtq(regs, VIORNG_QID, VIORNG_QUEUE_SIZE, (uint64_t) vrng->viorng_desc, (uint64_t) vrng->viorng_used, (uint64_t) vrng->viorng_avail);   
    virtio_enable_virtq(vrng->regs, VIORNG_QID);
    
    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    // YOUR CODE HERE
    register_device(VIORNG_NAME, instcnt++, &viorng_open, vrng);
    ioinit(&vrng->io, &viorng_intf, 1, 0); 
}

int viorng_open(struct io ** ioptr, void * aux) {
    struct viorng_device * viorng = aux; 
    trace("\nVRNG OPENED!!!!\n");
    enable_intr_source(viorng->irqno, VIORNG_IRQ_PRIO, viorng_isr, viorng);
    *ioptr = ioaddref(&viorng->io); 
    return 0; 
}

void viorng_reclaim(struct io * io) {
    struct viorng_device* vrng = (void *) io - offsetof(struct viorng_device, io);  
    disable_intr_source(vrng->irqno); 
}

long viorng_read(struct io * io, void * buf, long bufsz) {
    
    if(bufsz == 0) return 0; 
    
    struct viorng_device* vrng = (void *) io - offsetof(struct viorng_device, io); 
    uint64_t bytes_recieved = 0; 


    // buf is large enough to support bufsz as given in ioread() 
    rwlock_acquire(&vrng->vrng_lock, 1); 
    while(bytes_recieved < bufsz) {
        // add buf to the desc_table at idx = 0 (since queue size = 1)
        vrng->viorng_desc[0].addr = (uint64_t) buf + bytes_recieved; 
        vrng->viorng_desc[0].len = bufsz - bytes_recieved; 
        vrng->viorng_desc[0].flags = VIRTQ_DESC_F_WRITE;

        uint16_t used_idx_prev = vrng->viorng_used->idx; 

        __sync_synchronize(); 

        // add data to avail table 
        vrng->viorng_avail->ring[0] = 0; 
        vrng->viorng_avail->idx++; 

        __sync_synchronize(); 

        // notify driver that buffer has been made available 
        int pie = disable_interrupts(); 
        vrng->regs->queue_notify = VIORNG_QID; 
        
       // yield if waiting for the entropy device
        while(used_idx_prev == vrng->viorng_used->idx) {
            condition_wait(&vrng->write_finished);
        }
        restore_interrupts(pie); 
        
        // check how many bytes are written
        bytes_recieved += vrng->viorng_used->ring[0].len; 
    }
    rwlock_release(&vrng->vrng_lock); 

    return bufsz;
}

void viorng_isr(int irqno, void * aux) {
    struct viorng_device * vrng = (struct viorng_device * ) aux; 
    trace("vrng isr reached\n");
    if(!vrng->regs->interrupt_status) 
        return; 
    if(vrng->regs->interrupt_status & 1) {
        condition_broadcast(&vrng->write_finished); 
    }
    vrng->regs->interrupt_ack = vrng->regs->interrupt_status; 
}