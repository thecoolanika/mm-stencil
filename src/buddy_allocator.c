#include "buddy_allocator.h"

#include "sys/mem/mem.h"

#include "util/ktypes.h"
#include "util/string.h"

#include <stddef.h>
#include <stdint.h>

// 4k, 8k, 16k, 32k, 64k
#define NUM_ORDERS 5

// returns the start and end addresses of the usable memory region
static void pool_bounds(uintptr_t *lo, uintptr_t *hi) {
    uintptr_t start = ((uintptr_t)free_ptr + MAX_CHUNK_SIZE - 1) &
                      ~((uintptr_t)MAX_CHUNK_SIZE - 1);
    uintptr_t end = (uintptr_t)dram + (size_t)mem_size;

    *lo = start;
    *hi = end > start ? start + ((end - start) / MAX_CHUNK_SIZE) * MAX_CHUNK_SIZE
                      : start;
}

// returns 1 if the address range fits inside the usable memory region
static int in_pool(uintptr_t addr, uintptr_t bytes) {
    uintptr_t lo, hi;

    pool_bounds(&lo, &hi);
    return addr >= lo && addr <= hi && bytes <= hi - addr;
}

// returns 1 if address range overlaps any existing free block.
static int overlaps_free(uintptr_t addr, uintptr_t bytes) {
    // check every size list for a block with an address range that overlaps this one
    for (int list = 0; list < NUM_ORDERS; ++list) {
        uintptr_t list_bytes = (uintptr_t)PAGE_SIZE << list;
        for (chunk_t *node = free_list->lists[list]; node; node = node->next) {
            uintptr_t free_addr = (uintptr_t)node->addr;
            // two ranges overlap if each starts before the other one ends
            if (addr < free_addr + list_bytes && free_addr < addr + bytes)
                return 1;
        }
    }
    return 0;
}

// frees a block, merges it with any adjacent free buddies, and adds it to the free list
static void release_block(int index, uintptr_t addr) {
    uintptr_t block_bytes = (uintptr_t)PAGE_SIZE << index;

    // buddy has different address bit representing this block size
    while (index < NUM_ORDERS - 1) {
        uintptr_t buddy_addr = addr ^ block_bytes;
        chunk_t **link = &free_list->lists[index];
        while (*link && (uintptr_t)(*link)->addr != buddy_addr)
            link = &(*link)->next;
        // if the buddy is not free, the block can't merge any more
        if (!*link) break;

        // unlink the buddy since it will be part of our combined block now
        chunk_t *buddy = *link;
        *link = buddy->next;
        buddy->addr = NULL;
        buddy->next = all_chunks;
        all_chunks = buddy;

        // the merged block starts at whichever buddy has the lower address
        if (buddy_addr < addr) addr = buddy_addr;
        // we move to the next size list and double the size before checking again
        ++index;
        block_bytes <<= 1;
    }

    if (all_chunks) {
        chunk_t *node = all_chunks;
        all_chunks = node->next;
        // record the block address and link it at the front of its size list
        node->addr = (void *)addr;
        node->next = free_list->lists[index];
        free_list->lists[index] = node;
    }
}

void buddy_init() {
    slab_init(); 
    spinlock_init(&mm_buddy_mutex);

    // precomputing chunk count 
    long meta = sizeof(free_list_t) + NUM_ORDERS * sizeof(chunk_t *);
    long nchunks = (mem_size - meta) / (PAGE_SIZE + sizeof(chunk_t));

    // buddy slab = the free list, chunk slab = the chunk nodes
    slab_allocator_init(&buddy_slab_allocator, sizeof(free_list_t));
    slab_allocator_init(&chunk_slab_allocator, sizeof(chunk_t));

    free_list = slab_obj_alloc(buddy_slab_allocator);
    free_list->lists = free_ptr; // one head per size
    free_ptr += NUM_ORDERS * sizeof(chunk_t *);
    for (long i = 0; i < NUM_ORDERS; i++)
        free_list->lists[i] = NULL;

    // spare nodes on all_chunks until a free list needs them
    all_chunks = NULL;
    for (long i = 0; i < nchunks; i++) {
        chunk_t *c = slab_obj_alloc(chunk_slab_allocator);
        c->addr = NULL;
        c->next = all_chunks;
        all_chunks = c;
    }

    // section heap into blocks of MAX_CHUNK_SIZE
    unsigned long heap = (unsigned long)free_ptr;
    unsigned long end = (unsigned long)dram + mem_size;
    if (heap % MAX_CHUNK_SIZE)
        heap += MAX_CHUNK_SIZE - (heap % MAX_CHUNK_SIZE);

    while (heap + MAX_CHUNK_SIZE <= end && all_chunks) {
        chunk_t *c = all_chunks;
        all_chunks = c->next;
        c->addr = (void *)heap;
        c->next = free_list->lists[NUM_ORDERS - 1];
        free_list->lists[NUM_ORDERS - 1] = c;
        heap += MAX_CHUNK_SIZE;
    }
}

