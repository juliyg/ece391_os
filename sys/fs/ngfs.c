// ngfs.c - A FAT-like file system
//
// Copyright (c) 2026 University of Illinois
// SPDX-License-identifier: NCSA
//

#include <stddef.h>
#include <string.h>
#ifdef NGFS_TRACE
#define TRACE
#endif

#ifdef NGFS_DEBUG
#define DEBUG
#endif

#include "ngfs.h"

#include "../cache.h"
#include "../device.h"
#include "../error.h"
#include "../filesys.h"
#include "../fsimpl.h"
#include "../heap.h"
#include "../misc.h"
#include "../string.h"
#include "../ioimpl.h"
#include "../thread.h"

// INTERNAL TYPE DEFINITIONS
//

struct ngfs {
    // ngfs wraps the base object, which provides the filesystem interface 
    struct filesystem base; 

    // bkgio is the backing device for ngfs 
    struct io * bkgio; 

    // the cache for this filesystem 
    struct cache * cache; 

    // openfiles    
    struct ngfs_file * open_list; 

    // locks 
    struct rwlock ol_lock; 
    struct rwlock fat_lock; 
    struct rwlock root_lock;


    // metadata regarding start of data, and max block count 
    unsigned int total_blks; 
    unsigned int root_index; 
};

// the interface to a file is through the fileio object
// a file object will reference to a open-file object stored in the open-file linked list (open_list)
// an open file will have only one open-file, new opens to that file will create a new ioobject referencing the same open-file
struct ngfs_fileio {
    struct seekio io; 
    struct ngfs_file * file;
    struct ngfs * fs; 
};

struct ngfs_file {
    struct rwlock file_lock; 
    uint32_t size;
    uint32_t start_blk; 
    
    // ios can share a reference to a file 
    uint32_t refcnt; 
    struct ngfs_file * next; 
    char name[];
};

struct ngfs_lsio {
    struct io io; 
    struct ngfs * fs; 
    uint32_t dir_entry_index; 
};


// HELPER FUNCTIONS 
//

// FIND START BLOCK GETS THE INDEX OF _DATA_ BLOCKS, WHERE F IS 0 (F == ROOT DIR BLK OFFSET)
static uint32_t find_start_block(struct ngfs * ngfs, uint32_t start_blk, unsigned long long pos);

// GET NEXT DATA BLOCK RETURNS THE NEXT DATA INDEX, WHERE F IS 0
static uint32_t get_next_data_block(struct ngfs * ngfs, uint32_t cur_block);

static void set_next_data_block(struct ngfs * ngfs, uint32_t cur_block, uint32_t next_block);
static int64_t get_free_data_block(struct ngfs * ngfs);
static int ngfs_open_fileio(struct ngfs* ngfs, const char * name, struct io ** ioptr);
static int ngfs_open_lsio(struct ngfs* ngfs, struct io ** ioptr);
static void olappend(struct ngfs * ngfs, struct ngfs_file * file); 
static void olremove(struct ngfs * ngfs, const char *name); 
static struct ngfs_file * olfind(struct ngfs *ngfs, const char *name);
static int ngfs_find_root_entry(struct ngfs *ngfs, const char *name, struct ngfs_dir_entry *out, uint32_t* data_blk_no, uint32_t* entry_pos); 


// INTERNAL FUNCTION DECLARATIONS
//

static int ngfs_open(struct filesystem * fs, const char * name, struct io ** ioptr);
static int ngfs_create(struct filesystem * fs, const char * name);
static int ngfs_delete(struct filesystem * fs, const char * name); 
static void ngfs_flush(struct filesystem * fs);

static void ngfs_reclaim(struct io * io);
static long ngfs_fetch(struct io * io, unsigned long long pos, void * buf, long bufsz);
static long ngfs_read(struct io * io, void * buf, long bufsz);
static long ngfs_store(struct io * io, unsigned long long pos, const void * buf, long len);
static long ngfs_write(struct io * io, const void * buf, long len);
static int ngfs_ioctl(struct io * io, int op, void * arg);

static long ngfs_listing_read(struct io * lsio, void * buf, long bufsz);
static void ngfs_listing_reclaim(struct io * lsio);


// INTERNAL GLOBAL VARIABLES
//

// DISPATCH TABLES

struct filesystem ngfs = {
    .implname   = "ngfs",
    .createfile = &ngfs_create, 
    .deletefile = &ngfs_delete,
    .flush      = &ngfs_flush,
    .openfile   = &ngfs_open
};


static const struct iointf ngfs_file_intf = {
    .implname = "ngfs_fileio", 
    .reclaim  = &ngfs_reclaim,
    .fetch    = &ngfs_fetch,
    .store    = &ngfs_store,
    .read     = &ngfs_read,
    .write    = &ngfs_write,
    .ioctl    = &ngfs_ioctl
};


static const struct iointf ngfs_listing_intf = {
    .implname = "ngfs_lsio",
    .reclaim  = &ngfs_listing_reclaim, 
    .read     = &ngfs_listing_read
};

// HELPER FUNCTION DEFINITIONS 
// 

