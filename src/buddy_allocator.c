#include "buddy_allocator.h"

#include "sys/mem/mem.h"

#include "util/ktypes.h"
#include "util/string.h"

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
    chunk_t *all_chunks;  // pointer to first chunk? or 
    free_list_t *free_list; // list of chunks? -- pointer to pointer of first chunk -- to allocate memory we need to know how much is available
    // 1 pre-compute the sizes of your structures in advance or 2 use a heuristic ^^ 

    slab_allocator_t buddy_slab_allocator; // allocate all memory for buddy 
    slab_allocator_t chunk_slab_allocator; // allocate 1 chunk (?) or 1 list

    spinlock_t mm_buddy_mutex; // for free list?
}

/*
 * TODO: implement me!
 *
 * Hints:
 *   - given a size, how do we know where to allocate from?
 *   - if we don't have any available chunks, how do we find another one?
 */
void *buddy_obj_alloc(long size) {
    // look through chunk list and see if there are any chunks that are size or bigger?
    // allocate space using chunks -- return pointer to chunk that is free and remove it from free list
    // if we have no available chunks.....lowkey idk look back to beginning of chunk list before the current free list pointer? 
    return NULL;
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
}

/*
 * TODO: implement me!
 *
 * Hint: this function behaves similar to "obj_alloc" but what happens if
 *       npages is larger than MAX_CHUNK_SIZE?
 */
void *page_alloc_n(int npages) {
    return NULL;
    // if npages larger than max_chunk_size break up into bits that are chunk size and split them?
    // do buddy obj alloc per chunk
}

/*
 * TODO: implement me!
 *
 * Hint: this function should handle the inverse case of the "alloc" function!
 */
void page_free_n(void *ptr, int npages) {
    // if npages larger than max_chunk_size idk maybe free more thank one chunk? orsmth
    // do buddy obj free per chunk
}
