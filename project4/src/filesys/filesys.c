#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/cache.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
struct dir *get_file_dir (const char *path);
char* get_file_name(const char* path);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  buffer_cache_init();
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  bool sys_halt = true;
  write_cache_to_disk (sys_halt);
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool is_dir)
{
  block_sector_t inode_sector = 0;
  struct dir *dir;
  char *filename;
  dir = get_file_dir (name);
  filename = get_file_name (name);
  bool success = false;
  if (strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0) {
	  success = (dir != NULL && free_map_allocate(1, &inode_sector)
	  && inode_create (inode_sector, initial_size, is_dir)
	  && dir_add (dir, filename, inode_sector));
  }
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  free (filename);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  if (strlen (name) == 0)
	  return NULL;
  struct dir *dir;
  struct inode *inode = NULL;
  char *filename;
  dir = get_file_dir (name);
  filename = get_file_name (name);

  if (dir != NULL) {
	  if ((is_root_directory (dir)
	  			  && strlen (filename) == 0)
	  			  || strcmp (filename, ".") == 0) {
	  		  free (filename);
	  		  return (struct file *) dir;
	  } else if (strcmp (filename, "..") == 0) {
		  if (!get_parent_directory (dir, &inode)) {
			  free (filename);
			  return NULL;
		  }
	  } else {
		  dir_lookup (dir, filename, &inode);
	  }
  }

  free (filename);
  dir_close (dir);
  if (inode != NULL) {
	  if (inode_isdir (inode)) {
		  return (struct file*) dir_open (inode);
	  }
	  return file_open (inode);
  }
  return NULL;
}

bool
filesys_chdir (const char *name)
{
  struct dir *dir;
  struct inode *inode = NULL;
  char *filename;
  dir = get_file_dir (name);
  filename = get_file_name (name);

  if (dir != NULL) {
	  if ((is_root_directory (dir)
			  && strlen (filename) == 0)
			  || strcmp (filename, ".") == 0) {
		  thread_current ()->cur_working_dir = dir;
		  free (filename);
		  return true;
	  } else if (strcmp (filename, "..") == 0) {
  		  if (!get_parent_directory (dir, &inode)) {
  			  free (filename);
  			  return false;
  		  }
  	  } else {
  		  dir_lookup (dir, filename, &inode);
  	  }
   }
  free (filename);
  dir_close (dir);
  dir = dir_open (inode);
  if (!dir)
	  return false;

  dir_close (thread_current ()->cur_working_dir);
  thread_current ()->cur_working_dir = dir;
  return true;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir;
  char *filename;
  dir = get_file_dir (name);
  filename = get_file_name (name);
  bool success = dir != NULL && dir_remove (dir, filename);
  dir_close (dir); 
  free (filename);
  return success;
}

struct dir*
get_file_dir(const char* path)
{
	struct dir* dir;
	char *save_ptr, *token, *next_token = NULL;
	struct inode *inode;

	char file_path[strlen(path) + 1];
	memcpy(file_path, path, strlen(path) + 1);

	if (file_path[0] == 47 || !thread_current()->cur_working_dir) {
		dir = dir_open_root();
	} else {
		dir = dir_reopen(thread_current()->cur_working_dir);
	}

	token = strtok_r (file_path, "/", &save_ptr);
	next_token = strtok_r (NULL, "/", &save_ptr);
	for (; next_token != NULL;
			next_token = strtok_r (NULL, "/", &save_ptr)) {
		if ((strcmp (token, ".") != 0
				&& (strcmp (token, "..") == 0)
				&& (!get_parent_directory (dir, &inode)))
				|| !dir_lookup (dir, token, &inode)) {
			return NULL;
		}

		if (inode_isdir (inode)) {
			dir_close (dir);
			dir = dir_open (inode);
		}
		token = next_token;
	}

	return dir;
}

char* get_file_name(const char* path) {
	char *save_ptr, *file_name, *token, *prev_token = "";
	char file_path[strlen(path) + 1];
	memcpy(file_path, path, strlen(path) + 1);

	token = strtok_r(file_path, "/", &save_ptr);
	for (; token != NULL;
			token = strtok_r(NULL, "/", &save_ptr)) {
		prev_token = token;
	}

	file_name = malloc(strlen(prev_token) + 1);
	if (!file_name)
		return NULL;
	memcpy(file_name, prev_token, strlen(prev_token) + 1);
	return file_name;
}


/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
