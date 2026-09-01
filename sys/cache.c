// cache.c - Block cache for a storage device
//
// Copyright (c) 2024-2026 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#include "cache.h"
#include "conf.h"
#include "device.h"
#include "memory.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "heap.h"
#include "misc.h"
#include "io.h"
#include "console.h"

// INTERNAL TYPE DEFINITIONS
//


struct cache {
    // YOUR CODE HERE
    struct io *bkgio;
    struct block* head;
    struct block* tail;
    int size;
    int cache_blksz;
    struct rwlock* ll_lock;
};

struct block{
    unsigned long long blk_no;
    void *data;
    int dirty;
    struct block* next;
    struct block* prev;
    struct rwlock* lock;
};


// INTERNAL FUNCTION DECLARATIONS
//
void remove_entry(struct block * node);
void add_entry(struct cache* cache, struct block* node);

// EXPORTED FUNCTION DEFINITIONS
//

struct cache * create_cache(struct io * bkgio, unsigned long cache_blksz) {
    // YOUR CODE HERE
    struct cache* lru_cache = kmalloc(sizeof(struct cache));
     if (lru_cache == NULL){
        return NULL;
    }
    lru_cache->head = kmalloc(sizeof(struct block));
    lru_cache->tail = kmalloc(sizeof(struct block));
    lru_cache->ll_lock = kmalloc(sizeof(struct rwlock));
    rwlock_init(lru_cache->ll_lock, "ll");

    lru_cache->cache_blksz = cache_blksz;
    lru_cache->bkgio = bkgio;
    lru_cache->head->next = lru_cache->tail;
    lru_cache->head->prev = NULL;
    lru_cache->tail->prev = lru_cache->head;
    lru_cache->tail->next = NULL;
    lru_cache->size = 0; 
    return lru_cache;
}

void remove_entry(struct block * node){
    node->next->prev = node->prev;
    node->prev->next = node->next;
}

void add_entry(struct cache* cache, struct block* node){
    struct block* temp = cache->head->next;
    temp->prev = node;
    cache->head->next = node;
    node->next = temp;
    node->prev = cache->head;
}

int cache_fetch(
    struct cache * cache, unsigned long long pos, int exclusive, void ** pptr)
{
    //check if cached

    rwlock_acquire(cache->ll_lock, 1);
    struct block* cur = cache->head->next;
    while (cur != cache->tail){
        if (cur->blk_no == pos / cache->cache_blksz){
            rwlock_acquire(cur->lock, exclusive);
            remove_entry(cur);
            add_entry(cache, cur);
            *pptr = cur->data;
            rwlock_release(cache->ll_lock);
            return 0;
        }
        cur = cur->next;
    }

    //cache miss
    struct block* fetch_entry = kmalloc(sizeof(struct block));
    fetch_entry->data = kmalloc(cache->cache_blksz);
    fetch_entry->blk_no = pos / cache->cache_blksz;
    fetch_entry->lock = kmalloc(sizeof(struct rwlock));
    rwlock_init(fetch_entry->lock, "block_lock");
    rwlock_acquire(fetch_entry->lock, exclusive);
    fetch_entry->dirty = 0;
    fetch_entry->prev = NULL;
    fetch_entry->next = NULL;
    iofetch(cache->bkgio, pos, fetch_entry->data ,cache->cache_blksz);
    *pptr = fetch_entry->data;
    
    
    //add to head (most recent)
    add_entry(cache, fetch_entry);
    cache->size++;

    //remove when capacity will exceed
    if (cache->size > CACHE_CAPACITY){
        struct block* cur = cache->tail->prev;
        while (cur != cache->head) {
            if (cur->lock->cnt == 0){

                //write_back if dirty
                if (cur->dirty == 1){
                    long res = iostore(cache->bkgio, cur->blk_no*cache->cache_blksz, cur->data, cache->cache_blksz);
                    if (res < 0){
                        rwlock_release(cache->ll_lock);
                        rwlock_release(fetch_entry->lock);
                        return res;
                    }
                }
                remove_entry(cur);
                kfree(cur->data);
                kfree(cur->lock);
                kfree(cur);
                cache->size--;
                break;
            }
            cur = cur->prev;
        }
    }
    rwlock_release(cache->ll_lock);
    return 0;
}

void cache_release(struct cache * cache, void * pblk, int dirty) {
    // YOUR CODE HERE
    struct block* cur = cache->head->next;

    rwlock_acquire(cache->ll_lock, 1);

    while (cur != cache->tail) {
        if (cur->data == pblk){
            if (dirty > 0){
            cur->dirty = 1;
            }
            rwlock_release(cur->lock);
            break;
        }
        cur = cur->next;
    }

    rwlock_release(cache->ll_lock);
    return;

}

int cache_flush(struct cache * cache) {
    // YOUR CODE HERE
    struct block* cur = cache->head->next;
    
    rwlock_acquire(cache->ll_lock, 1);
    while (cur != cache->tail) {
        if (cur->dirty == 1 && cur->lock->cnt == 0){
            iostore(cache->bkgio, cur->blk_no*cache->cache_blksz, cur->data, cache->cache_blksz);
            cur->dirty = 0;
        }
        cur = cur->next;
    }
    rwlock_release(cache->ll_lock);
    return 0;


}

// INTERNAL FUNCTION DEFINITIONS
//