void *buddy_obj_alloc(long size) {
    // ignore a size this allocator cannot represent as one chunk
    if (size <= 0 || size > MAX_CHUNK_SIZE)
        return NULL;

    // round up to the smallest free-list index that can hold the request -- evey block at least 1 page
    long order = 0;
    long block_size = PAGE_SIZE;
    while (block_size < size) {
        block_size *= 2;
        order++;
    }

    spinlock_lock(&mm_buddy_mutex);

    // if that list is empty, go to a bigger one until larger free block exists
    long found = order;
    while (found < NUM_ORDERS && free_list->lists[found] == NULL)
        found++;
    if (found == NUM_ORDERS) {
        spinlock_unlock(&mm_buddy_mutex);
        return NULL;
    }

    // take that block off the free list
    chunk_t *block = free_list->lists[found];
    free_list->lists[found] = block->next;

    // split the block in half until it is the requested size --upper half goes back on the free list for smaller size
    while (found > order) {
        if (!all_chunks) {
            block->next = free_list->lists[found];
            free_list->lists[found] = block;
            spinlock_unlock(&mm_buddy_mutex);
            return NULL;
        }
        found--;
        chunk_t *buddy = all_chunks;
        all_chunks = buddy->next;
        buddy->addr = (char *)block->addr + (PAGE_SIZE << found);
        buddy->next = free_list->lists[found];
        free_list->lists[found] = buddy;
    }

    // node not free anymore: return it to the spare pile
    void *addr = block->addr;
    block->addr = NULL;
    block->next = all_chunks;
    all_chunks = block;

    spinlock_unlock(&mm_buddy_mutex);
    return addr;
}

void buddy_obj_free(long size, void *obj) {
    uintptr_t addr, block_bytes;
    int index = 0;
    size_t bytes = PAGE_SIZE;

    // ignore invalid requests
    if (!obj || size <= 0 || size > MAX_CHUNK_SIZE || !dram || !free_list) return;

    // start with the smallest block, one page, and double until it can hold size
    while (bytes < (size_t)size) {
        bytes <<= 1;
        ++index;
    }
  
    block_bytes = bytes;

    // the full block must fit in the managed area and start at its size boundary
    addr = (uintptr_t)obj;
    if (!in_pool(addr, block_bytes) || addr % block_bytes != 0) return;

    spinlock_lock(&mm_buddy_mutex);

    // ignore duplicate frees and frees that overlap a free block
    if (!overlaps_free(addr, block_bytes))
        release_block(index, addr);

    spinlock_unlock(&mm_buddy_mutex);
}

void *page_alloc_n(int npages) {
    if (npages <= 0)
        return NULL;

    long bytes = (long)npages * PAGE_SIZE;

    // fits in one chunk -- same path as obj alloc
    if (bytes <= MAX_CHUNK_SIZE)
        return buddy_obj_alloc(bytes);

    // bigger than MAX_CHUNK_SIZE-> need multiple adjacent blocks
    long max_pages = MAX_CHUNK_SIZE / PAGE_SIZE;
    long need = (npages + max_pages - 1) / max_pages;

    spinlock_lock(&mm_buddy_mutex);

    // search the largest free list for enough adjacent blocks
    chunk_t *start = free_list->lists[NUM_ORDERS - 1];
    while (start) {
        int ok = 1;
        for (long k = 1; k < need; k++) {
            void *want = (char *)start->addr + k * MAX_CHUNK_SIZE;
            chunk_t *scan = free_list->lists[NUM_ORDERS - 1];
            int found = 0;
            while (scan) {
                if (scan->addr == want) {
                    found = 1;
                    break;
                }
                scan = scan->next;
            }
            if (!found) {
                ok = 0;
                break;
            }
        }

        if (ok) {
            // unlink every block in that run and return the lowest address
            void *addr = start->addr;
            for (long k = 0; k < need; k++) {
                void *want = (char *)addr + k * MAX_CHUNK_SIZE;
                chunk_t **prev = &free_list->lists[NUM_ORDERS - 1];
                while ((*prev)->addr != want)
                    prev = &(*prev)->next;
                chunk_t *c = *prev;
                *prev = c->next;
                c->addr = NULL;
                c->next = all_chunks;
                all_chunks = c;
            }
            spinlock_unlock(&mm_buddy_mutex);
            return addr;
        }

        start = start->next;
    }

    spinlock_unlock(&mm_buddy_mutex);
    return NULL;
}


void page_free_n(void *ptr, int npages) {
    long max_pages = MAX_CHUNK_SIZE / PAGE_SIZE;

    if (!ptr || npages <= 0) return;  // ignore invalid frees

    // small requests use one buddy block
    if (npages <= max_pages) {
        buddy_obj_free((long)npages * PAGE_SIZE, ptr);
        return;
    }

    long blocks = ((long)npages + max_pages - 1) / max_pages; // large requests use whole 64 KB blocks
    uintptr_t addr = (uintptr_t)ptr;

    if (!dram || !free_list) return;
    if ((uintptr_t)blocks > UINTPTR_MAX / MAX_CHUNK_SIZE) return;
    uintptr_t span = (uintptr_t)blocks * MAX_CHUNK_SIZE;
    if (addr % MAX_CHUNK_SIZE != 0 || !in_pool(addr, span)) return;

    spinlock_lock(&mm_buddy_mutex);

    // if any part of the run is already free, we free none of
    // it.. this is so a bad request won't release part of a live allocation
    if (!overlaps_free(addr, span)) {
        for (long i = 0; i < blocks; ++i)
            release_block(NUM_ORDERS - 1, addr + (uintptr_t)i * MAX_CHUNK_SIZE);
    }

    spinlock_unlock(&mm_buddy_mutex);
}