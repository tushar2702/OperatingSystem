#include "vm/swap.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "devices/block.h"
#include <bitmap.h>

struct lock swap_lock;
struct block *swap_block;
struct bitmap *swap_bitmap;

#define NUM_SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

void initialize_swap_slot (void)
{
	swap_block = block_get_role(BLOCK_SWAP);
	if (swap_block) {
		swap_bitmap = bitmap_create(
				block_size (swap_block) / NUM_SECTORS_PER_PAGE);
		if (swap_bitmap) {
			bitmap_set_all (swap_bitmap, 0); // free swap slot
			lock_init (&swap_lock);
		}
	}
}

void swap_frame_in(size_t used_index, void* frame)
{
	int i;
	if (swap_block && swap_bitmap) {
		lock_acquire (&swap_lock);
		if (bitmap_test (swap_bitmap, used_index) == 0) { // 0 is free swap slot
			lock_release (&swap_lock);
			PANIC ("Cannot swap in a free block.");
		}
		bitmap_flip (swap_bitmap, used_index);
		for (i = 0; i < NUM_SECTORS_PER_PAGE; i++) {
			block_read (swap_block, used_index * NUM_SECTORS_PER_PAGE + i,
					(uint8_t *) frame + i * BLOCK_SECTOR_SIZE);
		}
		lock_release (&swap_lock);
	}
}

size_t swap_frame_out(void *frame)
{
	int i;
	size_t idx;

	if (!swap_block) {
		PANIC ("No swap block available.");
	}
	if (!swap_bitmap) {
		PANIC ("No swap bitmap available.");
	}

	lock_acquire (&swap_lock);
	idx = bitmap_scan_and_flip (swap_bitmap, 0, 1, 0); // 0 is free swap slot
	if (idx == BITMAP_ERROR) {
		lock_release(&swap_lock);
		PANIC ("Swap slot is full.");
	}

	for (i = 0; i < NUM_SECTORS_PER_PAGE; i++) {
		block_write (swap_block, idx * NUM_SECTORS_PER_PAGE + i,
				(uint8_t *) frame + i * BLOCK_SECTOR_SIZE);
	}

	lock_release (&swap_lock);
	return idx;
}
