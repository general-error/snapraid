/*
 * Copyright (C) 2011 Andrea Mazzoleni
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "portable.h"

#include "util.h"
#include "elem.h"
#include "state.h"

struct snapraid_scan {
	/**
	 * Counters of changes.
	 */
	unsigned count_equal; /**< Files equal. */
	unsigned count_move; /**< Files with a different name, but equal inode, size and timestamp. */
	unsigned count_restore; /**< Files with equal name, size and timestamp, but different inode. */
	unsigned count_change; /**< Files modified. */
	unsigned count_remove; /**< Files removed. */
	unsigned count_insert; /**< Files new. */

	tommy_list file_insert_list; /**< Files to insert. */
	tommy_list link_insert_list; /**< Links to insert. */
	tommy_list dir_insert_list; /**< Dirs to insert. */

	/* nodes for data structures */
	tommy_node node;
};

/**
 * Removes the specified link from the data set.
 */
static void scan_link_remove(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_link* link)
{
	/* state changed */
	state->need_write = 1;

	/* remove the file from the link containers */
	tommy_hashdyn_remove_existing(&disk->linkset, &link->nodeset);
	tommy_list_remove_existing(&disk->linklist, &link->nodelist);

	/* deallocate */
	link_free(link);
}

/**
 * Inserts the specified link in the data set.
 */
static void scan_link_insert(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_link* link)
{
	/* state changed */
	state->need_write = 1;

	/* insert the link in the link containers */
	tommy_hashdyn_insert(&disk->linkset, &link->nodeset, link, link_name_hash(link->sub));
	tommy_list_insert_tail(&disk->linklist, &link->nodelist, link);
}

/**
 * Processes a symbolic link.
 */
static void scan_link(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* sub, const char* linkto, unsigned link_flag)
{
	struct snapraid_link* link;

	/* check if the link already exists */
	link = tommy_hashdyn_search(&disk->linkset, link_name_compare, sub, link_name_hash(sub));
	if (link) {
		/* check if multiple files have the same name */
		if (link_flag_has(link, FILE_IS_PRESENT)) {
			fprintf(stderr, "Internal inconsistency for link '%s%s'\n", disk->dir, sub);
			exit(EXIT_FAILURE);
		}

		/* mark as present */
		link_flag_set(link, FILE_IS_PRESENT);

		/* check if the link is not changed and it's of the same kind */
		if (strcmp(link->linkto, linkto) == 0 && link_flag == link_flag_get(link, FILE_IS_LINK_MASK)) {
			/* it's equal */
			++scan->count_equal;

			if (state->opt.gui) {
				fprintf(stdlog, "scan:equal:%s:%s\n", disk->name, link->sub);
				fflush(stdlog);
			}
		} else {
			/* it's an update */

			/* we have to save the linkto/type */
			state->need_write = 1;

			++scan->count_change;

			if (state->opt.gui) {
				fprintf(stdlog, "scan:update:%s:%s\n", disk->name, link->sub);
				fflush(stdlog);
			}
			if (output) {
				printf("Update '%s%s'\n", disk->dir, link->sub);
			}

			/* update it */
			free(link->linkto);
			link->linkto = strdup_nofail(linkto);
			link_flag_let(link, link_flag, FILE_IS_LINK_MASK);
		}

		/* nothing more to do */
		return;
	} else {
		/* create the new link */
		++scan->count_insert;

		if (state->opt.gui) {
			fprintf(stdlog, "scan:add:%s:%s\n", disk->name, sub);
			fflush(stdlog);
		}
		if (output) {
			printf("Add '%s%s'\n", disk->dir, sub);
		}

		/* and continue to insert it */
	}

	/* insert it */
	link = link_alloc(sub, linkto, link_flag);

	/* mark it as present */
	link_flag_set(link, FILE_IS_PRESENT);

	/* insert it in the delayed insert list */
	tommy_list_insert_tail(&scan->link_insert_list, &link->nodelist, link);
}

/**
 * Removes the specified file from the data set.
 */
static void scan_file_remove(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_file* file)
{
	block_off_t i;

	/* state changed */
	state->need_write = 1;

	/* free all the blocks of the file */
	for(i=0;i<file->blockmax;++i) {
		struct snapraid_block* block = &file->blockvec[i];
		block_off_t block_pos = block->parity_pos;
		unsigned block_state;
		struct snapraid_deleted* deleted;

		/* adjust the first free position */
		/* note that doing all the deletions before alllocations, */
		/* first_free_block is always 0 and the "if" is never triggered */
		/* but we keep this code anyway for completeness. */
		if (disk->first_free_block > block_pos)
			disk->first_free_block = block_pos;

		/* in case we scan after an aborted sync, */
		/* we could get also intermediate states like inv/chg/new */
		block_state = block_state_get(block);
		switch (block_state) {
		case BLOCK_STATE_BLK :
			/* we keep the hash making it an "old" hash, because the parity is still containing data for it */
			break;
		case BLOCK_STATE_CHG :
		case BLOCK_STATE_NEW :
			/* if we have not already cleared the undeterminated hash */
			if (!state->clear_undeterminate_hash) {
				/* in these cases we don't know if the old state is still the one */
				/* stored inside the parity, because after an aborted sync, the parity */
				/* may be or may be not have been updated with the new data */
				/* Then we reset the hash to a bogus value */
				/* Note that this condition is possible only if: */
				/* - new files added/modified */
				/* - aborted sync, without saving the content file */
				/* - files deleted after the aborted sync */
				memset(block->hash, 0, HASH_SIZE);
			}
			break;
		default:
			fprintf(stderr, "Internal state inconsistency in scanning for block %u state %u\n", block->parity_pos, block_state);
			exit(EXIT_FAILURE);
		}

		/* allocated a new deleted block from the block we are going to delete */
		deleted = deleted_dup(block);

		/* insert it in the list of deleted blocks */
		tommy_list_insert_tail(&disk->deletedlist, &deleted->node, deleted);

		/* set the deleted block in the block array */
		tommy_arrayblk_set(&disk->blockarr, block_pos, &deleted->block);
	}

	/* remove the file from the file containers */
	if (!file_flag_has(file, FILE_IS_WITHOUT_INODE)) {
		tommy_hashdyn_remove_existing(&disk->inodeset, &file->nodeset);
	}
	tommy_hashdyn_remove_existing(&disk->pathset, &file->pathset);
	tommy_list_remove_existing(&disk->filelist, &file->nodelist);

	/* deallocate */
	file_free(file);
}

