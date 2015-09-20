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

// Lock required for file related syscalls
struct lock file_lock;

static void syscall_handler (struct intr_frame *);
static void check_ptr_validity (const void *vaddr);
static int map_user_to_kernel_vaddr (const void* vaddr);
static void check_buffer_validity (void* buffer, unsigned size);
static void extract_args (struct intr_frame *f, int numargs, int *args);

void
syscall_init (void)
{
	lock_init(&file_lock);
	intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/*
 * Checks whether a pointer is within the user
 * address space and whether there is a mapping
 * from user vaddr to kernel vaddr. Exits with
 * -1 status if the pointer is not valid.
 */
static void
check_ptr_validity (const void *vaddr)
{
	if (vaddr != NULL
			&& (is_user_vaddr (vaddr))
			&& pagedir_get_page(thread_current ()->pagedir, vaddr)) {
		return;
	} else {
		exit(-1);
	}
}

/*
 * Returns the kernel virtual address corresponding to
 * user virtual address. If not present, exits with a
 * -1 status.
 */
static int
map_user_to_kernel_vaddr (const void *vaddr)
{
	check_ptr_validity (vaddr);
	struct thread *cur = thread_current ();
	void *kernel_vaddr = pagedir_get_page (cur->pagedir, vaddr);
	if (!kernel_vaddr)
	{
		exit (-1);
	}
	return (int) kernel_vaddr;
}

/*
 * Used for read/write syscalls to check memory
 * pointer validity of the buffer to read/write.
 */
static void
check_buffer_validity (void* buffer, unsigned size)
{
	unsigned i;
	char *temp_buffer = (char *) buffer;
	for (i=0; i<size; i++) {
		check_ptr_validity ((const void*) temp_buffer);
		temp_buffer++;
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
		check_ptr_validity(ptr);
		args[i-1] = *ptr;
	}
}

static void
syscall_handler (struct intr_frame *f UNUSED)
{
    int args[3];
    check_ptr_validity ((const void*) f->esp);
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
        args[0] = map_user_to_kernel_vaddr((const void*) args[0]);
        f->eax = exec ((const char*) args[0]);
        break;

    case SYS_WAIT:
        extract_args (f, 1, &args[0]);
        f->eax = wait ((pid_t) args[0]);
        break;

    case SYS_CREATE:
        extract_args (f, 2, &args[0]);
        args[0] = map_user_to_kernel_vaddr((const void*) args[0]);
        f->eax = create ((const char*) args[0], (unsigned) args[1]);
        break;

    case SYS_REMOVE:
        extract_args (f, 1, &args[0]);
        args[0] = map_user_to_kernel_vaddr((const void*) args[0]);
        f->eax = remove ((const char*) args[0]);
        break;

    case SYS_OPEN:
        extract_args (f, 1, &args[0]);
        args[0] = map_user_to_kernel_vaddr((const void*) args[0]);
        f->eax = open ((const char*) args[0]);
        break;

    case SYS_FILESIZE:
        extract_args (f, 1, &args[0]);
        f->eax = filesize (args[0]);
        break;

    case SYS_READ:
        extract_args (f, 3, &args[0]);
        check_buffer_validity ((void *) args[1], (unsigned) args[2]);
        args[1] = map_user_to_kernel_vaddr((const void*) args[1]);
        f->eax = read ((int) args[0], (void *) args[1], (unsigned) args[2]);
        break;

    case SYS_WRITE:
        extract_args (f, 3, &args[0]);
        check_buffer_validity ((void *) args[1], (unsigned) args[2]);
        args[1] = map_user_to_kernel_vaddr((const void*) args[1]);
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
		if (child->tid == pid) {
			while (child->load == UNLOADED) {
				barrier ();
			}
			if (child->load == LOAD_SUCCESS) {
				return pid;
			} else {
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
	if (!file_to_open) {
		is_open = -1;
	} else {
		struct thread *cur = thread_current ();
		struct file_for_process *process_file =
				malloc (sizeof *process_file);
		process_file->fd = cur->fd;
		process_file->file = file_to_open;
		is_open = cur->fd;
		cur->fd++;
		list_push_back (&cur->opened_file_list, &process_file->file_elem);
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
			return;
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
