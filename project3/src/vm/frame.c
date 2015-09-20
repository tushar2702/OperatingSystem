#include "vm/frame.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "filesys/file.h"

unsigned
frame_hash(const struct hash_elem *f_elem, void *aux UNUSED)
{
	const struct frame_entry *fe = hash_entry(f_elem, struct frame_entry, frame_elem);
	return hash_bytes (&fe->vaddr, sizeof fe->vaddr);
}

bool
frame_less(const struct hash_elem *frame_a, const struct hash_elem *frame_b, void *aux UNUSED)
{
	const struct frame_entry *fa = hash_entry(frame_a, struct frame_entry, frame_elem);
	const struct frame_entry *fb = hash_entry(frame_a, struct frame_entry, frame_elem);
	return fa->vaddr < fb->vaddr;
}

void *evict_frame (enum palloc_flags flags)
{
	struct hash_iterator frame_table_itr;
	hash_first (&frame_table_itr, &frame_table);
	struct page_entry *pe;
	size_t size = 0;

	while (true) {
		struct frame_entry *fte = hash_entry (hash_cur (&frame_table_itr), struct frame_entry, frame_elem);

		struct thread *fte_thread = retrieve_thread(fte->tid);
		struct page_entry *pte = get_page_entry(fte->vaddr);

		if (pte->is_pinned == false) {
			if (pagedir_is_accessed(fte_thread->pagedir, fte->vaddr)) {
				pagedir_set_accessed(fte_thread->pagedir, fte->vaddr, false);
			} else {
				if (pagedir_is_dirty(fte_thread->pagedir, fte->vaddr)) {

					if (pte->type == PAGE_MMAP) {
						file_write_at(pte->file, fte->frame_ptr,
								pte->read_bytes,
								pte->zero_bytes);
					} else {
						pte->type = PAGE_SWAP;
						pte->swap_offset = swap_frame_out (fte->frame_ptr);
					}
				}
				pte->is_loaded = false;
				pagedir_clear_page(fte_thread->pagedir, fte->vaddr);
				deallocate_frame_entry (fte->frame_ptr);
				return allocate_frame_entry (flags, pte);
			}
		}
		size++;
		fte = hash_next(&frame_table_itr);
		if(size == hash_size(&frame_table)) {
			hash_first(&frame_table_itr, &frame_table);
			size = 0;
		}
	}
}

void
*allocate_frame_entry (enum palloc_flags flags, struct page_entry *pte)
{
	void *frame = palloc_get_page (flags);
	if (!frame) {
		lock_acquire (&ft_lock);
		frame = evict_frame (flags);
		lock_release(&ft_lock);
		if (frame == NULL) {
			PANIC ("Failed to evict a frame. Swap partiition is full too!");
		}
	}

	lock_acquire (&ft_lock);
	struct frame_entry *fte = malloc (sizeof (struct frame_entry));
	if (fte == NULL) {
		return NULL;
	}
	fte->frame_ptr = frame;
	fte->vaddr = pte->vaddr;
	fte->tid = thread_current ()->tid;
	hash_insert(&frame_table, &fte->frame_elem);
	lock_release (&ft_lock);
	return frame;
}

void
deallocate_frame_entry (void *frame)
{
	struct hash_elem *frame_elem;
	struct frame_entry fe;
	lock_acquire (&ft_lock);
	fe.frame_ptr = frame;
	frame_elem = hash_find(&frame_table, &fe.frame_elem);
	if(frame_elem) {
		struct frame_entry *fte = hash_entry(frame_elem, struct frame_entry, frame_elem);
		hash_delete(&frame_table, &fte->frame_elem);
		free(fte);
	}
	lock_release (&ft_lock);
}

struct frame_entry *get_frame_entry (void *vaddr)
{
	struct hash_elem *frame_elem;
	struct frame_entry fe;
	fe.vaddr = vaddr;
	frame_elem = hash_find(&frame_table, &fe.frame_elem);
	if(frame_elem) {
		return hash_entry(frame_elem, struct frame_entry, frame_elem);
	}
	return NULL;
}
