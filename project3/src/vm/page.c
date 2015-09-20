#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"

unsigned page_hash (const struct hash_elem *e, void *aux UNUSED)
{
	struct page_entry *pte = hash_entry(e, struct page_entry, page_elem);
	return hash_bytes (&pte->vaddr, sizeof pte->vaddr);
}

bool page_less (const struct hash_elem *a,
		const struct hash_elem *b,
		void *aux UNUSED)
{
	struct page_entry *page_entry_a = hash_entry(a, struct page_entry, page_elem);
	struct page_entry *page_entry_b = hash_entry(b, struct page_entry, page_elem);
	return page_entry_a->vaddr < page_entry_b->vaddr;
}

void page_destroy_action (struct hash_elem *e, void *aux UNUSED)
{
	struct thread *cur = thread_current ();
	struct page_entry *pte = hash_entry(e, struct page_entry, page_elem);
	if (pte->is_loaded) {
		deallocate_frame_entry (pagedir_get_page (cur->pagedir, pte->vaddr));
		pagedir_clear_page (cur->pagedir, pte->vaddr);
	}
	free(pte);
}

struct page_entry *get_page_entry (void *vaddr)
{
	struct page_entry pte;
	pte.vaddr = pg_round_down(vaddr);

	struct hash_elem *he = hash_find (&thread_current()->page_table, &pte.page_elem);
	if (he) {
		return hash_entry (he, struct page_entry, page_elem);
	}
	return NULL;
}

bool load_swap (struct page_entry *pte)
{
	uint8_t *frame = allocate_frame_entry (PAL_USER, pte);
	if (frame) {
		if (!install_page (pte->vaddr, frame, pte->is_writable)) {
			deallocate_frame_entry (frame);
			return false;
		}

		pte->is_pinned = true;
		swap_frame_in (pte->swap_offset, pte->vaddr);
		pte->is_pinned = false;
		pte->is_loaded = true;
		return pte->is_loaded;
	}
	return false;
}

bool load_mmap (struct page_entry *pte)
{
	return load_file(pte);
}

bool load_file (struct page_entry *pte)
{
	uint8_t *frame = allocate_frame_entry (PAL_USER, pte);
	if (frame) {
		pte->is_pinned = true;
		if ((int) pte->read_bytes != file_read_at(pte->file, frame,
				pte->read_bytes, pte->ofs)) {
			deallocate_frame_entry (frame);
			return false;
		}
		pte->is_pinned = false;
		memset (frame + pte->read_bytes, 0, pte->zero_bytes);
		bool is_page_installed = install_page(pte->vaddr, frame, pte->is_writable);

		if (is_page_installed == false) {
			deallocate_frame_entry (frame);
			return is_page_installed;
		}

		pte->is_loaded = true;
		return pte->is_loaded;
	}
	return false;
}

bool
grow_stack (void *vaddr)
{
	void *rounded_vaddr = pg_round_down (vaddr);
	if (STACK_MAX_SIZE <= (size_t) (PHYS_BASE - pg_round_down(vaddr))) {
		return false;
	}

	struct page_entry *pte = get_page_entry (rounded_vaddr);
	if (pte == NULL) {
		struct page_entry *pte = malloc (sizeof (struct page_entry));
		if (pte == NULL) {
			return false;
		}

		pte->vaddr = rounded_vaddr;
		pte->type = PAGE_SWAP;
		pte->is_loaded = true;
		pte->is_writable = true;
		pte->is_pinned = false;

		uint8_t *frame = allocate_frame_entry (PAL_USER, pte);
		if (frame == NULL) {
			free (pte);
			return false;
		}

		bool page_installed = install_page (pte->vaddr, frame, pte->is_writable);
		if (page_installed == false) {
			free (pte);
			deallocate_frame_entry (frame);
			return false;
		}
		return (hash_insert (&thread_current ()->page_table, &pte->page_elem) == NULL);
	}
	return false;
}
