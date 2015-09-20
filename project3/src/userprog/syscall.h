#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

#define USER_VADDR_BOTTOM ((void *) 0x08048000)

typedef int pid_t;
/*
 * This is defined here and not in thread struct
 * because a file can be opened by multiple threads/
 * processes at a time. The list_elem in this struct
 * is stored in the threads' opened_file_list because
 * a thread too can open multiple files and it has to
 * keep track of them.
 */
struct file_for_process
{
	struct file *file;
	int fd;
	struct list_elem file_elem;
};

void syscall_init (void);

void halt (void);
void exit (int status);
pid_t exec (const char *cmd_line);
int wait (pid_t pid);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
int write (int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);
void close_file (int fd);

void remove_process_mmap (int mapid);

#endif /* userprog/syscall.h */