// This function finds the fat table entry of a file
// out - the fat table entry for our desired file 
// data_blk_no - the root block position in which the fat entry is located
// entry_pos - the offset within the root block 
int ngfs_find_root_entry(struct ngfs *ngfs, const char *name, struct ngfs_dir_entry *out, uint32_t* data_blk_no, uint32_t* entry_pos) {
    void * buf; 
    int result;

    // access the root block into buf 
    result = cache_fetch(ngfs->cache, ngfs->root_index * NGFS_BLKSZ, 0, &buf);     
    if(result < 0) {
        return result; 
    }

    struct ngfs_dir_entry * root_entries = buf; 
    struct ngfs_dir_entry root_dir_entry = root_entries[0];

    // determine number of files / entries inside the the filesystem
    uint32_t numberOfEntries = root_dir_entry.size / sizeof(struct ngfs_dir_entry); 

    // indexing to find the root entry of our desired file
    uint32_t entriesParsed = 0; 

    // the number of disks blocks we have traversed since the first root file block
    uint32_t block_offset = 0; 

    // the actual block position of the root block we want 
    uint32_t cur_datablock_index = 0;

    while(entriesParsed < numberOfEntries) {

        // offset within one block for file data entry in the root 
        uint32_t entry_offset = entriesParsed % NGFS_DIR_ENTRIES_BLOCK;

        // the current block of root we are parsing 
        uint32_t cur_block_offset = entriesParsed / NGFS_DIR_ENTRIES_BLOCK; 

        // if the previous block offset is not equal to the current block offset, we must fetch the next root block 
        if (block_offset != cur_block_offset) {
            cache_release(ngfs->cache, buf, 0);
            result = cache_fetch(
                ngfs->cache, 
                ((cur_datablock_index = get_next_data_block(ngfs, cur_datablock_index)) + ngfs->root_index) * NGFS_BLKSZ, 
                0, 
                &buf
            );     
            if(result < 0) {
                return result; 
            }
            root_entries = buf;
            block_offset = cur_block_offset; 
        }

        if(strcmp(root_entries[entry_offset].name, name) == 0) {
            if(out != NULL) {
                *out = root_entries[entry_offset];
            }
            if(data_blk_no != NULL) {
                *data_blk_no = cur_datablock_index;
            }
            if(entry_pos != NULL) {
                *entry_pos = entry_offset;
            }
            cache_release(ngfs->cache, buf, 0); 
            return entriesParsed; 
        } 
        entriesParsed++; 
    }

    // handle dangling cache_fetch iteration
    cache_release(ngfs->cache, buf, 0); 
    return -ENOENT; 
}

uint32_t find_start_block(struct ngfs * ngfs, uint32_t start_blk, unsigned long long pos) {
    uint32_t block_offset = pos / NGFS_BLKSZ; 
    uint32_t cur_blk = start_blk; 
    for(uint32_t i = 0; i < block_offset; i++) {
        cur_blk = get_next_data_block(ngfs, cur_blk); 
    }
    return cur_blk; 
}


void olappend(struct ngfs * ngfs, struct ngfs_file * file) {
    file->next = ngfs->open_list; 
    ngfs->open_list = file; 
}


// This function removes _name_ from the open file list
// This is called when by ngfs_reclaim to close a file that no longer has any fileio objects referencing it 
void olremove(struct ngfs * ngfs, const char *name) {
    struct ngfs_file * curr = ngfs->open_list;
    struct ngfs_file * prev = NULL; 

    // remove name from linked list 
    while(curr) {
        if(strcmp(curr->name, name) == 0) {
            if(prev == NULL) {
                ngfs->open_list = curr->next;
            } else {
                prev->next = curr->next;
            }
            kfree(curr); 
            return; 
        }
        
        prev = curr; 
        curr = curr->next; 
    }
}

// This function returns a pointer to an open_file with _name_ 
// This function is called by ngfs_open_filio to attach and fileio object to an open file
struct ngfs_file * olfind(struct ngfs *ngfs, const char *name) {
    struct ngfs_file * curr = ngfs->open_list;

    while(curr) {
        if (strcmp(curr->name, name) == 0) {
            return curr; 
        }
        curr = curr->next; 
    }
    return NULL; 
}

// returns the index of the next data block fromf the current block index 
uint32_t get_next_data_block(struct ngfs * ngfs, uint32_t cur_block) {
    void * buf; 
    uint32_t fat_index = cur_block / NGFS_FAT_ENTRIES_PER_BLOCK; 
    cache_fetch(ngfs->cache, fat_index * NGFS_BLKSZ, 0, &buf); 

    uint32_t * fat = buf; 
    uint32_t next_block = fat[cur_block % NGFS_FAT_ENTRIES_PER_BLOCK]; 
    cache_release(ngfs->cache, buf, 0); 
    return next_block; 
}

// this function sets the next data block, for handling block linked list traversal
void set_next_data_block(struct ngfs * ngfs, uint32_t cur_block, uint32_t next_block) { 
    void * buf; 
    uint32_t fat_index = cur_block / NGFS_FAT_ENTRIES_PER_BLOCK;
    cache_fetch(ngfs->cache, fat_index * NGFS_BLKSZ, 1, &buf); 

    uint32_t * fat = buf; 
    fat[cur_block % NGFS_FAT_ENTRIES_PER_BLOCK] = next_block; 
    cache_release(ngfs->cache, buf, 1); 
}

