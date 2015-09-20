#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <list.h>
#include "devices/block.h"
#include "threads/synch.h"

uint32_t buffer_cache_size;
struct list buffer_cache;
struct lock buffer_cache_lock;
static block_sector_t read_ahead_sector = 0;

struct buffer {
	block_sector_t sector; 			  // sector number on disk.
    bool is_dirty; 					  // if the buffer is written or not.
	bool is_accessed; 				  // if the buffer is accessed recently or not.
	int open_count; 				  // number of threads using this buffer entry.
	uint8_t block[BLOCK_SECTOR_SIZE]; // the block of data read from disk.
	struct list_elem buffer_elem;    // list element in buffer_cache
};

void buffer_cache_init ();
struct buffer *get_buffer_from_cache (block_sector_t sector, bool dirty);
struct buffer *put_buffer_in_cache (block_sector_t sector, bool dirty);
struct buffer *buffer_evict_from_cache ();
void write_cache_to_disk (bool sys_halt);
void buffer_cache_write_behind ();
void buffer_cache_read_ahead (void *aux);

#endif
