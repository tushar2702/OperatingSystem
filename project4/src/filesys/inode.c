#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

static size_t bytes_to_sectors (off_t size, enum blocktype type);
static size_t inode_grow_helper (struct inode *inode, size_t new_direct_sectors, enum blocktype type);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos)
{
	ASSERT (inode != NULL);
	if (pos < inode->data_length) {
		if (pos < DIR_BLOCK_SIZE) {
			return inode->inode_block_ptrs[pos / BLOCK_SECTOR_SIZE];
		} else if (pos < (DIR_BLOCK_SIZE + INDIR_BLOCK_SIZE * NUM_INDIRECT_BLOCKS)) {
			uint32_t inode_block_ptrs_idx;
			uint32_t indirect_block[NUM_INDIRECT_BLOCK_PTRS];
			pos = pos - (DIR_BLOCK_SIZE);
			inode_block_ptrs_idx = (pos / INDIR_BLOCK_SIZE) + NUM_DIRECT_BLOCKS;
			block_read(fs_device, inode->inode_block_ptrs[inode_block_ptrs_idx], &indirect_block);
			pos = pos % INDIR_BLOCK_SIZE;
			return indirect_block[pos / BLOCK_SECTOR_SIZE];
		} else {
			uint32_t inode_block_ptrs_idx;
			uint32_t indirect_block[NUM_INDIRECT_BLOCK_PTRS];
			block_read(fs_device, inode->inode_block_ptrs[DOUBLY_INDIRECT_BLOCK_IDX], &indirect_block);
			pos = pos - (DIR_BLOCK_SIZE + INDIR_BLOCK_SIZE * NUM_INDIRECT_BLOCKS);
			inode_block_ptrs_idx = pos / INDIR_BLOCK_SIZE;
			block_read(fs_device, indirect_block[inode_block_ptrs_idx], &indirect_block);
			pos = pos % INDIR_BLOCK_SIZE;
			return indirect_block[pos / BLOCK_SECTOR_SIZE];
		}
	}
	return -1;
}