// this function returns the index of an free disk block 
int64_t get_free_data_block(struct ngfs * ngfs) {
    void * buf; 
    uint32_t cur_fat_entry = 0; 
    uint32_t fat_index = 0; 
    uint32_t * fat_entries; 
    int64_t result = -EIO; 

    if(cache_fetch(ngfs->cache, fat_index * NGFS_BLKSZ, 1, &buf) < 0) {
        return -EIO;
    }
    fat_entries = buf;
    while(cur_fat_entry < ngfs->total_blks) {
        uint32_t cur_fat_index = cur_fat_entry / NGFS_FAT_ENTRIES_PER_BLOCK; 
        uint32_t fat_index_offset = cur_fat_entry % NGFS_FAT_ENTRIES_PER_BLOCK; 
        
        if(cur_fat_index != fat_index) {
            cache_release(ngfs->cache, buf, 0); 
            if(cache_fetch(ngfs->cache, cur_fat_index * NGFS_BLKSZ, 1, &buf) < 0) {
                return -EIO; 
            } 
            fat_entries = buf; 
            fat_index = cur_fat_index; 
        }

        if(fat_entries[fat_index_offset] == NGFS_BLOCK_FREE) {
            result = cur_fat_entry; 
            break; 
        }
        cur_fat_entry++; 
    }
    cache_release(ngfs->cache, buf, 0); 
    return result; 
}


// mountpoints are directories that are linked to a particular file system
// the mountpoints list and filesystem generic interface give a VFS (virtual file system) where
// the filesystem details (FAT, ngfs, ext2) are abstracted away 
// see the filesystem.h for the interface
// bkgio is the backing device 
int mount_ngfs(const char * name, struct io * bkgio) {
    struct ngfs * fs; 
    unsigned long long bkgcap;
    int result; 
    uint32_t blksz;
    uint32_t numofblocks; 
    
    trace("%s()", __func__);

    fs = kcalloc(1, sizeof(*fs)); 
    
    // error handling, ensure backing device is as expected 
    blksz = ioblksz(bkgio); 
    if(NGFS_BLKSZ % blksz != 0) {
        debug("Incompatible device block size: %d", blksz);
        return -ENOTSUP;
    }

    trace("%s()", __func__);

    result = ioctl(bkgio, IOC_GETEND, &bkgcap);

    trace("%s()", __func__);

    if (result != 0) {
        debug("ioctl(IOC_GETEND) returned %d", result);
        return result;
    }

    numofblocks = bkgcap / NGFS_BLKSZ; 

    if (numofblocks == 0)
        return -ENOTSUP;

    // create the cache 
    fs->cache = create_cache(bkgio, NGFS_BLKSZ);  
    if(!fs->cache) 
        panic("create_cache() failed");


    // initialize struct fields and locks, then mount the created fs
    fs->bkgio = ioaddref(bkgio); 
    fs->base = ngfs; 
    fs->total_blks = numofblocks; 
    fs->open_list = NULL; 
    fs->root_index = (numofblocks + NGFS_FAT_ENTRIES_PER_BLOCK - 1) / NGFS_FAT_ENTRIES_PER_BLOCK;
    rwlock_init(&fs->fat_lock, NULL); 
    rwlock_init(&fs->ol_lock, NULL); 
    rwlock_init(&fs->root_lock, NULL); 
    result = mount_filesys(name, &fs->base); 
    return result;
}

int ngfs_open(struct filesystem * fs, const char * name, struct io ** ioptr) {

    // case 1: name == null or empty --> open a fs listing 
    // case 2: name != null --> open fileio 
    trace("%s()",  __func__);

    struct ngfs * const ngfs = (void*)fs;
    if(name == NULL || *name == '\0') {
        trace("%s()",  __func__);
        return ngfs_open_lsio(ngfs, ioptr);     
    } else if( strcmp(name, ".") != 0){
        trace("%s()",  __func__);
        return ngfs_open_fileio(ngfs, name, ioptr);
    }
    return -EACCESS;
}

int ngfs_open_fileio(struct ngfs* ngfs, const char * name, struct io ** ioptr) {
    struct ngfs_dir_entry desired_entry;
    int result; 
    struct ngfs_fileio * fio; 
    struct ngfs_file * f; 

    rwlock_acquire(&ngfs->root_lock, 0);
    result = ngfs_find_root_entry(ngfs, name, &desired_entry, NULL, NULL);    
    rwlock_release(&ngfs->root_lock);
  
    if(result < 0) {
        return result; 
    }

    fio = kcalloc(1, sizeof(*fio)); 
    if(!fio) {
        return -ENOMEM;
    }

    // RETURN IO OBJECT ON FILE HERE 
    *ioptr = seekio_init(&fio->io, &ngfs_file_intf, 1, 1);
    fio->fs = ngfs; 

    // case 1: file is already open --> attach new io object to file in openfile
    rwlock_acquire(&ngfs->ol_lock, 1); // mutual exlcusion on open_list
    if( ( f = olfind(ngfs, desired_entry.name) ) ) {
        fio->file = f;
        f->refcnt++; 
        rwlock_release(&ngfs->ol_lock); 
        return 0; 
    }

    // case 2: file is not open --> allocate file --> attach to new io object --> append to open_list
    // since name[] is a flexible length member of the struct 
    f = kcalloc(1, sizeof(*f) + strlen(desired_entry.name) + 1); 
    if(!f) {
        kfree(fio); 
        rwlock_release(&ngfs->ol_lock); 
        return -ENOMEM;
    }
    fio->file = f; 

    // intialize file data
    f->refcnt = 1;
    f->next = NULL; 
    f->size = desired_entry.size; 
    f->start_blk = desired_entry.start_block; 

    rwlock_init(&f->file_lock, NULL); 
    strncpy(f->name, desired_entry.name, strlen(desired_entry.name) + 1); 

    // append to the open file list
    olappend(ngfs, f); 
    rwlock_release(&ngfs->ol_lock); 

    return 0; 
}