/**
 * Inserts the specified file in the data set.
 */
static void scan_file_insert(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_file* file)
{
	block_off_t i;
	block_off_t block_max;
	block_off_t block_pos;

	/* state changed */
	state->need_write = 1;

	/* allocate the blocks of the file */
	block_pos = disk->first_free_block;
	block_max = tommy_arrayblk_size(&disk->blockarr);
	for(i=0;i<file->blockmax;++i) {
		struct snapraid_block* block;

		/* find a free block */
		while (block_pos < block_max && block_has_file(tommy_arrayblk_get(&disk->blockarr, block_pos)))
			++block_pos;

		/* if not found, allocate a new one */
		if (block_pos == block_max) {
			++block_max;
			tommy_arrayblk_grow(&disk->blockarr, block_max);
		}

		/* set the position */
		file->blockvec[i].parity_pos = block_pos;

		/* block to overwrite */
		block = tommy_arrayblk_get(&disk->blockarr, block_pos);

		/* if the block is an empty one */
		if (block == BLOCK_EMPTY) {
			/* we just overwrite it with a NEW one */
			block_state_set(&file->blockvec[i], BLOCK_STATE_NEW);
		} else {
			/* otherwise it's a DELETED one */

			/* if we have not already cleared the undeterminated hash */
			if (!state->clear_undeterminate_hash) {
				/* in this case we don't know if the old state is still the one */
				/* stored inside the parity, because after an aborted sync, the parity */
				/* may be or may be not have been updated with the new data */
				/* Then we reset the hash to a bogus value */
				/* Note that this condition is possible only if: */
				/* - files are deleted */
				/* - aborted sync, without saving the content file */
				/* - files are readded after the aborted sync */
				memset(block->hash, 0, HASH_SIZE);
			}

			block_state_set(&file->blockvec[i], BLOCK_STATE_CHG);
			memcpy(file->blockvec[i].hash, block->hash, HASH_SIZE);
		}

		/* store in the disk map, after invalidating all the other blocks */
		tommy_arrayblk_set(&disk->blockarr, block_pos, &file->blockvec[i]);
	}
	if (file->blockmax) {
		/* set the new free position, but only if allocated something */
		disk->first_free_block = block_pos + 1;
	}

	/* note that the file is already added in the file hashtables */
	tommy_list_insert_tail(&disk->filelist, &file->nodelist, file);
}

/**
 * Processes a file.
 */
