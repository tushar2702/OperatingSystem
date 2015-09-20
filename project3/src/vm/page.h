#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/file.h"
#include "vm/frame.h"

#define STACK_MAX_SIZE 0x800000		// 8MB

// Different types of pages.
enum page_type
{
	PAGE_FILE,
	PAGE_SWAP,
	PAGE_MMAP,
	PAGE_HASH_ERROR
};

struct page_entry {
	void *vaddr;                  // user virtual address
	struct file *file;            // file corresponding to page
	enum page_type type;          // page type
	off_t ofs;                     // page offset
	size_t read_bytes;             // number of bytes read
	size_t zero_bytes;             // number of zero bytes
	bool is_loaded;               // whether page is loaded or not
	bool is_writable;             // whether page is writable or not
	bool is_pinned;               // used for synchronization during eviction
	struct hash_elem page_elem;  // hash elem for page table entry
	off_t swap_offset;            // swap offset for page entry
};

unsigned page_hash (const struct hash_elem *pg_elem, void *aux);
bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux);
void page_destroy_action (struct hash_elem *he, void *aux);

bool load_file (struct page_entry *pte);
bool load_swap (struct page_entry *pte);
bool load_mmap (struct page_entry *pte);
bool grow_stack (void *vaddr);
struct page_entry *get_page_entry (void *vaddr);

#endif