int ngfs_open_lsio(struct ngfs* ngfs, struct io ** ioptr) {
    struct ngfs_lsio * lsio; 
    lsio = kcalloc(1, sizeof(*lsio)); 
    if(!lsio) {
        return -ENOMEM; 
    }
    lsio->dir_entry_index = 0; 
    lsio->fs = ngfs; 
    *ioptr = ioinit(&lsio->io, &ngfs_listing_intf, 1, 1); 
    return 0; 
}

void ngfs_reclaim(struct io * io) {
    struct ngfs_fileio * fileio = (void*)io - offsetof(struct ngfs_fileio, io);
    struct ngfs * ngfs = fileio->fs; 
    
    // acquire a lock on the open-file linked list 
    rwlock_acquire(&ngfs->ol_lock, 1); 

    // drop the reference count to the open-file object, since an fileio object is being destroyed 
    fileio->file->refcnt--; 

    // if there are no more fileio objects referencing that open-file object, clean up the open file
    if(fileio->file->refcnt == 0) {
        olremove(fileio->fs, fileio->file->name); 
    } 
    rwlock_release(&ngfs->ol_lock);
    kfree(fileio); 
}

long ngfs_fetch(struct io * io, unsigned long long pos, void * buf, long bufsz) {
    struct ngfs_fileio * fileio = (void*)io - offsetof(struct ngfs_fileio, io); 
    struct ngfs_file * file = fileio->file; 
    void * block_buf; 
    int result; 
    int startup_case = 0; 
    uint32_t bytes_to_read;
    uint32_t bytes_read = 0; 

    // error handling for bufsz 
    if (bufsz < 0) {
        return -EINVAL;
    } else if(bufsz == 0) {
        return 0; 
    }

    // acquire lock on the open-file object 
    rwlock_acquire(&file->file_lock, 0); 
    uint32_t filesz = fileio->file->size;

    // error handling for position of the read 
    if(pos > filesz) {
        rwlock_release(&file->file_lock); 
        return -EIO; 
    }

    if(pos == filesz) {
        rwlock_release(&file->file_lock); 
        return 0; 
    }

    if( bufsz > filesz - pos ) {
        bytes_to_read = filesz - pos; 
    } else {
        bytes_to_read = bufsz;
    }

    // get the starting block for the file 
    uint32_t cur_block_index = find_start_block(fileio->fs, fileio->file->start_blk, pos);
    result = cache_fetch(fileio->fs->cache,  (fileio->fs->root_index + cur_block_index) * NGFS_BLKSZ, 0, &block_buf); 
    
    if(result < 0) {
        rwlock_release(&file->file_lock);
        return -EIO; 
    }
    
    while (bytes_read < bytes_to_read) {
        // handle the startup case since, there can be partially filled data
        if (!startup_case) {
            uint32_t first_blk_avail = NGFS_BLKSZ - pos % NGFS_BLKSZ;
            uint32_t remaining = bytes_to_read - bytes_read;
            uint32_t bytes_to_copy;

            if (remaining < first_blk_avail) {
                bytes_to_copy = remaining;
            } else {
                bytes_to_copy = first_blk_avail;
            }

            memcpy(buf + bytes_read, block_buf + pos % NGFS_BLKSZ, bytes_to_copy);

            startup_case = 1;
            bytes_read += bytes_to_copy;

            trace("after copy: bytes_read=%u startup_case=%d\n",
                bytes_read, startup_case);

            continue;
        }

        // get next data block
        cache_release(fileio->fs->cache, block_buf, 0); 
        result = cache_fetch(
            fileio->fs->cache, 
            ((cur_block_index = get_next_data_block(fileio->fs, cur_block_index)) + fileio->fs->root_index ) * NGFS_BLKSZ, 
            0, 
            &block_buf
        ); 
        if(result < 0) {
            rwlock_release(&file->file_lock); 
            return -EIO; 
        }

        // three cases:
        // 1 bytes to read is > 512 
        // 2 bytes to read is < 512
        // 3 bytes to read is == 512 
        if(bytes_to_read - bytes_read > NGFS_BLKSZ) {
            memcpy(buf + bytes_read, block_buf, NGFS_BLKSZ); 
            bytes_read += NGFS_BLKSZ; 
        } else if (bytes_to_read - bytes_read < NGFS_BLKSZ) {
            memcpy(buf + bytes_read, block_buf, bytes_to_read - bytes_read); 
            bytes_read += bytes_to_read - bytes_read; 
        } else {
            memcpy(buf + bytes_read, block_buf, NGFS_BLKSZ);
            bytes_read += NGFS_BLKSZ; 
            break; 
        }
    }
    cache_release(fileio->fs->cache, block_buf, 0); 
    rwlock_release(&file->file_lock); 

    return bytes_read; 
}

long ngfs_read(struct io * io, void * buf, long bufsz) {
    return seekio_read(io, buf, bufsz); 
}