static size_t
bytes_to_sectors (off_t size, enum blocktype type)
{
	switch (type) {
		case DIRECT:
			return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
		case INDIRECT:
			if (size <= DIR_BLOCK_SIZE)
				return 0;
			size = size - (DIR_BLOCK_SIZE);
			return DIV_ROUND_UP (size, INDIR_BLOCK_SIZE);
		case DOUBLY_INDIRECT:
			if (size <= (DIR_BLOCK_SIZE + INDIR_BLOCK_SIZE * NUM_INDIRECT_BLOCKS)) {
				return 0;
			}
			return NUM_DOUBLY_INDIRECT_BLOCKS;
		default:
			PANIC ("Block type not supported.");
			return NULL;
	}
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
	list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
{
	struct inode_disk *disk_inode = NULL;
	bool success = false;

	ASSERT (length >= 0);

	/* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
	ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

	disk_inode = calloc (1, sizeof (struct inode_disk));
	if (disk_inode != NULL)
	{
		disk_inode->magic = INODE_MAGIC;
		disk_inode->parent = ROOT_DIR_SECTOR;
		disk_inode->isdir = isdir;
		if (length > MAX_FILE_SIZE)
			disk_inode->length = MAX_FILE_SIZE;
		else
			disk_inode->length = length;
		if (inode_allocate (disk_inode) == true)
		{
			block_write (fs_device, sector, disk_inode);
			success = true;
		}
		free (disk_inode);
	}
	return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
	struct list_elem *e;
	struct inode *inode;
	struct inode_disk disk_inode;

	/* Check whether this inode is already open. */
	for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
			e = list_next (e))
	{
		inode = list_entry (e, struct inode, elem);
		if (inode->sector == sector)
		{
			inode_reopen (inode);
			return inode;
		}
	}

	/* Allocate memory. */
	inode = malloc (sizeof *inode);
	if (inode == NULL)
		return NULL;

	/* Initialize. */
	list_push_front (&open_inodes, &inode->elem);
	inode->sector = sector;
	inode->open_cnt = 1;
	inode->deny_write_cnt = 0;
	inode->removed = false;
	lock_init (&inode->inode_lock);

	block_read (fs_device, inode->sector, &disk_inode);
	inode->data_length = disk_inode.length;
	inode->read_length = disk_inode.length;
	inode->parent = disk_inode.parent;
	inode->isdir = disk_inode.isdir;
	inode->dir_index = disk_inode.dir_index;
	inode->indir_index = disk_inode.indir_index;
	inode->doubly_indir_index = disk_inode.doubly_indir_index;
	memcpy (&inode->inode_block_ptrs, &disk_inode.inode_block_ptrs,
			NUM_INODE_BLOCK_PTRS * sizeof (block_sector_t));
	return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
	if (inode != NULL)
		inode->open_cnt++;
	return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
	return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
	/* Ignore null pointer. */
	if (inode == NULL)
		return;

	/* Release resources if this was the last opener. */
	if (--inode->open_cnt == 0)
	{
		/* Remove from inode list and release lock. */
		list_remove (&inode->elem);

		/* Deallocate blocks if removed. */
		if (inode->removed) {
			free_map_release (inode->sector, 1);
			inode_deallocate (inode);
		} else {
			struct inode_disk inode_disk;
			inode_disk.magic = INODE_MAGIC;
			inode_disk.length = inode->data_length;
			inode_disk.parent = inode->parent;
			inode_disk.isdir = inode->isdir;
			inode_disk.dir_index = inode->dir_index;
			inode_disk.indir_index = inode->indir_index;
			inode_disk.doubly_indir_index = inode->doubly_indir_index;
			memcpy (&inode_disk.inode_block_ptrs, &inode->inode_block_ptrs,
					NUM_INODE_BLOCK_PTRS * sizeof (block_sector_t));
			block_write (fs_device, inode->sector, &inode_disk);
		}
		free (inode);
	}
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
	ASSERT (inode != NULL);
	inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
	uint8_t *buffer = buffer_;
	off_t bytes_read = 0;
	struct buffer *buf;

	if (offset >= inode->read_length) {
		return bytes_read;
	}

	while (size > 0)
	{
		/* Disk sector to read, starting byte offset within sector. */
		block_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % BLOCK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually copy out of this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		buf = get_buffer_from_cache (sector_idx, false);
		read_ahead_sector = sector_idx + 1;
		memcpy (buffer + bytes_read, (uint8_t *) &buf->block + sector_ofs, chunk_size);
		buf->is_accessed = true;
		buf->open_count--;

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_read += chunk_size;
	}

	return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
		off_t offset)
{
	const uint8_t *buffer = buffer_;
	off_t bytes_written = 0;
	struct buffer *buf;

	if (inode->deny_write_cnt)
		return 0;

	if (offset + size > inode_length (inode)) {
		if (inode->isdir == false) {
			lock_acquire (&inode->inode_lock);
		}
		inode_grow (inode, offset + size);
		if (inode->isdir == false) {
			lock_release (&inode->inode_lock);
		}
	}

	while (size > 0)
	{
		/* Sector to write, starting byte offset within sector. */
		block_sector_t sector_idx = byte_to_sector (inode, offset);
		int sector_ofs = offset % BLOCK_SECTOR_SIZE;

		/* Bytes left in inode, bytes left in sector, lesser of the two. */
		off_t inode_left = inode_length (inode) - offset;
		int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
		int min_left = inode_left < sector_left ? inode_left : sector_left;

		/* Number of bytes to actually write into this sector. */
		int chunk_size = size < min_left ? size : min_left;
		if (chunk_size <= 0)
			break;

		buf = get_buffer_from_cache (sector_idx, true);
		memcpy ((uint8_t *) &buf->block + sector_ofs, buffer + bytes_written, chunk_size);
		buf->open_count--;
		buf->is_accessed = true;
		buf->is_dirty = true;

		/* Advance. */
		size -= chunk_size;
		offset += chunk_size;
		bytes_written += chunk_size;
	}

	inode->read_length = inode_length(inode);
	return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
	inode->deny_write_cnt++;
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
	ASSERT (inode->deny_write_cnt > 0);
	ASSERT (inode->deny_write_cnt <= inode->open_cnt);
	inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
	return inode->data_length;
}

bool
inode_isdir (const struct inode *inode)
{
	return inode->isdir;
}

