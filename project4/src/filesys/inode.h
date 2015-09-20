#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include <list.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NUM_DIRECT_BLOCKS 4
#define NUM_INDIRECT_BLOCKS 9
#define NUM_DOUBLY_INDIRECT_BLOCKS 1
#define NUM_INDIRECT_BLOCK_PTRS (512/4)
#define NUM_INODE_BLOCK_PTRS 14

#define DIR_BLOCK_SIZE (BLOCK_SECTOR_SIZE * NUM_DIRECT_BLOCKS)
#define INDIR_BLOCK_SIZE (BLOCK_SECTOR_SIZE * NUM_INDIRECT_BLOCK_PTRS)

#define DIRECT_BLOCK_IDX 0
#define INDIRECT_BLOCK_IDX 4
#define DOUBLY_INDIRECT_BLOCK_IDX 13

#define MAX_FILE_SIZE 8980480

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	/* Pointers to inode blocks. */
	block_sector_t inode_block_ptrs[NUM_INODE_BLOCK_PTRS];
	uint32_t dir_index;
	uint32_t indir_index;
	uint32_t doubly_indir_index;
	bool isdir;
	block_sector_t parent;
	uint32_t unused[107];               /* Not used. */
};

/* In-memory inode. */
struct inode
{
	struct list_elem elem;              /* Element in inode list. */
	block_sector_t sector;              /* Sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	off_t data_length;                  /* File size in bytes. */
	off_t read_length;
	/* Pointers to inode blocks. */
	block_sector_t inode_block_ptrs[NUM_INODE_BLOCK_PTRS];
	size_t dir_index;
	size_t indir_index;
	size_t doubly_indir_index;
	bool isdir;
	block_sector_t parent;
	struct lock inode_lock;

};

struct indirect_block
{
	block_sector_t indirect_block_ptrs[NUM_INDIRECT_BLOCK_PTRS];
};

enum blocktype {
	DIRECT,
	INDIRECT,
	DOUBLY_INDIRECT
};

struct bitmap;

void inode_init (void);
bool inode_create (block_sector_t, off_t, bool);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool inode_isdir (const struct inode *);

bool inode_allocate (struct inode_disk *inode_disk);
void inode_grow (struct inode *inode, off_t modified_len);
size_t inode_grow_direct_block (struct inode *inode, size_t new_direct_sectors);
size_t inode_grow_indirect_block (struct inode *inode,
		size_t new_direct_sectors);
size_t inode_grow_doubly_outer_indirect_block (struct inode *inode,
		size_t new_direct_sectors);
size_t inode_grow_doubly_inner_indirect_block (struct inode *inode,
		size_t new_direct_sectors, struct indirect_block *outer_block);

void inode_deallocate (struct inode *inode);
void inode_deallocate_doubly_indir_block (block_sector_t *ptr,
		size_t indirect_ptrs, size_t direct_ptrs);

#endif /* filesys/inode.h */