long ngfs_store(struct io * io, unsigned long long pos, const void * buf, long len) {
    trace("%s()", __func__); 
    struct ngfs_fileio * fileio = (void*)io - offsetof(struct ngfs_fileio, io); 
    struct ngfs_file * file = fileio->file; 
    void * block_buf; 
    int result; 
    int startup_case = 0;
    uint32_t bytes_to_write;
    uint32_t bytes_written = 0; 

    // error handling for invalid length
    if (len < 0) {
        return -EINVAL;
    } else if(len == 0) {
        return 0; 
    }
    
    // acquire lock over the open-file object 
    rwlock_acquire(&file->file_lock, 1); 
    uint32_t filesz = fileio->file->size;


    // error handling for invalid position and length 
    if(pos > filesz) {
        rwlock_release(&file->file_lock); 
        return -EIO; 
    }

    if(pos == filesz) {
        rwlock_release(&file->file_lock); 
        return 0; 
    }

    if( len > filesz - pos ) {
        bytes_to_write = filesz - pos; 
    } else {
        bytes_to_write = len;
    }

    trace("%s()", __func__); 
    // get the starting block of the file we are writing to 
    uint32_t cur_block_index =  find_start_block(fileio->fs, file->start_blk, pos); 
    result = cache_fetch(fileio->fs->cache, (fileio->fs->root_index + cur_block_index) * NGFS_BLKSZ, 1, &block_buf); 
    
    if(result < 0) {
        rwlock_release(&file->file_lock);
        return -EIO; 
    }

    while(bytes_written < bytes_to_write) {
        // treat the first block as special since the start block may be partially full
        if(!startup_case) {
            // determine number of bytes remaining in the current block 
            uint32_t first_blk_remaining = NGFS_BLKSZ - pos % NGFS_BLKSZ; 
            uint32_t remaining = bytes_to_write - bytes_written;
            uint32_t bytes_to_copy; 

            // if bytes remaining in the block is greater than number of bytes we need to write
            // we copy the all the unwritten bytes 
            if(remaining < first_blk_remaining) {
                bytes_to_copy = remaining;
            } else {
                bytes_to_copy = first_blk_remaining; 
            }

            memcpy(block_buf + pos % NGFS_BLKSZ, buf + bytes_written, bytes_to_copy);
            startup_case = 1; 
            bytes_written += bytes_to_copy; 
            continue; 
        }

        // fetch the next data block 
        cache_release(fileio->fs->cache, block_buf, 1); 
        result = cache_fetch(
            fileio->fs->cache, 
            ((cur_block_index = get_next_data_block(fileio->fs, cur_block_index)) + fileio->fs->root_index ) * NGFS_BLKSZ, 
            1, 
            &block_buf
        ); 
        if(result < 0) {
            rwlock_release(&file->file_lock); 
            return -EIO; 
        }

        // three cases: 
        // 1 we need to write > 512 bytes 
        // 2 we need to write < 512 bytes 
        // 3 we need to write == 512 bytes, break loop 
        if(bytes_to_write - bytes_written > NGFS_BLKSZ) {
            memcpy(block_buf, buf + bytes_written, NGFS_BLKSZ); 
            bytes_written += NGFS_BLKSZ; 
        } else if (bytes_to_write - bytes_written < NGFS_BLKSZ) {
            memcpy(block_buf, buf + bytes_written, bytes_to_write - bytes_written); 
            bytes_written += bytes_to_write - bytes_written; 
        } else {
            memcpy(block_buf, buf + bytes_written, NGFS_BLKSZ);
            bytes_written += NGFS_BLKSZ; 
            break; 
        }
    }
        trace("%s()", __func__); 

    cache_release(fileio->fs->cache, block_buf, 1); 
    rwlock_release(&file->file_lock); 

    return bytes_written; 
}

long ngfs_write(struct io * io, const void * buf, long len) {
    return seekio_write(io, buf, len); 
}

