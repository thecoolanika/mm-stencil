#include "buddy_allocator.h"

#include "sys/mem/mem.h"

#include "util/ktypes.h"
#include "util/string.h"

#include <stddef.h>

// mem.h leaves this commented out
long mem_size;
static unsigned char *base;
static volatile size_t *allocation_map;
static size_t usable_pages;
static int num_free_lists;
static int initialized;
#define ALLOCATED_CONTINUATION ((size_t)-1)

// put a free-list node at the start of its own free memory block 
static void push(int index, unsigned char *addr) {
    chunk_t *node = (chunk_t *)addr;
    node->addr = addr;
    node->next = free_list->lists[index];
    free_list->lists[index] = node;
}

// remove and return the address of the first free block at this list index
static unsigned char *pop(int index) {
    chunk_t *node = free_list->lists[index];

    if (!node) return NULL;
    free_list->lists[index] = node->next;
    return node->addr;
}

// unlink a buddy block from its list before we merge it into its parent
static int remove_free_block(int index, unsigned char *addr) {
    chunk_t **link = &free_list->lists[index];

    while (*link && (*link)->addr != addr) link = &(*link)->next;
    if (!*link) return 0;

    *link = (*link)->next;
    return 1;
}
// 4k, 8k, 16k, 32k, 64k
#define NUM_ORDERS 5

/*
 * TODO: implement me!
 *
 * Hints:
 *   - we need to construct the free list, how many lists should reside in
 *     the structure?
 *   - to allocate the "chunks" and the free list, we need to reserve space in
 *     memory as a slab... how big will these components be?
 *   - once we have allocated space for the "mm" subsystem components, the
 *     amount of free space in memory has changed... bare this in mind when
 *     constructing your metadata!
 */
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

/*
 * TODO: implement me!
 *
 * Hints:
 *   - given a size, how do we know where to allocate from?
 *   - if we don't have any available chunks, how do we find another one?
 */
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

/*
 * TODO: implement me!
 *
 * Hint: this function should handle the inverse cases of the "alloc" function!
 *       if there are cases that need to be handled in that function, what
 *       would the inverse cases look like?
 */
void buddy_obj_free(long size, void *obj) {
    // put chunks back into free list ? 
    // move current free list pointer back to front of all chunk list if these chunks are at the front and the pointer is past it?
    int index;
    size_t first_page;
    unsigned char *block = obj;

    // ignore uninitialized, null, or out-of-bounds frees
    if (!initialized || !obj || size <= 0 || block < base || block >= base + usable_pages * PAGE_SIZE) return;

    // buddy allocator only supports blocks up to MAX_CHUNK_SIZE, and
    // block must begin on a usable page boundary
    if ((size_t)size > MAX_CHUNK_SIZE || (size_t)(block - base) % PAGE_SIZE != 0) return;

    // find the free-list index
    size_t bytes = PAGE_SIZE;
    index = 0;
    while (bytes < (size_t)size) {
        bytes <<= 1;
        ++index;
    }

    // convert address into a page index
    // the whole block needs to fit inside allocator region
    first_page = (size_t)(block - base) / PAGE_SIZE;
    if (first_page + ((size_t)1 << index) > usable_pages) return;

    spinlock_lock(&mm_buddy_mutex);

    // saved index rejects duplicate frees
    // also reject sizes that do not match the original allocation from this address
    if (allocation_map[first_page] != (unsigned char)(index + 1)) {
        spinlock_unlock(&mm_buddy_mutex);
        return;
    }

    // block is being returned, so clear allocation record
    allocation_map[first_page] = 0;
    for (size_t page = 0; page < ((size_t)1 << index); ++page) {
        if (page != 0) allocation_map[first_page + page] = 0;
    }

    // find page index of this block's buddy (of the same size)
    while (index < num_free_lists - 1) {
        size_t buddy_page = first_page ^ ((size_t)1 << index);
        unsigned char *buddy = base + buddy_page * PAGE_SIZE;

        // we merge if buddy is currently on same free-list index
        // this will make room for bigger combined block
        if (!remove_free_block(index, buddy)) break;

        // both halves now free
        // parent starts at the lower address
        if (buddy_page < first_page) first_page = buddy_page;

        // now the combined block is twice as large, so we check the
        // next free-list index on the next iteration
        ++index;
    }

    // put our final merged block back on its free list
    push(index, base + first_page * PAGE_SIZE);

    spinlock_unlock(&mm_buddy_mutex);
}

/*
 * TODO: implement me!
 *
 * Hint: this function behaves similar to "obj_alloc" but what happens if
 *       npages is larger than MAX_CHUNK_SIZE?
 */
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

/*
 * TODO: implement me!
 *
 * Hint: this function should handle the inverse case of the "alloc" function!
 */
 void page_free_n(void *ptr, int npages) {
    size_t requested, reserved, max_chunk_pages, tail, first;

    if (!ptr || npages <= 0) return;  // ignore invalid frees
    requested = (size_t)npages;  // keep the requested page count

    max_chunk_pages = MAX_CHUNK_SIZE / PAGE_SIZE;

    if (requested <= max_chunk_pages) {
        buddy_obj_free((long)requested * PAGE_SIZE, ptr);
        return;
    }

    // round up the final partial chunk
    reserved = (requested / max_chunk_pages) * max_chunk_pages;
    tail = requested % max_chunk_pages;
    if (tail) {
        size_t tail_pages = 1;
        while (tail_pages < tail) tail_pages <<= 1;
        reserved += tail_pages;
    }

    if (!initialized || (unsigned char *)ptr < base ||
        (unsigned char *)ptr >= base + usable_pages * PAGE_SIZE ||
        ((unsigned char *)ptr - base) % PAGE_SIZE != 0) return;

    first = (size_t)((unsigned char *)ptr - base) / PAGE_SIZE;  // turn the pointer into a page number
    if (first + reserved > usable_pages) return;

    spinlock_lock(&mm_buddy_mutex);

    // store the original large request in the first page
    if (allocation_map[first] != (size_t)num_free_lists + requested) {
        spinlock_unlock(&mm_buddy_mutex);
        return;
    }

    // later pages in the rounded span must still carry the continuation
    // marker written when the large range was allocated
    for (size_t page = 0; page < reserved; ++page) {
        if (page != 0 && allocation_map[first + page] != ALLOCATED_CONTINUATION) {
            spinlock_unlock(&mm_buddy_mutex);
            return;
        }
    }

    // pass each page through the normal buddy free path so all buddies merge
    for (size_t page = 0; page < reserved; ++page) {
        allocation_map[first + page] = 1;
    }

    spinlock_unlock(&mm_buddy_mutex);

    // free pages in order
    // each call can merge with pages freed earlier
    for (size_t page = 0; page < reserved; ++page) {
        buddy_obj_free(PAGE_SIZE, base + (first + page) * PAGE_SIZE);
    }
}
