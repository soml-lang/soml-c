/*
An allocator for the fuzz targets. It checks every old size, always moves reallocated buffers, fails the chosen allocation, and counts live blocks.

Include it after the SOML implementation, which provides the ASan poisoning hooks.
*/
#ifndef FUZZ_ALLOCATOR_H
#define FUZZ_ALLOCATOR_H

#include <stddef.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct fuzz_allocator {
	size_t allocations;
	size_t fail_at;
	size_t live_blocks;
} fuzz_allocator;

typedef struct fuzz_allocation {
	size_t size;
	alignas(8) unsigned char redzone[8];
	max_align_t data[];
} fuzz_allocation;

/*
Checks the allocator contract before changing memory. A failed reallocation leaves the original block intact.
*/
static void *fuzz_reallocate(void *context, void *pointer, size_t old_size, size_t new_size) {
	fuzz_allocator *state = context;
	fuzz_allocation *allocation = pointer == nullptr ? nullptr : (fuzz_allocation *)((unsigned char *)pointer - offsetof(fuzz_allocation, data));

	if (old_size != (allocation == nullptr ? 0 : allocation->size)) {
		abort();
	}

	if (new_size == 0) {
		state->live_blocks -= allocation != nullptr;
		free(allocation);
		return nullptr;
	}

	size_t size;

	if (state->allocations++ == state->fail_at || ckd_add(&size, offsetof(fuzz_allocation, data), new_size)) {
		return nullptr;
	}

	fuzz_allocation *result = malloc(size);

	if (result == nullptr) {
		return nullptr;
	}

	result->size = new_size;
	// Poison the guard and any alignment padding before the payload, leaving the size readable by this allocator.
	SOML__POISON(result->redzone, offsetof(fuzz_allocation, data) - offsetof(fuzz_allocation, redzone));

	if (allocation == nullptr) {
		state->live_blocks++;
	} else {
		memcpy(result->data, pointer, old_size < new_size ? old_size : new_size);
		free(allocation);
	}

	return result->data;
}

#endif