int ngfs_ioctl(struct io * io, int op, void * arg) {
    struct ngfs_fileio * fileio = (void*) io - offsetof(struct ngfs_fileio, io); 
    struct ngfs_file * file = fileio->file; 

    switch(op) {
        case IOC_GETPOS: 
            if(!arg) {
                return -EINVAL; 
            }
            return seekio_ioctl(io, IOC_GETPOS, arg); 

        case IOC_SETPOS:
            if(!arg) {
                return -EINVAL; 
            }
            return seekio_ioctl(io, IOC_SETPOS, arg);

        case IOC_GETEND:
            if(!arg) {
                return -EINVAL; 
            }
            rwlock_acquire(&file->file_lock, 0); 
            *(unsigned long long*) arg  = file->size; 
            rwlock_release(&file->file_lock);
            return 0; 

        case IOC_SETEND:        
            if(!arg) {
                return -EINVAL; 
            }
            rwlock_acquire(&file->file_lock, 1); 
            uint32_t cur_filesz = file->size; 

            // ensure that desired position is valid [0, uint32_t size]
            unsigned long long req = *(unsigned long long *)arg;
            if (req > UINT32_MAX) {
                rwlock_release(&file->file_lock);
                return -EINVAL;
            }
            uint32_t new_filesz = (uint32_t)req;

            uint32_t new_block_chain_len = (new_filesz + NGFS_BLKSZ - 1) / NGFS_BLKSZ; 
            uint32_t curr_block_chain_len = (cur_filesz + NGFS_BLKSZ - 1) / NGFS_BLKSZ; 
            
            // case 1: new file length < current length 
            rwlock_acquire(&fileio->fs->fat_lock, 1);
            if(new_filesz < cur_filesz) {
                // case 1: new_filesz == 0 
                if (new_filesz == 0) {
                    uint32_t cur_blk = file->start_blk; 
                    file->start_blk = NGFS_BLOCK_END; 

                    // delete free all blocks
                    while(cur_blk != NGFS_BLOCK_END) {
                        uint32_t next_blk = get_next_data_block(fileio->fs, cur_blk);
                        set_next_data_block(fileio->fs, cur_blk, NGFS_BLOCK_FREE);
                        cur_blk = next_blk; 
                    }
                } else {
                    uint32_t new_end_block = file->start_blk; 

                    // find the new end block of the list 
                    for(uint32_t i = 1; i < new_block_chain_len; i++) {
                        new_end_block = get_next_data_block(fileio->fs, new_end_block);
                    }

                    // free the remainder of the list 
                    uint32_t first_free_blk = get_next_data_block(fileio->fs, new_end_block);
                    assert(new_end_block != NGFS_BLOCK_END);
                    set_next_data_block(fileio->fs, new_end_block, NGFS_BLOCK_END); 
                    uint32_t cur_blk = first_free_blk; 

                    while(cur_blk != NGFS_BLOCK_END) {
                        uint32_t next_blk = get_next_data_block(fileio->fs, cur_blk);
                        set_next_data_block(fileio->fs, cur_blk, NGFS_BLOCK_FREE);
                        cur_blk = next_blk; 
                    }
                }
            // case 2: new file length > current length
            } else if (new_filesz > cur_filesz) {

                // Find the last block in the current chain 
                uint32_t last_blk = file->start_blk; 
                for(uint32_t i = 1; i < curr_block_chain_len; i++) {
                    last_blk = get_next_data_block(fileio->fs, last_blk);
                }

                // If a file partially fills up the last block in its chain, then fill the remaining 
                if( new_filesz > 0 && (cur_filesz % NGFS_BLKSZ) != 0) {
                    
                    // calculate position inside block
                    uint32_t offset_old = cur_filesz % NGFS_BLKSZ;

                    // get the block with trailing data to write 
                    void * buf; 
                    int result = cache_fetch(fileio->fs->cache, (fileio->fs->root_index + last_blk) * NGFS_BLKSZ, 1, &buf);
                    if( result < 0 ) {
                        rwlock_release(&file->file_lock);
                        rwlock_release(&fileio->fs->fat_lock);
                        return result; 
                    }
                
                    memset(buf + offset_old, 0, NGFS_BLKSZ - offset_old); 
                    cache_release(fileio->fs->cache, buf, 1); 
                }

                // create the new blocks 
                uint32_t cur_blk = last_blk; 
                int64_t next_blk; 
                for(uint32_t i = 0; i < new_block_chain_len - curr_block_chain_len; i++) {

                    // find the next free block 
                    next_blk = get_free_data_block(fileio->fs);
                    if(next_blk < 0) {
                        rwlock_release(&file->file_lock);
                        rwlock_release(&fileio->fs->fat_lock);
                        return next_blk; 
                    }

                    // set the next_data_block to END 
                    assert(next_blk != NGFS_BLOCK_END);
                    set_next_data_block(fileio->fs, next_blk, NGFS_BLOCK_END);

                    // fill the new block with 0 
                    void * buf; 
                    int result = cache_fetch(fileio->fs->cache, (fileio->fs->root_index + next_blk) * NGFS_BLKSZ, 1, &buf);
                    if( result < 0 ) {
                        rwlock_release(&file->file_lock);
                        rwlock_release(&fileio->fs->fat_lock);
                        return result; 
                    }

                    memset(buf, 0, NGFS_BLKSZ); 
                    cache_release(fileio->fs->cache, buf, 1); 
                    
                    // Attach next_blk to current block or set next_blk to current block (last_blk) if filesz was 0
                    if(cur_blk == NGFS_BLOCK_END) {
                        file->start_blk = next_blk; 
                    } else {
                        set_next_data_block(fileio->fs, cur_blk, next_blk);
                    }
                    cur_blk = next_blk; 
                }

            } else {
                rwlock_release(&file->file_lock);
                rwlock_release(&fileio->fs->fat_lock);
                return 0; 
            }
            rwlock_release(&fileio->fs->fat_lock);

            file->size = new_filesz; 

            // update the metadata in the root block 
            
            rwlock_acquire(&fileio->fs->root_lock, 1); 
            void * buf; 
            struct ngfs_dir_entry * entries; 
            uint32_t entry_off;
            uint32_t data_block_pos;
            int result = ngfs_find_root_entry(fileio->fs, file->name, NULL, &data_block_pos, &entry_off); 
            trace("reuslt ======= %d", result); 
            if(result < 0) {
                rwlock_release(&fileio->fs->root_lock);
                rwlock_release(&file->file_lock);
                return result; 
            }

            result = cache_fetch(fileio->fs->cache, (fileio->fs->root_index + data_block_pos) * NGFS_BLKSZ, 1,  &buf); 
            if(result < 0) {
                rwlock_release(&fileio->fs->root_lock);
                rwlock_release(&file->file_lock);
                return result; 
            }
            entries = (struct ngfs_dir_entry *) buf; 
            trace("reuslt ======= %u", entries[entry_off].start_block); 
            entries[entry_off].start_block = file->start_blk;
            entries[entry_off].size = file->size;
            cache_release(fileio->fs->cache, buf, 1);
            rwlock_release(&fileio->fs->root_lock);
            rwlock_release(&file->file_lock);
            return 0; 
            
        default:
            return -ENOTSUP; 
    }
}

