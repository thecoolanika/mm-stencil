CC = gcc
FLAGS = -Og -g -Wall
LIBFLAGS = -c
INCLUDE = -I. -Iinclude

UTILS = spinlock list string
ALLOCS = slaballocator
ALL = $(UTILS) $(ALLOCS) $(LIBS)

ALLOCOBJS = bin/slab_allocator.o
ALLOBJS = $(UTILOBJS) $(ALLOCOBJS) $(LIBOBJS)

slaballocator:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/slab_allocator.c -o bin/slab_allocator.o

buddyallocator:
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) src/buddy_allocator.c -o bin/buddy_allocator.o

mm: $(UTILS) $(ALLOCS)
	$(CC) $(FLAGS) $(LIBFLAGS) $(INCLUDE) $(ALLOCOBJS) src/mm.c -o bin/mm.o

clean:
	rm -rf bin/*

mm_test:
	$(CC) $(FLAGS) $(INCLUDE) -Dpage_alloc_n=slab_page_alloc_n -Dpage_free_n=slab_page_free_n -c src/slab_allocator.c -o bin/slab_renamed.o
	$(CC) $(FLAGS) $(INCLUDE) test/mm_test.c src/buddy_allocator.c bin/slab_renamed.o sys/mem/mem.c util/spinlock.c -o bin/mm_test
	./bin/mm_test
