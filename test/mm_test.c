#include "buddy_allocator.h"
#include "sys/mem/mem.h"

#include <stdio.h>
#include <stdlib.h>

#define MEMORY_BYTES (2L * 1024 * 1024)
#define MAX_TEST_BLOCKS 128

static int failures;

#define CHECK(condition, message) do {                                      \
    if (!(condition)) {                                                     \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);        \
        ++failures;                                                         \
    }                                                                       \
} while (0)

// testing process memory lifecycle
static void test1(void) {
    void *text = buddy_obj_alloc(7000);
    void *stack = buddy_obj_alloc(3 * PAGE_SIZE);
    void *pages = page_alloc_n(5);

    CHECK(text != NULL && stack != NULL && pages != NULL,
          "process address-space allocations succeed");
    if (text && stack && pages) {
        CHECK(text != stack && text != pages && stack != pages,
              "process allocations have distinct addresses");
        buddy_obj_free(7000, text);
        buddy_obj_free(3 * PAGE_SIZE, stack);
        page_free_n(pages, 5);
    } else {
        if (text) buddy_obj_free(7000, text);
        if (stack) buddy_obj_free(3 * PAGE_SIZE, stack);
        if (pages) page_free_n(pages, 5);
    }
}

// testing memory exhaustion and recovery
static void test2(void) {
    void *blocks[MAX_TEST_BLOCKS];
    int count = 0;

    while (count < MAX_TEST_BLOCKS &&
           (blocks[count] = buddy_obj_alloc(MAX_CHUNK_SIZE)) != NULL)
        ++count;

    CHECK(count > 0, "whole-memory allocation obtains at least one block");
    CHECK(count < MAX_TEST_BLOCKS,
          "allocation eventually fails after consuming available memory");
    CHECK(buddy_obj_alloc(MAX_CHUNK_SIZE) == NULL,
          "allocation fails while the allocator is exhausted");

    if (count > 0) {
        buddy_obj_free(MAX_CHUNK_SIZE, blocks[0]);
        blocks[0] = buddy_obj_alloc(MAX_CHUNK_SIZE);
        CHECK(blocks[0] != NULL,
              "freeing memory allows a subsequent allocation to succeed");
    }

    for (int i = 0; i < count; ++i)
        if (blocks[i]) buddy_obj_free(MAX_CHUNK_SIZE, blocks[i]);
}

// testing oversized requests
static void test3(void) {
    void *too_large = buddy_obj_alloc(MAX_CHUNK_SIZE + 1L);
    CHECK(too_large == NULL,
          "object allocation larger than the largest free-list chunk fails");

    // page allocations can span adjacent maximum-size chunks
    int pages = MAX_CHUNK_SIZE / PAGE_SIZE + 1;
    void *large_run = page_alloc_n(pages);
    CHECK(large_run != NULL,
          "page allocation larger than one chunk succeeds when space permits");
    if (large_run) page_free_n(large_run, pages);
}

// testing that freeing buddy halves coalesces into a maximum-size block
static void test4(void) {
    void *blocks[MAX_TEST_BLOCKS];
    int count = 0;

    while (count < MAX_TEST_BLOCKS &&
           (blocks[count] = buddy_obj_alloc(MAX_CHUNK_SIZE)) != NULL)
        ++count;

    CHECK(count > 0 && count < MAX_TEST_BLOCKS,
          "coalescing setup exhausts the available maximum-size blocks");
    if (count == 0 || count == MAX_TEST_BLOCKS) {
        for (int i = 0; i < count; ++i)
            buddy_obj_free(MAX_CHUNK_SIZE, blocks[i]);
        return;
    }

    void *original = blocks[0];
    buddy_obj_free(MAX_CHUNK_SIZE, original);
    blocks[0] = NULL;

    void *left = buddy_obj_alloc(MAX_CHUNK_SIZE / 2);
    void *right = buddy_obj_alloc(MAX_CHUNK_SIZE / 2);
    CHECK(left != NULL && right != NULL,
          "released maximum-size block can be split into two halves");
    if (left && right) {
        CHECK((char *)left + MAX_CHUNK_SIZE / 2 == (char *)right ||
              (char *)right + MAX_CHUNK_SIZE / 2 == (char *)left,
              "allocated halves are adjacent buddies");
        buddy_obj_free(MAX_CHUNK_SIZE / 2, left);
        buddy_obj_free(MAX_CHUNK_SIZE / 2, right);

        void *merged = buddy_obj_alloc(MAX_CHUNK_SIZE);
        CHECK(merged == original,
              "freeing both halves coalesces them back to the original block");
        if (merged) buddy_obj_free(MAX_CHUNK_SIZE, merged);
    } else {
        if (left) buddy_obj_free(MAX_CHUNK_SIZE / 2, left);
        if (right) buddy_obj_free(MAX_CHUNK_SIZE / 2, right);
    }

    for (int i = 0; i < count; ++i)
        if (blocks[i]) buddy_obj_free(MAX_CHUNK_SIZE, blocks[i]);
}

int main(void) {
    mem_size = MEMORY_BYTES;
    dram = malloc((size_t)mem_size);
    if (!dram) {
        perror("malloc");
        return 2;
    }

    buddy_init();
    test1();
    test2();
    test3();
    test4();

    free(dram);
    if (failures) {
        fprintf(stderr, "%d assignment test(s) failed\n", failures);
        return 1;
    }
    puts("All buddy allocator scenarios passed");
    return 0;
}