int ngfs_create(struct filesystem * fs, const char * name) {
    struct ngfs * ngfs = (void*) fs - offsetof(struct ngfs, base); 
    struct ngfs_dir_entry root_entry; 
    uint32_t new_dir_index;
    void * cache_buf; 
    struct ngfs_dir_entry * entries; 

    if(strlen(name)  > NGFS_MAX_FILENAME_LEN) {
        return -EINVAL;
    }

    // Lock root file
    rwlock_acquire(&ngfs->root_lock, 1); 

    // Check if file exists 
    int result = ngfs_find_root_entry(ngfs, name, NULL, NULL, NULL);
    if(result >= 0) {
        rwlock_release(&ngfs->root_lock); 
        trace("FILE ALREADY EXISTS");
        return -EEXIST; 
    }

    // access disk / cache for root directory to write & get root information
    rwlock_acquire(&ngfs->fat_lock, 1); 
    result = cache_fetch(ngfs->cache, ngfs->root_index * NGFS_BLKSZ, 1, &cache_buf);
    if(result < 0) {
        rwlock_release(&ngfs->fat_lock); 
        rwlock_release(&ngfs->root_lock); 
        return result; 
    }

    // Check if we need to allocate more space for the root dir file 
    entries = (struct ngfs_dir_entry *) cache_buf; 
    root_entry = entries[0]; 
    uint32_t oldsize = root_entry.size;
    entries[0].size += NGFS_DENSZ; 
    uint32_t newsize = entries[0].size;
    uint32_t start_blk = entries[0].start_block; 
    cache_release(ngfs->cache, cache_buf, 1); 
 
    // we we are at a boundary, extend the file size
    if((oldsize + NGFS_BLKSZ - 1) / NGFS_BLKSZ != (newsize + NGFS_BLKSZ - 1) / NGFS_BLKSZ) {
        uint32_t cur = start_blk;
        uint32_t next = get_next_data_block(ngfs, cur);
        while(next != NGFS_BLOCK_END) {
            cur = next; 
            next = get_next_data_block(ngfs, cur);
        }

        int64_t result = get_free_data_block(ngfs); 
        if(result < 0) {
            rwlock_release(&ngfs->root_lock); 
            rwlock_release(&ngfs->fat_lock); 
            return result; 
        } 
        trace("%u ===== result", result); 
        set_next_data_block(ngfs, (uint32_t) result, NGFS_BLOCK_END);
        set_next_data_block(ngfs, cur, (uint32_t) result);
    }
    

    // Get index of new dir entry index in the root file 
    new_dir_index = root_entry.size / NGFS_DENSZ; 
    uint32_t data_block_pos = root_entry.start_block;
    uint32_t curr_block_chain_len = (newsize + NGFS_BLKSZ - 1) / NGFS_BLKSZ;
    for(uint32_t i = 1; i < curr_block_chain_len; i++) {
        data_block_pos = get_next_data_block(ngfs, data_block_pos);
    }

    // Write to the root file with that memory 
    result = cache_fetch(ngfs->cache, (ngfs->root_index + data_block_pos) * NGFS_BLKSZ, 1, &cache_buf);
    if(result < 0) {
        rwlock_release(&ngfs->fat_lock); 
        rwlock_release(&ngfs->root_lock); 
        return result; 
    }
    entries = (struct ngfs_dir_entry *) cache_buf; 
    entries[new_dir_index % NGFS_DIR_ENTRIES_BLOCK].start_block = NGFS_BLOCK_END; 
    entries[new_dir_index % NGFS_DIR_ENTRIES_BLOCK].size = 0; 
    strncpy(entries[new_dir_index % NGFS_DIR_ENTRIES_BLOCK].name, name, strlen(name) + 1); 
    cache_release(ngfs->cache, cache_buf, 1); 

    rwlock_release(&ngfs->fat_lock); 
    rwlock_release(&ngfs->root_lock); 
    return 0; 
}