bool
inode_allocate (struct inode_disk *disk_inode)
{
	struct inode inode;
	inode.data_length = 0;
	inode.dir_index = 0;
	inode.indir_index = 0;
	inode.doubly_indir_index = 0;

	inode_grow (&inode, disk_inode->length);
	disk_inode->dir_index = inode.dir_index;
	disk_inode->indir_index = inode.indir_index;
	disk_inode->doubly_indir_index = inode.doubly_indir_index;
	memcpy(&disk_inode->inode_block_ptrs, &inode.inode_block_ptrs,
			NUM_INODE_BLOCK_PTRS * sizeof (block_sector_t));
	return true;
}

void
inode_grow (struct inode *inode, off_t modified_len)
{
	size_t new_direct_sectors;
	new_direct_sectors = bytes_to_sectors (modified_len, DIRECT) -
			bytes_to_sectors (inode->data_length, DIRECT);
	if (new_direct_sectors == 0) {
		inode->data_length = modified_len;
		return;
	}

	while (inode->dir_index < INDIRECT_BLOCK_IDX) {
		new_direct_sectors = inode_grow_helper (inode, new_direct_sectors, DIRECT);
		if (new_direct_sectors == 0) {
			inode->data_length = modified_len;
			return;
		}
	}

	while (inode->dir_index < DOUBLY_INDIRECT_BLOCK_IDX) {
		new_direct_sectors = inode_grow_helper (inode, new_direct_sectors, INDIRECT);
		if (new_direct_sectors == 0) {
			inode->data_length = modified_len;
			return;
		}
	}

	while (inode->dir_index == DOUBLY_INDIRECT_BLOCK_IDX)	{
		new_direct_sectors = inode_grow_helper (inode, new_direct_sectors, DOUBLY_INDIRECT);
		if (new_direct_sectors == 0) {
			inode->data_length = modified_len;
			return;
		}
	}
}

static size_t
inode_grow_helper (struct inode *inode, size_t new_direct_sectors, enum blocktype type)
{
	static char zeros[BLOCK_SECTOR_SIZE];
	switch (type) {
		case DIRECT:
			free_map_allocate (1, &inode->inode_block_ptrs[inode->dir_index]);
			block_write(fs_device, inode->inode_block_ptrs[inode->dir_index], zeros);
			inode->dir_index++;
			return --new_direct_sectors;
		case INDIRECT:
			return inode_grow_indirect_block (inode, new_direct_sectors);
		case DOUBLY_INDIRECT:
			return inode_grow_doubly_outer_indirect_block (inode, new_direct_sectors);
	}
}

size_t
inode_grow_indirect_block (struct inode *inode, size_t new_direct_sectors)
{
	static char zeros[BLOCK_SECTOR_SIZE];
	struct indirect_block indir_block;
	if (inode->indir_index == 0) {
		free_map_allocate (1, &inode->inode_block_ptrs[inode->dir_index]);
	} else {
		block_read (fs_device, inode->inode_block_ptrs[inode->dir_index], &indir_block);
	}
	while (inode->indir_index < NUM_INDIRECT_BLOCK_PTRS) {
		free_map_allocate (1, &indir_block.indirect_block_ptrs[inode->indir_index]);
		block_write (fs_device, indir_block.indirect_block_ptrs[inode->indir_index], zeros);
		inode->indir_index++;
		new_direct_sectors--;
		if (new_direct_sectors == 0) {
			break;
		}
	}
	block_write (fs_device, inode->inode_block_ptrs[inode->dir_index], &indir_block);
	if (inode->indir_index == NUM_INDIRECT_BLOCK_PTRS) {
		inode->indir_index = 0;
		inode->dir_index++;
	}
	return new_direct_sectors;
}

size_t
inode_grow_doubly_outer_indirect_block(struct inode *inode, size_t new_direct_sectors) {
	struct indirect_block indir_block;
	if (inode->doubly_indir_index == 0 && inode->indir_index == 0) {
		free_map_allocate(1, &inode->inode_block_ptrs[inode->dir_index]);
	} else {
		block_read(fs_device, inode->inode_block_ptrs[inode->dir_index], &indir_block);
	}
	while (inode->indir_index < NUM_INDIRECT_BLOCK_PTRS) {
		new_direct_sectors = inode_grow_doubly_inner_indirect_block
				(inode, new_direct_sectors, &indir_block);
		if (new_direct_sectors == 0) {
			break;
		}
	}
	block_write(fs_device, inode->inode_block_ptrs[inode->dir_index], &indir_block);
	inode->dir_index++;
	return new_direct_sectors;
}

