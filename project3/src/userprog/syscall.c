#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/frame.h"
#include "vm/page.h"

// Lock required for file related syscalls
struct lock file_lock;

static void syscall_handler (struct intr_frame *);
static struct page_entry *check_ptr_validity (const void *vaddr, void *esp);
static struct page_entry *check_valid_pte (const void *vaddr, void *esp);
static void check_buffer_validity (void* buffer, unsigned size, void *esp, bool to_write);
static void extract_args (struct intr_frame *f, int numargs, int *args);
static bool insert_mmap_in_page_table(struct file *file, int32_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable);
static bool add_process_mmap (struct page_entry *pte);

void
syscall_init (void)
{
	lock_init(&file_lock);
	intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/*
 * Checks whether vaddr has a corresponding
 * valid pte and is loaded.
 */
static struct page_entry
*check_valid_pte (const void *vaddr, void *esp)
{
	bool load = false;
	struct page_entry *pte = get_page_entry (vaddr);
	if (pte) {
		if (!pte->is_loaded) {
			if (pte->type == PAGE_FILE) {
				load_file(pte);
			} else if (pte->type == PAGE_SWAP) {
				load_swap(pte);
			} else if (pte->type == PAGE_MMAP) {
				load_mmap(pte);
			}
		}

		load = pte->is_loaded;
	} else if (esp - 32 < vaddr) {
		load = grow_stack (vaddr);
	}
	if (!load)
		exit (-1);
	return pte;
}

/*
 * Used for read/write syscalls to check memory
 * pointer validity of the buffer to read/write.
 */
static void
check_buffer_validity (void* buffer, unsigned size, void *esp, bool to_write)
{
	unsigned i;
	char *temp_buffer = (char *) buffer;
	for (i=0; i<size; i++) {
		struct page_entry *pte =
				check_ptr_validity ((const void*) temp_buffer, esp);
		if (to_write && pte) {
			if (pte->is_writable == false) {
				exit (-1);
			}
		}
		temp_buffer++;
	}
}

/*
 * Checks validity of address of each character
 * position in the string str.
 */
void check_str_validity (const void* str, void *esp)
{
	check_ptr_validity(str, esp);
	//TODO: refactor if poss
	while (*(char *) str != 0)
	{
		str = (char *) str + 1;
		check_ptr_validity(str, esp);
	}
}

/*
 * Checks whether a pointer is within the user
 * address space and checks validity of the page
 * table entry. Exits with -1 status if the
 * pointer is not valid.
 */
static struct page_entry
*check_ptr_validity (const void *vaddr, void *esp)
{
	if (vaddr != NULL
			&& (is_user_vaddr (vaddr))
			&& vaddr >= USER_VADDR_BOTTOM) {
		return check_valid_pte (vaddr, esp);
	} else {
		exit(-1);
	}
}

/*
 * Extract numargs args from stack and save
 * them in *args.
 */
static void
extract_args (struct intr_frame *f, int numargs, int *args)
{
	int i;
	int *ptr;
	for (i=1; i<=numargs; i++)
	{
		ptr = ((int *) f->esp) + i;
		check_ptr_validity(ptr, f->esp);
		args[i-1] = *ptr;
	}
}

static void
syscall_handler (struct intr_frame *f UNUSED)
{
	int args[3];
	check_ptr_validity ((const void*) f->esp, f->esp);
	int *esp_pointer = (int *) f->esp;

	switch (*esp_pointer) {
	case SYS_HALT:
		halt ();
		break;

	case SYS_EXIT:
		extract_args (f, 1, &args[0]);
		exit (args[0]);
		break;

	case SYS_EXEC:
		extract_args (f, 1, &args[0]);
		check_str_validity ((const void*) args[0], f->esp);
		f->eax = exec ((const char*) args[0]);
		break;

	case SYS_WAIT:
		extract_args (f, 1, &args[0]);
		f->eax = wait ((pid_t) args[0]);
		break;

	case SYS_CREATE:
		extract_args (f, 2, &args[0]);
		check_str_validity ((const void*) args[0], f->esp);
		f->eax = create ((const char*) args[0], (unsigned) args[1]);
		break;

	case SYS_REMOVE:
		extract_args (f, 1, &args[0]);
		check_str_validity ((const void*) args[0], f->esp);
		f->eax = remove ((const char*) args[0]);
		break;

	case SYS_OPEN:
		extract_args (f, 1, &args[0]);
		check_str_validity ((const void*) args[0], f->esp);
		f->eax = open ((const char*) args[0]);
		break;

	case SYS_FILESIZE:
		extract_args (f, 1, &args[0]);
		f->eax = filesize (args[0]);
		break;

	case SYS_READ:
		extract_args (f, 3, &args[0]);
		check_buffer_validity ((void *) args[1], (unsigned) args[2], f->esp, true);
		f->eax = read ((int) args[0], (void *) args[1], (unsigned) args[2]);
		break;

	case SYS_WRITE:
		extract_args (f, 3, &args[0]);
		check_buffer_validity ((void *) args[1], (unsigned) args[2], f->esp, false);
		f->eax = write ((int) args[0], (void *) args[1], (unsigned) args[2]);
		break;

	case SYS_SEEK:
		extract_args (f, 2, &args[0]);
		seek ((int) args[0], (unsigned) args[1]);
		break;

	case SYS_TELL:
		extract_args (f, 1, &args[0]);
		f->eax = tell ((int) args[0]);
		break;

	case SYS_CLOSE:
		extract_args (f, 1, &args[0]);
		close ((int) args[0]);
		break;

	case SYS_MMAP:
		extract_args (f, 2, &args[0]);
		f->eax = mmap (args[0], (void *) args[1]);
		break;

	case SYS_MUNMAP:
		extract_args (f, 1, &args[0]);
		munmap (args[0]);
		break;

	default:
		exit (-1);
		break;
	}
}

void
halt (void)
{
	shutdown_power_off ();
}

void
exit (int status)
{
	struct thread *cur = thread_current ();
	tid_t parent_id = cur->parent;
	struct thread *parent_thread = retrieve_thread (parent_id);
	if (parent_thread != NULL) {
		struct child_process *cur_cp =
				retrieve_cur_cp_from_parent (parent_thread);
		if (cur_cp != NULL) {
			cur_cp->exit_status = status;
		}
	}

	printf ("%s: exit(%d)\n", thread_current ()->name, status);
	thread_exit ();
}

pid_t
exec (const char *cmd_line)
{
	pid_t pid = process_execute (cmd_line);
	struct thread *cur = thread_current ();
	struct list_elem *e;
	for (e = list_begin (&cur->child_list);
			e != list_end (&cur->child_list);
			e = list_next(e)) {
		struct child_process *child =
				list_entry (e, struct child_process, child_elem);
		if (child == NULL) {
			break;
		}
		if (child->tid == pid) {
			if (child->load == UNLOADED) {
				lock_acquire (&cur->load_lock);
				cond_wait (&child->load_cond, &cur->load_lock);
				lock_release (&cur->load_lock);
			}
			if (child->load == LOAD_SUCCESS) {
				return pid;
			} else {
				list_remove (&child->child_elem);
				free (child);
				break;
			}
		}
	}
	return -1;
}

int wait (pid_t pid)
{
	return process_wait(pid);
}

bool
create (const char *file, unsigned initial_size)
{
	ASSERT (file != NULL);

	lock_acquire (&file_lock);
	off_t size = (off_t) initial_size;
	bool is_file_created = filesys_create (file, size);
	lock_release (&file_lock);
	return is_file_created;
}

bool
remove (const char *file)
{
	ASSERT (file != NULL);

	lock_acquire (&file_lock);
	bool is_file_removed = filesys_remove(file);
	lock_release (&file_lock);
	return is_file_removed;
}

int
open (const char *file)
{
	ASSERT (file != NULL);

	lock_acquire (&file_lock);
	int is_open;
	struct file *file_to_open = filesys_open(file);
	if (file_to_open == NULL) {
		is_open = -1;
	} else {
		struct thread *cur = thread_current ();
		struct file_for_process *process_file =
				malloc (sizeof *process_file);
		if (process_file == NULL) {
			is_open = -1;
		} else {
			process_file->fd = cur->fd;
			process_file->file = file_to_open;
			is_open = cur->fd;
			cur->fd++;
			list_push_back (&cur->opened_file_list, &process_file->file_elem);
		}
	}

	lock_release (&file_lock);
	return is_open;
}

int
filesize (int fd)
{
	lock_acquire (&file_lock);
	int filesize = -1;

	struct thread *cur = thread_current ();
	struct list_elem *e;
	for (e = list_begin (&cur->opened_file_list);
			e != list_end (&cur->opened_file_list);
			e = list_next(e)) {
		struct file_for_process *process_file =
				list_entry (e, struct file_for_process, file_elem);
		if (process_file == NULL) {
			filesize = -1;
			break;
		}
		if (fd == process_file->fd) {
			filesize = file_length (process_file->file);
			break;
		}
	}

	lock_release (&file_lock);
	return filesize;
}

int
read (int fd, void *buffer, unsigned size)
{
	int num_bytes_read = 0;
	if (fd == 0) {
		unsigned i;
		for (i=0; i<size; i++) {
			((uint8_t*) buffer)[i] = input_getc ();
			num_bytes_read++;
		}
		return num_bytes_read;
	}
	lock_acquire (&file_lock);

	struct thread *cur = thread_current ();
	struct list_elem *e;
	for (e = list_begin (&cur->opened_file_list);
			e != list_end (&cur->opened_file_list);
			e = list_next (e)) {
		struct file_for_process *process_file =
				list_entry (e, struct file_for_process, file_elem);
		if (process_file == NULL) {
			num_bytes_read = -1;
			break;
		}

		if (fd == process_file->fd) {
			num_bytes_read = file_read (process_file->file, buffer, size);
			break;
		}
	}
	lock_release (&file_lock);
	return num_bytes_read;
}

int
write (int fd, const void *buffer, unsigned size)
{
	if (fd == 1) {
		putbuf ((const char *) buffer, (size_t) size);
		return size;
	}
	lock_acquire (&file_lock);
	int num_bytes_written = 0;
	struct thread *cur = thread_current ();
	struct list_elem *e;
	for (e = list_begin (&cur->opened_file_list);
			e != list_end (&cur->opened_file_list);
			e = list_next(e)) {
		struct file_for_process *process_file =
				list_entry (e, struct file_for_process, file_elem);
		if (process_file == NULL || process_file->file == NULL) {
			num_bytes_written = 0;
			break;
		}

		if (fd == process_file->fd) {
			num_bytes_written =
					file_write (process_file->file, buffer, size);
			break;
		}
	}

	lock_release (&file_lock);
	return num_bytes_written;
}

void
seek (int fd, unsigned position)
{
	lock_acquire (&file_lock);
	struct thread *cur = thread_current ();
	struct list_elem *e;
	for (e = list_begin (&cur->opened_file_list);
			e != list_end (&cur->opened_file_list);
			e = list_next(e)) {
		struct file_for_process *process_file =
				list_entry (e, struct file_for_process, file_elem);
		if (process_file == NULL) {
			break;
		}
		if (fd == process_file->fd) {
			if (process_file->file != NULL) {
				file_seek (process_file->file, position);
			}
			break;
		}
	}
	lock_release (&file_lock);
}

unsigned
tell (int fd)
{
	lock_acquire (&file_lock);
	unsigned nxt_byte_pos = -1;
	struct thread *cur = thread_current ();
	struct list_elem *e;
	for (e = list_begin (&cur->opened_file_list);
			e != list_end (&cur->opened_file_list);
			e = list_next(e)) {
		struct file_for_process *process_file =
				list_entry (e, struct file_for_process, file_elem);
		if (process_file == NULL) {
			nxt_byte_pos = -1;
			break;
		}
		if (fd == process_file->fd) {
			if (process_file->file == NULL) {
				nxt_byte_pos = -1;
			} else {
				nxt_byte_pos = (file_tell (process_file->file));
			}
			break;
		}
	}
	lock_release (&file_lock);
	return nxt_byte_pos;
}

void
close (int fd)
{
	close_file (fd);
}

void
close_file (int fd)
{
	lock_acquire (&file_lock);
	struct thread *cur = thread_current ();
	struct list_elem *e, *next;
	for (e = list_begin (&cur->opened_file_list);
			e != list_end (&cur->opened_file_list);) {
		next = list_next (e);
		struct file_for_process *process_file =
				list_entry (e, struct file_for_process, file_elem);
		if (process_file == NULL) {
			break;
		}

		// -1 stands for closing all file descriptors.
		if (fd == -1) {
			file_close (process_file->file);
			list_remove (e);
			free (process_file);
		} else if (fd == process_file->fd) {
			file_close (process_file->file);
			list_remove (e);
			free (process_file);
			break;
		}
		e = next;
	}
	lock_release (&file_lock);
}

int mmap (int fd, void *addr)
{
	struct file *old_file = NULL;

	struct thread *cur = thread_current ();
	struct list_elem *e;
	for (e = list_begin (&cur->opened_file_list);
			e != list_end (&cur->opened_file_list);
			e = list_next(e)) {
		struct file_for_process *process_file =
				list_entry (e, struct file_for_process, file_elem);
		if (process_file == NULL) {
			break;
		}
		if (fd == process_file->fd) {
			old_file = process_file->file;
			break;
		}
	}

	if ((old_file == NULL)
			|| !is_user_vaddr(addr)
			|| addr < USER_VADDR_BOTTOM
			|| ((uint32_t) addr % PGSIZE) != 0) {
		return -1;
	}

	struct file *file = file_reopen (old_file);
	if (!file || file_length (old_file) == 0)
	{
		return -1;
	}

	cur->map_id++;
	int32_t ofs = 0;
	uint32_t read_bytes;

	for (read_bytes = file_length(file); read_bytes > 0;) {
		uint32_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		uint32_t page_zero_bytes = PGSIZE - page_read_bytes;
		if (!insert_mmap_in_page_table(file, ofs, addr, page_read_bytes, page_zero_bytes, true)) {
			remove_process_mmap (thread_current()->map_id);
			return -1;
		}
		ofs += page_read_bytes;
		addr += PGSIZE;
		read_bytes -= page_read_bytes;
	}
	return thread_current()->map_id;
}

void munmap (int map_id)
{
	struct thread *cur = thread_current ();
	struct list_elem *e, *next;
	bool close = false;

	for (e = list_begin (&cur->mmap_list);
			e != list_end (&cur->mmap_list);) {
		next = list_next(e);
		struct mmap_file *mmf = list_entry (e, struct mmap_file, mmap_elem);
		if (mmf->map_id == map_id || map_id == -1)
		{
			if (mmf->pte->is_loaded)
			{
				if (pagedir_is_dirty(cur->pagedir, mmf->pte->vaddr))
				{
					file_write_at(mmf->pte->file, mmf->pte->vaddr, mmf->pte->read_bytes, mmf->pte->ofs);
				}
				deallocate_frame_entry (pagedir_get_page(cur->pagedir, mmf->pte->vaddr));
				pagedir_clear_page (cur->pagedir, mmf->pte->vaddr);
			}

			if (mmf->pte->type != PAGE_HASH_ERROR)
			{
				hash_delete (&cur->page_table, &mmf->pte->page_elem);
			}

			list_remove (&mmf->mmap_elem);
			if (close == false) {
				file_close(mmf->pte->file);
				close = true;
			}
			free (mmf->pte);
			free (mmf);
		}
		e = next;
	}
}

static bool insert_mmap_in_page_table(struct file *file, int32_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
	void *rounded_vaddr = pg_round_down(upage);
	struct page_entry *pte = get_page_entry (rounded_vaddr);
	if (pte == NULL) {
		pte = malloc(sizeof(struct page_entry));
		if (pte == NULL) {
			return false;
		}

		pte->vaddr = upage;
		//		pte->frame = get_frame_entry(pte->vaddr);
		pte->file = file;
		pte->type = PAGE_MMAP;
		pte->ofs = ofs;
		pte->read_bytes = read_bytes;
		pte->zero_bytes = zero_bytes;
		pte->is_loaded = false;
		pte->is_writable = writable;
		pte->swap_offset = 0;
		pte->is_pinned = false;
		if (!add_process_mmap(pte)) {
			free(pte);
			return false;
		}
		if (hash_insert(&thread_current()->page_table, &pte->page_elem)) {
			pte->type = PAGE_HASH_ERROR;
			return false;
		}
		return true;
	} else {
		return false;
	}
}

static bool
add_process_mmap (struct page_entry *pte)
{
	struct thread *cur = thread_current ();
	struct mmap_file *mmf = malloc (sizeof (struct mmap_file));
	if (mmf == NULL) {
		return false;
	}
	mmf->map_id = cur->map_id;
	mmf->pte = pte;
	list_push_back (&cur->mmap_list, &mmf->mmap_elem);
	return true;
}

void
remove_process_mmap (int mapid)
{
	struct thread *cur = thread_current ();
	struct list_elem *e, *next;
	bool close = false;

	for (e = list_begin (&cur->mmap_list);
			e != list_end (&cur->mmap_list);) {
		next = list_next(e);
		struct mmap_file *mmf = list_entry (e, struct mmap_file, mmap_elem);
		if (mmf->map_id == mapid || mapid == -1)
		{
			if (mmf->pte->is_loaded)
			{
				if (pagedir_is_dirty(cur->pagedir, mmf->pte->vaddr))
				{
					file_write_at(mmf->pte->file, mmf->pte->vaddr, mmf->pte->read_bytes, mmf->pte->ofs);
				}
				deallocate_frame_entry (pagedir_get_page(cur->pagedir, mmf->pte->vaddr));
				pagedir_clear_page (cur->pagedir, mmf->pte->vaddr);
			}

			if (mmf->pte->type != PAGE_HASH_ERROR)
			{
				hash_delete (&cur->page_table, &mmf->pte->page_elem);
			}

			list_remove (&mmf->mmap_elem);
			if (close == false) {
				file_close(mmf->pte->file);
				close = true;
			}
			free (mmf->pte);
			free (mmf);
		}
		e = next;
	}
}
