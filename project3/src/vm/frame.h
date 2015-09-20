#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "vm/page.h"

struct frame_entry {
	void *frame_ptr;				// Address of frame.
	void *vaddr;					// Virtual Address corresponding to frame enrty.
	tid_t tid;						// Id of the thread to whom page corresponding to the frame entry belongs.
	struct hash_elem frame_elem;	// List elem for adding in frame_table list.
};

// Data structure to store mapping of user virtual address to physical address on memory
struct hash frame_table;

// This is the lock that a thread/process has to acquire
// when perfoming any operation on the frame table. This
// is added for synchronization purposes.
struct lock ft_lock;

unsigned frame_hash (const struct hash_elem *f_elem, void *aux);
bool frame_less (const struct hash_elem *frame_a, const struct hash_elem *frame_b, void *aux);
void* allocate_frame_entry (enum palloc_flags flags, struct page_entry *pte);
void deallocate_frame_entry (void *frame);
void* evict_frame (enum palloc_flags flags);

#endif