size_t
inode_grow_doubly_inner_indirect_block(struct inode *inode,
		size_t new_direct_sectors, struct indirect_block* outer_block) {
	static char zeros[BLOCK_SECTOR_SIZE];
	struct indirect_block inner_block;
	if (inode->doubly_indir_index == 0) {
		free_map_allocate(1,
				&outer_block->indirect_block_ptrs[inode->indir_index]);
	} else {
		block_read(fs_device, outer_block->indirect_block_ptrs[inode->indir_index],
				&inner_block);
	}
	while (inode->doubly_indir_index < NUM_INDIRECT_BLOCK_PTRS) {
		free_map_allocate(1, &inner_block.indirect_block_ptrs[inode->doubly_indir_index]);
		block_write(fs_device,
				inner_block.indirect_block_ptrs[inode->doubly_indir_index], zeros);
		inode->doubly_indir_index++;
		new_direct_sectors--;
		if (new_direct_sectors == 0) {
			break;
		}
	}
	block_write(fs_device,
			outer_block->indirect_block_ptrs[inode->indir_index], &inner_block);
	if (inode->doubly_indir_index == NUM_INDIRECT_BLOCK_PTRS) {
		inode->doubly_indir_index = 0;
		inode->indir_index++;
	}
	return new_direct_sectors;
}

void
inode_deallocate (struct inode *inode)
{
	size_t direct_sectors = bytes_to_sectors (inode->data_length, DIRECT);
	size_t indirect_sectors = bytes_to_sectors (inode->data_length, INDIRECT);
	size_t doubly_indirect_sector = bytes_to_sectors (inode->data_length, DOUBLY_INDIRECT);

	int idx;
	for (idx = 0; direct_sectors && idx < INDIRECT_BLOCK_IDX; idx++) {
		free_map_release (inode->inode_block_ptrs[idx], 1);
		direct_sectors--;
	}

	for (; indirect_sectors && idx < DOUBLY_INDIRECT_BLOCK_IDX; idx++) {
		size_t direct_ptrs;
		if (direct_sectors < NUM_INDIRECT_BLOCK_PTRS) {
			direct_ptrs = direct_sectors;
		} else {
			direct_ptrs = NUM_INDIRECT_BLOCK_PTRS;
		}

		// Deallocate indirect block
		block_sector_t *indir_blk_ptr = &inode->inode_block_ptrs[idx];
		struct indirect_block indir_block;
		block_read(fs_device, indir_blk_ptr, &indir_block);
		int i;
		for (i = 0; i < direct_ptrs; i++) {
			free_map_release (indir_block.indirect_block_ptrs[i], 1);
		}
		free_map_release (indir_blk_ptr, 1);

		direct_sectors = direct_sectors - direct_ptrs;
		indirect_sectors--;
	}

	if (doubly_indirect_sector) {
		inode_deallocate_doubly_indir_block (&inode->inode_block_ptrs[idx],
				indirect_sectors, direct_sectors);
	}
}

void
inode_deallocate_doubly_indir_block (block_sector_t *ptr,
		size_t indirect_ptrs, size_t direct_ptrs)
{
	struct indirect_block indir_block;
	block_read(fs_device, *ptr, &indir_block);
	int i;
	for (i = 0; i < indirect_ptrs; i++) {
		size_t data;
		if (direct_ptrs < NUM_INDIRECT_BLOCK_PTRS) {
			data = direct_ptrs;
		} else {
			data = NUM_INDIRECT_BLOCK_PTRS;
		}

		// Deallocate indirect block
		block_sector_t *indir_blk_ptr = &indir_block.indirect_block_ptrs[i];
		struct indirect_block indir_block;
		block_read(fs_device, indir_blk_ptr, &indir_block);
		int i;
		for (i = 0; i < direct_ptrs; i++) {
			free_map_release (indir_block.indirect_block_ptrs[i], 1);
		}
		free_map_release (indir_blk_ptr, 1);

		direct_ptrs -= data;
	}
	free_map_release(*ptr, 1);
}