int ngfs_delete(struct filesystem * fs, const char * name) {
    struct ngfs * ngfs = (void*) fs - offsetof(struct ngfs, base); 
    struct ngfs_dir_entry root_entry; 
    struct ngfs_dir_entry delete_entry; 
    struct ngfs_dir_entry back_entry; 
    void * cache_buf; 
    struct ngfs_dir_entry * entries; 

    // we can't delete the root file 
    if(strcmp(name, ".") == 0) {
        return -EINVAL;
    }

    // validate file name 
    if(strlen(name)  > NGFS_MAX_FILENAME_LEN) {
        return -EINVAL;
    }

    // lock the root file 
    rwlock_acquire(&ngfs->root_lock, 1); 

    // CHECK IF FILE EXISTS 
    uint32_t delete_block_pos, delete_block_offset;
    int result = ngfs_find_root_entry(ngfs, name, &delete_entry, &delete_block_pos, &delete_block_offset);
    if(result < 0) {
        rwlock_release(&ngfs->root_lock); 
        return -ENOENT; 
    }
    
    // FETCH INFO FROM THE ROOT DIR ENTRY 
    result = ngfs_find_root_entry(ngfs, ".", &root_entry, NULL, NULL);
    if(result < 0) {
        rwlock_release(&ngfs->root_lock); 
        return result; 
    }
    
    // Get the block_index of the last entry 
    rwlock_acquire(&ngfs->fat_lock, 1);
    
    // Get the last block position, entry offset for the last file entry in root 
    uint32_t last_entry = root_entry.size / NGFS_DENSZ - 1; 
    uint32_t curr_block_chain_len = last_entry / NGFS_DIR_ENTRIES_BLOCK + 1; 
    uint32_t last_block_pos = root_entry.start_block;
    for(int i = 1; i < curr_block_chain_len; i++) {
        last_block_pos = get_next_data_block(ngfs, last_block_pos);
    }

    // Get the data block using the computed block position
    result = cache_fetch(ngfs->cache, (ngfs->root_index + last_block_pos) * NGFS_BLKSZ, 0, &cache_buf);
    if(result < 0) {
        rwlock_release(&ngfs->fat_lock);
        rwlock_release(&ngfs->root_lock); 
        return result; 
    }

    // store the data from the last block 
    entries = (struct ngfs_dir_entry *) cache_buf; 
    back_entry = entries[last_entry % NGFS_DIR_ENTRIES_BLOCK]; 
    cache_release(ngfs->cache, cache_buf, 0); 

    // fetch the block holding the data with the entry to be deleted
    result = cache_fetch(ngfs->cache, (ngfs->root_index + delete_block_pos) * NGFS_BLKSZ, 1, &cache_buf);
    if(result < 0) {
        rwlock_release(&ngfs->fat_lock);
        rwlock_release(&ngfs->root_lock); 
        return result; 
    }

    // replace the entry to be deleted with the data from the last block 
    entries = (struct ngfs_dir_entry *) cache_buf; 
    entries[delete_block_offset] = back_entry; 
    cache_release(ngfs->cache, cache_buf, 1); 

    // store current and next size (after delete) 
    uint32_t oldsize = root_entry.size;
    uint32_t newsize = oldsize - NGFS_DENSZ; 

    // Fetch the first root block 
    result = cache_fetch(ngfs->cache, ngfs->root_index  * NGFS_BLKSZ, 1, &cache_buf);
    if(result < 0) {
        rwlock_release(&ngfs->root_lock); 
        rwlock_release(&ngfs->fat_lock);
        return result; 
    }

    // decrease the size of the root file 
    entries = (struct ngfs_dir_entry *) cache_buf; 
    entries[0].size -= NGFS_DENSZ; 
    cache_release(ngfs->cache, cache_buf, 1); 

    // store block counts 
    uint32_t old_blocks = (oldsize + NGFS_BLKSZ - 1) / NGFS_BLKSZ; 
    uint32_t new_blocks = (newsize + NGFS_BLKSZ - 1) / NGFS_BLKSZ; 

    // check if we need to update the FAT table for root dir 
    if( old_blocks != new_blocks ) {
        set_next_data_block(ngfs, last_block_pos, NGFS_BLOCK_FREE);
        last_block_pos = root_entry.start_block;
        for(int i = 1; i < curr_block_chain_len - 1; i++) {
            last_block_pos = get_next_data_block(ngfs, last_block_pos);
        }
        set_next_data_block(ngfs, last_block_pos, NGFS_BLOCK_END); 
    }

    uint32_t cur_blk = delete_entry.start_block; 

    // delete free all blocks in deleted file 
    while(cur_blk != NGFS_BLOCK_END) {
        uint32_t next_blk = get_next_data_block(ngfs, cur_blk);
        set_next_data_block(ngfs, cur_blk, NGFS_BLOCK_FREE);
        cur_blk = next_blk; 
    }
    rwlock_release(&ngfs->fat_lock);
    rwlock_release(&ngfs->root_lock); 

    return 0; 
}

void ngfs_flush(struct filesystem * fs) {
    struct ngfs * ngfs = (void*) fs - offsetof(struct ngfs, base); 
    cache_flush(ngfs->cache);
}

long ngfs_listing_read(struct io * lsio, void * buf, long bufsz) {
    struct ngfs_lsio * ls = (void*) lsio - offsetof(struct ngfs_lsio, io);
    struct ngfs_dir_entry root_entry; 
    void * cache_buf; 
    struct ngfs_dir_entry * entries; 
    long bytes_read; 

    // check for invalid buffer sizes
    if (bufsz < 0) {
        return -EINVAL;
    }

    if (bufsz == 0) {
        return 0;
    }

    // acquire a lock over the root file since we are reading to to parse directory entries 
    rwlock_acquire(&ls->fs->root_lock, 0); 

    // get the root entry data
    int result = ngfs_find_root_entry(ls->fs, ".", &root_entry, NULL, NULL);
    if(result < 0) {
        rwlock_release(&ls->fs->root_lock); 
        return result; 
    }

    // determine whether the dir_entry_index is = number of files or greater than -> indicates that we are done
    if(root_entry.size / NGFS_DENSZ <= ls->dir_entry_index) {
        rwlock_release(&ls->fs->root_lock); 
        return 0; 
    } else {
        // iterate through to find the correct disk block to read from 
        uint32_t curr_blk = root_entry.start_block; 
        for(int i = 0; i < ls->dir_entry_index / NGFS_DIR_ENTRIES_BLOCK; i++) {
            curr_blk = get_next_data_block(ls->fs, curr_blk); 
        }

        // fetch the disk block
        int result = cache_fetch(ls->fs->cache, (ls->fs->root_index + curr_blk) * NGFS_BLKSZ, 0, &cache_buf);
        if(result < 0) {
            rwlock_release(&ls->fs->root_lock);
            return result; 
        }

        // convert into usable formot and read the name of the file 
        entries = (struct ngfs_dir_entry *) cache_buf;
        struct ngfs_dir_entry curr = entries[ls->dir_entry_index % NGFS_DIR_ENTRIES_BLOCK];
        bytes_read = MIN(bufsz, strlen(curr.name));
        memcpy(buf, curr.name, bytes_read);
        ls->dir_entry_index++; 
        cache_release(ls->fs->cache, cache_buf, 0);
    }
    rwlock_release(&ls->fs->root_lock);
    return bytes_read; 
}

void ngfs_listing_reclaim(struct io * lsio) {
    kfree(lsio); 
}