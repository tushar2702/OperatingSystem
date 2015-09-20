#include "filesys/cache.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "threads/thread.h"
#include "devices/timer.h"

#define MAX_CACHE_SIZE 64
#define WRITE_BEHIND_INTERVAL 5 * TIMER_FREQ

void
buffer_cache_init ()
{
	buffer_cache_size = 0;
	list_init (&buffer_cache);
	lock_init (&buffer_cache_lock);
	thread_create ("cache_write_behind", 0, buffer_cache_write_behind, NULL);
	thread_create ("cache_read_ahead", 0, buffer_cache_read_ahead, NULL);
}

struct buffer
*get_buffer_from_cache (block_sector_t sector, bool dirty)
{
	struct buffer *buf;
	struct list_elem *e;
	lock_acquire (&buffer_cache_lock);
	for (e = list_begin (&buffer_cache);
			e != list_end (&buffer_cache);
			e = list_next (e)) {
		buf = list_entry (e, struct buffer, buffer_elem);
		if (buf->sector == sector) {
			buf->open_count++;
			buf->is_accessed = true;
			if (!buf->is_dirty) {
				buf->is_dirty = dirty;
			}
			lock_release (&buffer_cache_lock);
			return buf;
		}
	}

	buf = put_buffer_in_cache (sector, dirty);

	if (buf == NULL) {
		lock_release (&buffer_cache_lock);
		PANIC ("No more space for buffer cache");
	}

	lock_release (&buffer_cache_lock);
	return buf;
}

struct buffer
*put_buffer_in_cache (block_sector_t sector, bool dirty)
{
	struct buffer *buf;
	if (buffer_cache_size < MAX_CACHE_SIZE) {
		buf = malloc (sizeof (struct buffer));
		if (buf == NULL) {
			return NULL;
		}
		buffer_cache_size++;
		buf->open_count = 0;
		list_push_back (&buffer_cache, &buf->buffer_elem);
	} else {
		buf = buffer_evict_from_cache ();
	}

	buf->open_count++;
	buf->is_accessed = true;
	buf->is_dirty = dirty;
	buf->sector = sector;
	block_read (fs_device, buf->sector, &buf->block);
	return buf;
}

struct buffer
*buffer_evict_from_cache ()
{
	struct buffer *buf;
	for (;;) {
		struct list_elem *e;
		for (e = list_begin(&buffer_cache);
				e != list_end(&buffer_cache);
				e = list_next(e)) {
			buf = list_entry (e, struct buffer, buffer_elem);
			if (buf->open_count > 0) {
				continue;
			}
			if (buf->is_accessed) {
				buf->is_accessed = false;
			} else {
				if (buf->is_dirty) {
					block_write(fs_device, buf->sector, &buf->block);
				}
				return buf;
			}
		}
	}
	return NULL;
}

void
write_cache_to_disk (bool sys_halt)
{
	struct list_elem *next, *e;
	lock_acquire (&buffer_cache_lock);
	for (e = list_begin (&buffer_cache);
			e != list_end (&buffer_cache);) {
		next = list_next (e);
		struct buffer *buf = list_entry (e, struct buffer, buffer_elem);
		if (buf->is_dirty) {
			block_write (fs_device, buf->sector, &buf->block);
			buf->is_dirty = false;
		}
		if (sys_halt) {
			list_remove (&buf->buffer_elem);
			free (buf);
		}
		e = next;
	}
	lock_release (&buffer_cache_lock);
}

void
buffer_cache_write_behind ()
{
	for (;;) {
		timer_sleep (WRITE_BEHIND_INTERVAL);
		write_cache_to_disk (false);
	}
}

void
buffer_cache_read_ahead (void *aux)
{
	lock_acquire (&buffer_cache_lock);
	block_sector_t sector = read_ahead_sector;
	if (sector != 0) {
		struct buffer *buf;
		struct list_elem *e;
		for (e = list_begin (&buffer_cache);
				e != list_end (&buffer_cache);
				e = list_next(e)) {
			buf = list_entry (e, struct buffer, buffer_elem);
			if (buf->sector == sector) {
				break;
			}
		}
		if (!buf) {
			get_buffer_from_cache (sector, false);
		}
	}
	lock_release (&buffer_cache_lock);
}