static void scan_file(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* sub, const struct stat* st, uint64_t physical)
{
	struct snapraid_file* file;
	uint64_t inode;

	file = 0;

	/*
	 * If the disk has persistent inodes, try a search by inode,
	 * to detect moved files.
	 *
	 * For persistent inodes we mean inodes that keep their values when the filesystem
	 * is unmounted and remounted. This don't always happen.
	 *
	 * Cases found are:
	 * - Linux FUSE with exFAT driver from https://code.google.com/p/exfat/.
	 *   Inodes are reassigned at every mount restarting from 1 and incrementing.
	 *   As worse, the exFAT support in FUSE doesn't use subsecond precision in timestamps
	 *   making inode collision more easy (exFAT by design supports 10ms precision).
	 * - Linux VFAT kernel (3.2) driver. Inodes are fully reassigned at every mount.
	 *
	 * In such cases, to avoid possible random collisions, it's better to disable the moved
	 * file recognition.
	 *
	 * We do this implicitely removing all the inode before searching for files.
	 * This ensure that no file is found with an old inode, but at the same time,
	 * it allows to find new files with the same inode, and to identify them as hardlinks.
	 */

	inode = st->st_ino;
	file = tommy_hashdyn_search(&disk->inodeset, file_inode_compare_to_arg, &inode, file_inode_hash(inode));

	/* identify moved files searching by inode */
	if (file) {
		/* check if the file is not changed */
		if (file->size == st->st_size
			&& file->mtime_sec == st->st_mtime
			&& (file->mtime_nsec == STAT_NSEC(st)
				/* always accept the stored value if it's STAT_NSEC_INVALID */
				/* it happens when upgrading from an old version of SnapRAID */
				/* not yet supporting the nanosecond field */
				|| file->mtime_nsec == STAT_NSEC_INVALID
			)
		) {
			/* check if multiple files have the same inode */
			if (file_flag_has(file, FILE_IS_PRESENT)) {
				if (st->st_nlink > 1) {
					/* it's a hardlink */
					scan_link(scan, state, output, disk, sub, file->sub, FILE_IS_HARDLINK);
					return;
				} else {
					fprintf(stderr, "Internal inode '%"PRIu64"' inconsistency for file '%s%s' already present\n", (uint64_t)st->st_ino, disk->dir, sub);
					exit(EXIT_FAILURE);
				}
			}

			/* mark as present */
			file_flag_set(file, FILE_IS_PRESENT);

			/* update the nano seconds mtime only if different */
			/* to avoid unneeded updates */
			if (file->mtime_nsec == STAT_NSEC_INVALID
				&& STAT_NSEC(st) != STAT_NSEC_INVALID
			) {
				file->mtime_nsec = STAT_NSEC(st);

				/* we have to save the new mtime */
				state->need_write = 1;
			}

			if (strcmp(file->sub, sub) != 0) {
				/* if the path is different, it means a moved file with the same inode */
				++scan->count_move;

				if (state->opt.gui) {
					fprintf(stdlog, "scan:move:%s:%s:%s\n", disk->name, file->sub, sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Move '%s%s' '%s%s'\n", disk->dir, file->sub, disk->dir, sub);
				}

				/* remove from the name set */
				tommy_hashdyn_remove_existing(&disk->pathset, &file->pathset);

				/* save the new name */
				file_rename(file, sub);

				/* reinsert in the name set */
				tommy_hashdyn_insert(&disk->pathset, &file->pathset, file, file_path_hash(file->sub));

				/* we have to save the new name */
				state->need_write = 1;
			} else {
				/* otherwise it's equal */
				++scan->count_equal;

				if (state->opt.gui) {
					fprintf(stdlog, "scan:equal:%s:%s\n", disk->name, file->sub);
					fflush(stdlog);
				}
			}

			/* nothing more to do */
			return;
		}

		/* here the file matches the inode, but not the other info */
		/* if could be a modified file with the same name, */
		/* or a restored/copied file that get assigned a previously used inode, */
		/* or a filesystem with not persistent inodes */

		/* for sure it cannot be already present */
		if (file_flag_has(file, FILE_IS_PRESENT)) {
			fprintf(stderr, "Internal inode '%"PRIu64"' inconsistency for files '%s%s' and '%s%s' matching and already present but different\n", file->inode, disk->dir, sub, disk->dir, file->sub);
			exit(EXIT_FAILURE);
		}

		/* assume a previously used inode, it's the worst case */
		/* and we handle it removing the duplicate stored inode. */
		/* If the file is found by name (not necessarely in this function call), */
		/* it will have the inode restored, otherwise, it will get removed */

		/* remove from the inode set */
		tommy_hashdyn_remove_existing(&disk->inodeset, &file->nodeset);

		/* clear the inode */
		/* this is not really needed for correct functionality */
		/* because we are going to set FILE_IS_WITHOUT_INODE */
		/* but it's easier for debugging to have invalid inodes set to 0 */
		file->inode = 0;

		/* mark as missing inode */
		file_flag_set(file, FILE_IS_WITHOUT_INODE);

		/* go further to find it by name */
	}

	/* then try findind it by name */
	file = tommy_hashdyn_search(&disk->pathset, file_path_compare, sub, file_path_hash(sub));
	if (file) {
		/* if the file is without an inode */
		if (file_flag_has(file, FILE_IS_WITHOUT_INODE)) {
			/* set it now */
			file->inode = st->st_ino;

			/* insert in the set */
			tommy_hashdyn_insert(&disk->inodeset, &file->nodeset, file, file_inode_hash(file->inode));

			/* unmark as missing inode */
			file_flag_clear(file, FILE_IS_WITHOUT_INODE);
		} else {
			/* here the inode has to be different, otherwise we would have found it before */
			if (file->inode == st->st_ino) {
				fprintf(stderr, "Internal inode  '%"PRIu64"' inconsistency for files '%s%s' as unexpected matching\n", file->inode, disk->dir, sub);

				exit(EXIT_FAILURE);
			}
		}

		/* for sure it cannot be already present */
		if (file_flag_has(file, FILE_IS_PRESENT)) {
			fprintf(stderr, "Internal path inconsistency for file '%s%s' matching and already present\n", disk->dir, sub);
			exit(EXIT_FAILURE);
		}

		/* check if the file is not changed */
		if (file->size == st->st_size
			&& file->mtime_sec == st->st_mtime
			&& (file->mtime_nsec == STAT_NSEC(st)
				/* always accept the stored value if it's STAT_NSEC_INVALID */
				/* it happens when upgrading from an old version of SnapRAID */
				/* not yet supporting the nanosecond field */
				|| file->mtime_nsec == STAT_NSEC_INVALID
			)
		) {
			/* mark as present */
			file_flag_set(file, FILE_IS_PRESENT);

			/* update the nano seconds mtime only if different */
			/* to avoid unneeded updates */
			if (file->mtime_nsec == STAT_NSEC_INVALID
				&& STAT_NSEC(st) != STAT_NSEC_INVALID
			) {
				file->mtime_nsec = STAT_NSEC(st);

				/* we have to save the new mtime */
				state->need_write = 1;
			}

			/* if the disk support persistent inodes */
			if (!disk->has_not_persistent_inodes) {
				/* if persistent inodes are supported, we are sure that the inode number */
				/* is now different, because otherwise the file would have been found */
				/* when searching by inode. */
				/* if the inode is different, it means a rewritten file with the same path */
				/* like when restoring a backup that restores also the timestamp */
				++scan->count_restore;

				if (state->opt.gui) {
					fprintf(stdlog, "scan:restore:%s:%s\n", disk->name, sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Restore '%s%s'\n", disk->dir, sub);
				}

				/* remove from the inode set */
				tommy_hashdyn_remove_existing(&disk->inodeset, &file->nodeset);

				/* save the new inode */
				file->inode = st->st_ino;

				/* reinsert in the inode set */
				tommy_hashdyn_insert(&disk->inodeset, &file->nodeset, file, file_inode_hash(file->inode));

				/* we have to save the new inode */
				state->need_write = 1;
			} else {
				/* otherwise it's the case of not persistent inode, where doesn't */
				/* matter if the inode is different or equal, because they have no */
				/* meaning, and then we don't even save them */
				++scan->count_equal;

				if (state->opt.gui) {
					fprintf(stdlog, "scan:equal:%s:%s\n", disk->name, file->sub);
					fflush(stdlog);
				}
			}

			/* nothing more to do */
			return;
		}

		/* here if the file is changed but with the correct name */
		
		/* do a safety check to ensure that the common ext4 case of zeroing */
		/* the size of a file after a crash doesn't propagate to the backup */
		if (file->size != 0 && st->st_size == 0) {
			if (!state->opt.force_zero) {
				fprintf(stderr, "The file '%s%s' has unexpected zero size! If this an expected state\n", disk->dir, sub);
				fprintf(stderr, "you can '%s' anyway usinge 'snapraid --force-zero %s'\n", state->command, state->command);
				fprintf(stderr, "Instead, it's possible that after a kernel crash this file was lost,\n");
				fprintf(stderr, "and you can use 'snapraid --filter %s fix' to recover it.\n", sub);
				exit(EXIT_FAILURE);
			}
		}

		/* it has the same name, so it's an update */
		++scan->count_change;

		if (state->opt.gui) {
			fprintf(stdlog, "scan:update:%s:%s\n", disk->name, file->sub);
			fflush(stdlog);
		}
		if (output) {
			if (file->size != st->st_size)
				printf("Update '%s%s' new size\n", disk->dir, file->sub);
			else
				printf("Update '%s%s' new modification time\n", disk->dir, file->sub);
		}

		/* remove it */
		scan_file_remove(state, disk, file);

		/* and continue to reinsert it */
	} else {
		/* if the name doesn't exist, it's a new file */
		++scan->count_insert;

		if (state->opt.gui) {
			fprintf(stdlog, "scan:add:%s:%s\n", disk->name, sub);
			fflush(stdlog);
		}
		if (output) {
			printf("Add '%s%s'\n", disk->dir, sub);
		}

		/* and continue to insert it */
	}

	/* insert it */
	file = file_alloc(state->block_size, sub, st->st_size, st->st_mtime, STAT_NSEC(st), st->st_ino, physical);

	/* mark it as present */
	file_flag_set(file, FILE_IS_PRESENT);

	/* insert the file in the file hashtables, to allow to find duplicate hardlinks */
	tommy_hashdyn_insert(&disk->inodeset, &file->nodeset, file, file_inode_hash(file->inode));
	tommy_hashdyn_insert(&disk->pathset, &file->pathset, file, file_path_hash(file->sub));

	/* insert the file in the delayed block allocation */
	tommy_list_insert_tail(&scan->file_insert_list, &file->nodelist, file);
}

/**
 * Removes the specified dir from the data set.
 */
static void scan_emptydir_remove(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_dir* dir)
{
	/* state changed */
	state->need_write = 1;

	/* remove the file from the dir containers */
	tommy_hashdyn_remove_existing(&disk->dirset, &dir->nodeset);
	tommy_list_remove_existing(&disk->dirlist, &dir->nodelist);

	/* deallocate */
	dir_free(dir);
}

/**
 * Inserts the specified dir in the data set.
 */
static void scan_emptydir_insert(struct snapraid_state* state, struct snapraid_disk* disk, struct snapraid_dir* dir)
{
	/* state changed */
	state->need_write = 1;

	/* insert the dir in the dir containers */
	tommy_hashdyn_insert(&disk->dirset, &dir->nodeset, dir, dir_name_hash(dir->sub));
	tommy_list_insert_tail(&disk->dirlist, &dir->nodelist, dir);
}

/**
 * Processes a dir.
 */
static void scan_emptydir(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* sub)
{
	struct snapraid_dir* dir;

	/* check if the dir already exists */
	dir = tommy_hashdyn_search(&disk->dirset, dir_name_compare, sub, dir_name_hash(sub));
	if (dir) {
		/* check if multiple files have the same name */
		if (dir_flag_has(dir, FILE_IS_PRESENT)) {
			fprintf(stderr, "Internal inconsistency for dir '%s%s'\n", disk->dir, sub);
			exit(EXIT_FAILURE);
		}

		/* mark as present */
		dir_flag_set(dir, FILE_IS_PRESENT);

		/* it's equal */
		++scan->count_equal;

		if (state->opt.gui) {
			fprintf(stdlog, "scan:equal:%s:%s\n", disk->name, dir->sub);
			fflush(stdlog);
		}

		/* nothing more to do */
		return;
	} else {
		/* create the new dir */
		++scan->count_insert;

		if (state->opt.gui) {
			fprintf(stdlog, "scan:add:%s:%s\n", disk->name, sub);
			fflush(stdlog);
		}
		if (output) {
			printf("Add '%s%s'\n", disk->dir, sub);
		}

		/* and continue to insert it */
	}

	/* insert it */
	dir = dir_alloc(sub);

	/* mark it as present */
	dir_flag_set(dir, FILE_IS_PRESENT);

	/* insert it in the delayed insert list */
	tommy_list_insert_tail(&scan->dir_insert_list, &dir->nodelist, dir);
}

struct dirent_sorted {
	/* node for data structures */
	tommy_node node;

#if HAVE_STRUCT_DIRENT_D_INO
	uint64_t d_ino; /**< Inode number. */
#endif
#if HAVE_STRUCT_DIRENT_D_TYPE
	uint32_t d_type; /**< File type. */
#endif
#if HAVE_STRUCT_DIRENT_D_STAT
	struct stat d_stat; /**< Stat result. */
#endif
	char d_name[1]; /**< Variable length name. It must be the last field. */
};

#ifndef _WIN32 /* In Windows doesn't sort */
static int dd_ino_compare(const void* void_a, const void* void_b)
{
	const struct dirent_sorted* a = void_a;
	const struct dirent_sorted* b = void_b;

	if (a->d_ino < b->d_ino)
		return -1;
	if (a->d_ino > b->d_ino)
		return 1;

	return 0;
}
#endif

/**
 * Returns the stat info of a dir entry.
 */
#if HAVE_STRUCT_DIRENT_D_STAT
#define DSTAT(file, dd, buf) dstat(dd)
struct stat* dstat(struct dirent_sorted* dd)
{
	return &dd->d_stat;
}
#else
#define DSTAT(file, dd, buf) dstat(file, buf)
struct stat* dstat(const char* file, struct stat* st)
{
	if (lstat(file, st) != 0) {
		fprintf(stderr, "Error in stat file/directory '%s'. %s.\n", file, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return st;
}
#endif

/**
 * Processes a directory.
 * Return != 0 if at least one file or link is processed.
 */
static int scan_dir(struct snapraid_scan* scan, struct snapraid_state* state, int output, struct snapraid_disk* disk, const char* dir, const char* sub)
{
	int processed = 0;
	DIR* d;
	tommy_list list;
	tommy_node* node;

	tommy_list_init(&list);

	d = opendir(dir);
	if (!d) {
		fprintf(stderr, "Error opening directory '%s'. %s.\n", dir, strerror(errno));
		fprintf(stderr, "You can exclude it in the config file with:\n\texclude /%s\n", sub);
		exit(EXIT_FAILURE);
	}

	/* read the full directory */
	while (1) {
		char path_next[PATH_MAX];
		char sub_next[PATH_MAX];
		struct dirent_sorted* entry;
		const char* name;
		struct dirent* dd;
		size_t name_len;

		/* clear errno to detect erroneous conditions */
		errno = 0;
		dd = readdir(d);
		if (dd == 0 && errno != 0) {
			fprintf(stderr, "Error reading directory '%s'. %s.\n", dir, strerror(errno));
			fprintf(stderr, "You can exclude it in the config file with:\n\texclude /%s\n", sub);
			exit(EXIT_FAILURE);
		}
		if (dd == 0) {
			break; /* finished */
		}

		/* skip "." and ".." files */
		name = dd->d_name;
		if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
			continue;

		pathprint(path_next, sizeof(path_next), "%s%s", dir, name);
		pathprint(sub_next, sizeof(sub_next), "%s%s", sub, name);

		/* check for not supported file names */
		if (name[0] == 0) {
			fprintf(stderr, "Unsupported name '%s' in file '%s'.\n", name, path_next);
			exit(EXIT_FAILURE);
		}

		/* exclude hidden files even before calling lstat() */
		if (filter_hidden(state->filter_hidden, dd) != 0) {
			if (state->opt.verbose) {
				printf("Excluding hidden '%s'\n", path_next);
			}
			continue;
		}

		/* exclude content files even before calling lstat() */
		if (filter_content(&state->contentlist, path_next) != 0) {
			if (state->opt.verbose) {
				printf("Excluding content '%s'\n", path_next);
			}
			continue;
		}

		name_len = strlen(dd->d_name);
		entry = malloc_nofail(sizeof(struct dirent_sorted) + name_len);

		/* copy the dir entry */
#if HAVE_STRUCT_DIRENT_D_INO
		entry->d_ino = dd->d_ino;
#endif
#if HAVE_STRUCT_DIRENT_D_TYPE
		entry->d_type = dd->d_type;
#endif
#if HAVE_STRUCT_DIRENT_D_STAT
		/* convert dirent to lstat result */
		dirent_lstat(dd, &entry->d_stat);
#endif
		memcpy(entry->d_name, dd->d_name, name_len + 1);

		/* insert in the list */
		tommy_list_insert_tail(&list, &entry->node, entry);
	}

	if (closedir(d) != 0) {
		fprintf(stderr, "Error closing directory '%s'. %s.\n", dir, strerror(errno));
		exit(EXIT_FAILURE);
	}

#ifndef _WIN32 /* In Windows doesn't sort. The inode is not significative */
	/* if inodes are persistent */
	if (!disk->has_not_persistent_inodes) {
		/* sort the list of dir entries by inodes */
		tommy_list_sort(&list, dd_ino_compare);
	}
#endif

	/* process the sorted dir entries */
	node = list;
	while (node!=0) {
		char path_next[PATH_MAX];
		char sub_next[PATH_MAX];
		struct dirent_sorted* dd = node->data;
		const char* name = dd->d_name;
		struct stat* st;
		int type;
#if !HAVE_STRUCT_DIRENT_D_STAT
		struct stat st_buf;
#endif

		pathprint(path_next, sizeof(path_next), "%s%s", dir, name);
		pathprint(sub_next, sizeof(sub_next), "%s%s", sub, name);

		/* start with an unknown type */
		type = -1;
		st = 0;

		/* if dirent has the type, use it */
#if HAVE_STRUCT_DIRENT_D_TYPE
		switch (dd->d_type) {
		case DT_UNKNOWN : break;
		case DT_REG : type = 0; break;
		case DT_LNK : type = 1; break;
		case DT_DIR : type = 2; break;
		default : type = 3; break;
		}
#endif

		/* if type is still unknown */
		if (type < 0) {
			/* get the type from stat */
			st = DSTAT(path_next, dd, &st_buf);

			if (S_ISREG(st->st_mode))
				type = 0;
			else if (S_ISLNK(st->st_mode))
				type = 1;
			else if (S_ISDIR(st->st_mode))
				type = 2;
			else
				type = 3;
		}

		if (type == 0) { /* REG */
			if (filter_path(&state->filterlist, disk->name, sub_next) == 0) {
				uint64_t physical;

				/* late stat */
				if (!st) st = DSTAT(path_next, dd, &st_buf);

#if HAVE_LSTAT_EX
				/* get inode info about the file, Windows needs an additional step */
				/* also for hardlink, the real size of the file is read here */
				if (lstat_ex(path_next, st) != 0) {
					fprintf(stderr, "Error in stat_inode file '%s'. %s.\n", path_next, strerror(errno));
					exit(EXIT_FAILURE);
				}
#endif
				if (state->opt.force_order == SORT_PHYSICAL) {
					if (filephy(path_next, st, &physical) != 0) {
						fprintf(stderr, "Error in getting the physical offset of file '%s'. %s.\n", path_next, strerror(errno));
						exit(EXIT_FAILURE);
					}
				} else {
					physical = 0;
				}

				scan_file(scan, state, output, disk, sub_next, st, physical);
				processed = 1;
			} else {
				if (state->opt.verbose) {
					printf("Excluding file '%s'\n", path_next);
				}
			}
		} else if (type == 1) { /* LNK */
			if (filter_path(&state->filterlist, disk->name, sub_next) == 0) {
				char subnew[PATH_MAX];
				int ret;

				ret = readlink(path_next, subnew, sizeof(subnew));
				if (ret >= PATH_MAX) {
					fprintf(stderr, "Error in readlink file '%s'. Symlink too long.\n", path_next);
					exit(EXIT_FAILURE);
				}
				if (ret < 0) {
					fprintf(stderr, "Error in readlink file '%s'. %s.\n", path_next, strerror(errno));
					exit(EXIT_FAILURE);
				}

				/* readlink doesn't put the final 0 */
				subnew[ret] = 0;

				/* process as a symbolic link */
				scan_link(scan, state, output, disk, sub_next, subnew, FILE_IS_SYMLINK);
				processed = 1;
			} else {
				if (state->opt.verbose) {
					printf("Excluding link '%s'\n", path_next);
				}
			}
		} else if (type == 2) { /* DIR */
			if (filter_dir(&state->filterlist, disk->name, sub_next) == 0) {
#ifndef _WIN32
				/* late stat */
				if (!st) st = DSTAT(path_next, dd, &st_buf);

				/* in Unix don't follow mount points in different devices */
				/* in Windows we are already skipping them reporting them as special files */
				if ((uint64_t)st->st_dev != disk->device) {
					fprintf(stderr, "WARNING! Ignoring mount point '%s' because it appears to be in a different device\n", path_next);
				} else
#endif
				{
					char sub_dir[PATH_MAX];

					/* recurse */
					pathslash(path_next, sizeof(path_next));
					pathcpy(sub_dir, sizeof(sub_dir), sub_next);
					pathslash(sub_dir, sizeof(sub_dir));
					if (scan_dir(scan, state, output, disk, path_next, sub_dir) == 0) {
						/* scan the directory as empty dir */
						scan_emptydir(scan, state, output, disk, sub_next);
					}
					/* or we processed something internally, or we have added the empty dir */
					processed = 1;
				}
			} else {
				if (state->opt.verbose) {
					printf("Excluding directory '%s'\n", path_next);
				}
			}
		} else {
			if (filter_path(&state->filterlist, disk->name, sub_next) == 0) {
				/* late stat */
				if (!st) st = DSTAT(path_next, dd, &st_buf);
			
				fprintf(stderr, "WARNING! Ignoring special '%s' file '%s'\n", stat_desc(st), path_next);
			} else {
				if (state->opt.verbose) {
					printf("Excluding special file '%s'\n", path_next);
				}
			}
		}

		/* next entry */
		node = node->next;

		/* free the present one */
		free(dd);
	}

	return processed;
}

void state_scan(struct snapraid_state* state, int output)
{
	tommy_node* i;
	tommy_node* j;
	tommy_list scanlist;

	tommy_list_init(&scanlist);

	for(i=state->disklist;i!=0;i=i->next) {
		struct snapraid_disk* disk = i->data;
		struct snapraid_scan* scan;
		tommy_node* node;
		int ret;
		int has_persistent_inode;
		unsigned phy_count;
		unsigned phy_dup;
		uint64_t phy_last;

		scan = malloc_nofail(sizeof(struct snapraid_scan));
		scan->count_equal = 0;
		scan->count_move = 0;
		scan->count_restore = 0;
		scan->count_change = 0;
		scan->count_remove = 0;
		scan->count_insert = 0;
		tommy_list_init(&scan->file_insert_list);
		tommy_list_init(&scan->link_insert_list);
		tommy_list_init(&scan->dir_insert_list);

		tommy_list_insert_tail(&scanlist, &scan->node, scan);

		printf("Scanning disk %s...\n", disk->name);

		/* check if the disk supports persistent inodes */
		ret = fsinfo(disk->dir, &has_persistent_inode);
		if (ret < 0) {
			fprintf(stderr, "Error accessing disk '%s' to get filesystem info. %s.\n", disk->dir, strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (!has_persistent_inode) {
			/* mask the disk */
			disk->has_not_persistent_inodes = 1;

			/* removes all the inodes from the inode collection */
			/* if they are not persistent, all of them could be changed now */
			/* and we don't want to find false matching ones */
			/* see scan_file() for more details */
			node = disk->filelist;
			while (node) {
				struct snapraid_file* file = node->data;

				node = node->next;

				/* remove from the inode set */
				tommy_hashdyn_remove_existing(&disk->inodeset, &file->nodeset);

				/* clear the inode */
				file->inode = 0;

				/* mark as missing inode */
				file_flag_set(file, FILE_IS_WITHOUT_INODE);
			}
		}

		scan_dir(scan, state, output, disk, disk->dir, "");

		/* check for removed files */
		node = disk->filelist;
		while (node) {
			struct snapraid_file* file = node->data;

			/* next node */
			node = node->next;

			/* remove if not present */
			if (!file_flag_has(file, FILE_IS_PRESENT)) {
				++scan->count_remove;

				if (state->opt.gui) {
					fprintf(stdlog, "scan:remove:%s:%s\n", disk->name, file->sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Remove '%s%s'\n", disk->dir, file->sub);
				}

				scan_file_remove(state, disk, file);
			}
		}

		/* check for removed links */
		node = disk->linklist;
		while (node) {
			struct snapraid_link* link = node->data;

			/* next node */
			node = node->next;

			/* remove if not present */
			if (!link_flag_has(link, FILE_IS_PRESENT)) {
				++scan->count_remove;

				if (state->opt.gui) {
					fprintf(stdlog, "scan:remove:%s:%s\n", disk->name, link->sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Remove '%s%s'\n", disk->dir, link->sub);
				}

				scan_link_remove(state, disk, link);
			}
		}

		/* check for removed dirs */
		node = disk->dirlist;
		while (node) {
			struct snapraid_dir* dir = node->data;

			/* next node */
			node = node->next;

			/* remove if not present */
			if (!dir_flag_has(dir, FILE_IS_PRESENT)) {
				++scan->count_remove;

				if (state->opt.gui) {
					fprintf(stdlog, "scan:remove:%s:%s\n", disk->name, dir->sub);
					fflush(stdlog);
				}
				if (output) {
					printf("Remove '%s%s'\n", disk->dir, dir->sub);
				}

				scan_emptydir_remove(state, disk, dir);
			}
		}

		/* sort the files before inserting them */
		/* we use a stable sort to ensure that if the reported physical offset/inode */
		/* are always 0, we keep at least the directory order */
		switch (state->opt.force_order) {
		case SORT_PHYSICAL :
			tommy_list_sort(&scan->file_insert_list, file_physical_compare);
			break;
		case SORT_INODE :
			tommy_list_sort(&scan->file_insert_list, file_inode_compare);
			break;
		case SORT_ALPHA :
			tommy_list_sort(&scan->file_insert_list, file_alpha_compare);
			break;
		case SORT_DIR :
			/* already in order */
			break;
		}

		/* insert all the new files, we insert them only after the deletion */
		/* to reuse the just freed space */
		node = scan->file_insert_list;
		phy_count = 0;
		phy_dup = 0;
		phy_last = -1;
		while (node) {
			struct snapraid_file* file = node->data;

			/* if the file is not empty, count duplicate physical offsets */
			if (state->opt.force_order == SORT_PHYSICAL && file->size != 0) {
				if (phy_count > 0 && file->physical == phy_last && phy_last != FILEPHY_WITHOUT_OFFSET)
					++phy_dup;
				phy_last = file->physical;
				++phy_count;
			}

			/* next node */
			node = node->next;

			/* insert it */
			scan_file_insert(state, disk, file);
		}

		/* mark the disk without reliable physical offset if it has duplicates */
		/* here it should never happen because we already sorted out hardlinks */
		if (state->opt.force_order == SORT_PHYSICAL && phy_dup > 0) {
			disk->has_not_reliable_physical = 1;
		}

		/* insert all the new links */
		node = scan->link_insert_list;
		while (node) {
			struct snapraid_link* link = node->data;

			/* next node */
			node = node->next;

			/* insert it */
			scan_link_insert(state, disk, link);
		}

		/* insert all the new dirs */
		node = scan->dir_insert_list;
		while (node) {
			struct snapraid_dir* dir = node->data;

			/* next node */
			node = node->next;

			/* insert it */
			scan_emptydir_insert(state, disk, dir);
		}
	}

	/* checks for disks where all the previously existing files where removed */
	if (!state->opt.force_empty) {
		int done = 0;
		for(i=state->disklist,j=scanlist;i!=0;i=i->next,j=j->next) {
			struct snapraid_disk* disk = i->data;
			struct snapraid_scan* scan = j->data;

			if (scan->count_equal == 0 && scan->count_move == 0 && scan->count_restore == 0 && (scan->count_remove != 0 || scan->count_change != 0)) {
				if (!done) {
					done = 1;
					fprintf(stderr, "All the files previously present in disk '%s' at dir '%s'", disk->name, disk->dir);
				} else {
					fprintf(stderr, ", disk '%s' at dir '%s'", disk->name, disk->dir);
				}
			}
		}
		if (done) {
			fprintf(stderr, " are now missing or rewritten!\n");
			fprintf(stderr, "This could happen when deleting all the files from a disk,\n");
			fprintf(stderr, "and restoring them with a program not setting correctly the timestamps.\n");
			fprintf(stderr, "If this is really what you are doing, you can '%s' anyway, \n", state->command);
			fprintf(stderr, "using 'snapraid --force-empty %s'.\n", state->command);
			fprintf(stderr, "Instead, it's possible that you have some disks not mounted.\n");
			exit(EXIT_FAILURE);
		}
	}

	/* checks for disks without the physical offset support */
	if (state->opt.force_order == SORT_PHYSICAL) {
		int done = 0;
		for(i=state->disklist;i!=0;i=i->next) {
			struct snapraid_disk* disk = i->data;

			if (disk->has_not_reliable_physical) {
				if (!done) {
					done = 1;
					fprintf(stderr, "WARNING! Physical offsets not supported for disk '%s'", disk->name);
				} else {
					fprintf(stderr, ", '%s", disk->name);
				}
			}
		}
		if (done) {
			fprintf(stderr, ". Performance won't be optimal.\n");
		}
	}

	/* Check for disks without persisten inodes */
	{
		int done = 0;
		for(i=state->disklist;i!=0;i=i->next) {
			struct snapraid_disk* disk = i->data;

			if (disk->has_not_persistent_inodes) {
				if (!done) {
					done = 1;
					fprintf(stderr, "WARNING! Inodes are not persistent for disk '%s'", disk->name);
				} else {
					fprintf(stderr, ", '%s", disk->name);
				}
			}
		}
		if (done) {
			fprintf(stderr, ". Move operations won't be optimized.\n");
		}
	}

	if (state->opt.verbose || output) {
		struct snapraid_scan total;
		int no_difference;

		total.count_equal = 0;
		total.count_move = 0;
		total.count_restore = 0;
		total.count_change = 0;
		total.count_remove = 0;
		total.count_insert = 0;

		for(i=scanlist;i!=0;i=i->next) {
			struct snapraid_scan* scan = i->data;
			total.count_equal += scan->count_equal;
			total.count_move += scan->count_move;
			total.count_restore += scan->count_restore;
			total.count_change += scan->count_change;
			total.count_remove += scan->count_remove;
			total.count_insert += scan->count_insert;
		}

		if (state->opt.verbose) {
			printf("\tequal %d\n", total.count_equal);
			printf("\tmoved %d\n", total.count_move);
			printf("\trestored %d\n", total.count_restore);
			printf("\tchanged %d\n", total.count_change);
			printf("\tremoved %d\n", total.count_remove);
			printf("\tadded %d\n", total.count_insert);
		}

		if (state->opt.gui) {
			fprintf(stdlog, "summary:equal:%u\n", total.count_equal);
			fprintf(stdlog, "summary:moved:%u\n", total.count_move);
			fprintf(stdlog, "summary:restored:%u\n", total.count_restore);
			fprintf(stdlog, "summary:changed:%u\n", total.count_change);
			fprintf(stdlog, "summary:removed:%u\n", total.count_remove);
			fprintf(stdlog, "summary:added:%u\n", total.count_insert);
			fflush(stdlog);
		}

		no_difference = !total.count_move && !total.count_restore && !total.count_change && !total.count_remove && !total.count_insert;

		if (output) {
			if (no_difference) {
				printf("No difference\n");
			} else {
				printf("There are differences\n");
			}
		}

		if (state->opt.gui) {
			if (no_difference) {
				fprintf(stdlog, "summary:exit:equal\n");
				fflush(stdlog);
			} else {
				fprintf(stdlog, "summary:exit:diff\n");
				fflush(stdlog);
			}
		}
	}

	tommy_list_foreach(&scanlist, (tommy_foreach_func*)free);
}

