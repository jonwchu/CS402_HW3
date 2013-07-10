/*
 *   FILE: s5fs_subr.c
 * AUTHOR: afenn
 *  DESCR:
 *  $Id: s5fs_subr.c,v 1.1.2.1 2006/06/04 01:02:15 afenn Exp $
 */

#include "kernel.h"
#include "util/debug.h"
#include "mm/kmalloc.h"
#include "globals.h"
#include "proc/sched.h"
#include "proc/kmutex.h"
#include "errno.h"
#include "util/string.h"
#include "util/printf.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "fs/stat.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/s5fs/s5fs_subr.h"
#include "fs/s5fs/s5fs.h"
#include "mm/mm.h"
#include "mm/page.h"

#define dprintf(...) dbg(DBG_S5FS, __VA_ARGS__)

#define s5_dirty_super(fs)                                           \
        do {                                                         \
                pframe_t *p;                                         \
                int err;                                             \
                pframe_get(S5FS_TO_VMOBJ(fs), S5_SUPER_BLOCK, &p);   \
                KASSERT(p);                                          \
                err = pframe_dirty(p);                               \
                KASSERT(!err                                         \
                        && "shouldn\'t fail for a page belonging "   \
                        "to a block device");                        \
        } while (0)


static void s5_free_block(s5fs_t *fs, int block);
static int s5_alloc_block(s5fs_t *);


/*
 * Return the disk-block number for the given seek pointer (aka file
 * position).
 *
 * If the seek pointer refers to a sparse block, and alloc is false,
 * then return 0. If the seek pointer refers to a sparse block, and
 * alloc is true, then allocate a new disk block (and make the inode
 * point to it) and return it.
 *
 * Be sure to handle indirect blocks!
 *
 * If there is an error, return -errno.
 *
 * You probably want to use pframe_get, pframe_pin, pframe_unpin, pframe_dirty.
 */
int
s5_seek_to_block(vnode_t *vnode, off_t seekptr, int alloc)
{
    /* CASE     BLOCK TYPE      ALLOC       Direct Sparse       Indirect Sparse         WAT DO?
    *    1          (BLOCK > Total Blocks)                                              Return Error
    *    2      DIRECT          FALSE       TRUE                N/A                     return block from s5_direct_blocks
    *    3      DIRECT          TRUE        TRUE                N/A                     allocate new block and point inode (also memcpy)
    *    4      DIRECT          TRUE        FALSE               N/A                     return block from s5_direct_blocks
    *    5      INDIRECT        FALSE       TRUE                TRUE                    return 0
    *    7      INDIRECT        FALSE       FALSE               FALSE                   Find block we want
    *    8      INDIRECT        TRUE        FALSE               FALSE                   Find block we want
    *    9      INDIRECT        TRUE        TRUE                FALSE                   allocate new block, memcpy to 0, set data address in indirect
    
    *    *      INDIRECT        TRUE        N/A                 TRUE                    allocate new block, pframe_get on inode->indirect_block
    */
    dbg_print("s5_seek_to_block: Entering Function, seekptr: %i, alloc: %i\n",seekptr,alloc);
    s5fs_t* vnode_s5fs = VNODE_TO_S5FS(vnode);
    s5_inode_t* vnode_inode = VNODE_TO_S5INODE(vnode);
    struct mmobj* vnode_vmobj = S5FS_TO_VMOBJ(vnode_s5fs);
    uint32_t data_block = S5_DATA_BLOCK(seekptr);
    
    pframe_t* pf;
    dbg_print("s5_seek_to_block: a\n");
    if(data_block > S5_MAX_FILE_BLOCKS)
    {
        /* Case 1 */
        dbg_print("s5_seek_to_block: Case 1\n");
        return 0;
    }

    if(data_block < S5_NDIRECT_BLOCKS)
    {
        dbg_print("s5_seek_to_block: b\n");
        /* Direct Block */
        if(!alloc)
        {
            /* ALLOC FALSE */
            /* CASE 2 */
            dbg_print("s5_seek_to_block: c\n");
            dbg_print("s5_seek_to_block: Case 2\n");
            return vnode_inode->s5_direct_blocks[data_block];
        }
        else
        {
            /* ALLOC TRUE */
            dbg_print("s5_seek_to_block: d\n");
            if(vnode_inode->s5_direct_blocks[data_block] == 0)
            {
                /* Sparse Block */
                /* CASE 3 */
                dbg_print("s5_seek_to_block: e\n");
                pframe_get(vnode_vmobj,data_block,&pf);
                pframe_pin(pf);

                int block_alloc = s5_alloc_block(vnode_s5fs);
                dbg_print("s5_seek_to_block: f\n");
                if(block_alloc == -ENOSPC)
                {
                    /* Allocation Failure */
                    dbg_print("s5_seek_to_block: g\n");
                    pframe_unpin(pf);
                    dbg_print("s5_seek_to_block: Allocation Failure #1\n");
                    return -ENOSPC;
                }
                else
                {
                    /* Success in Allocation, Connect Inode and Dirty */
                    dbg_print("s5_seek_to_block: h\n");
                    vnode_inode->s5_direct_blocks[data_block] = block_alloc;
                    /* memset(pf->pf_addr, 0, PAGE_SIZE); */
                    pframe_dirty(pf);
                    s5_dirty_inode(vnode_s5fs,vnode_inode);
                    pframe_unpin(pf);
                    dbg_print("s5_seek_to_block: Case 3\n");
                    return block_alloc;
                }
            }
            else
            {
                /* Not Sparse Block */

                /* CASE 4 */
                dbg_print("s5_seek_to_block: Case 4\n");
                return vnode_inode->s5_direct_blocks[data_block];
            }
        }
    }
    else
    {
        /* Indirect Block */
        dbg_print("s5_seek_to_block: i\n");
        if(!alloc)
        {
            /* ALLOC FALSE */
            dbg_print("s5_seek_to_block: j\n");
            if(vnode_inode->s5_indirect_block == 0)
            {
                /* Sparse Block */
                /* CASE 5 */
                dbg_print("s5_seek_to_block: Case 5\n");
                return 0;
            }
            else
            {
                /* Not Sparse Block */
                /* CASE 7 */
                dbg_print("s5_seek_to_block: Case 7\n");
                return vnode_inode->s5_direct_blocks[data_block - S5_NDIRECT_BLOCKS];
            }
        }
        else
        {
            /* ALLOC TRUE */
            dbg_print("s5_seek_to_block: k\n");
            if(vnode_inode->s5_indirect_block == 0)
            {
                /* Sparse Block */
                /* CASE 5 */
                dbg_print("s5_seek_to_block: l\n");
                int indirect_alloc = s5_alloc_block(vnode_s5fs);
            
                if(indirect_alloc == -ENOSPC)
                {
                    /* Allocation Failure */
                    dbg_print("s5_seek_to_block: Allocation Failure #2\n");
                    return -ENOSPC;
                }

                /* Success in Allocation, Connect Inode and Dirty */  
                dbg_print("s5_seek_to_block: m\n");       
                pframe_get(vnode_vmobj,vnode_inode->s5_indirect_block,&pf);
                pframe_pin(pf);

                /* memset(pf->pf_addr, 0, PAGE_SIZE); */
                vnode_inode->s5_indirect_block = indirect_alloc;

                pframe_dirty(pf);
                s5_dirty_inode(vnode_s5fs,vnode_inode);
                dbg_print("s5_seek_to_block: n\n");
            }
            else
            {
                /* Not Sparse Block */
                dbg_print("s5_seek_to_block: o\n");
                pframe_get(vnode_vmobj,vnode_inode->s5_indirect_block,&pf);
                pframe_pin(pf);
            }

            dbg_print("s5_seek_to_block: p\n");
            uint32_t indirect_map = data_block - S5_NDIRECT_BLOCKS;
            uint32_t* block_array = (uint32_t*)pf->pf_addr;
            int direct_index = block_array[indirect_map];
            if(direct_index == 0)
            {
                dbg_print("s5_seek_to_block: q\n");
                direct_index = s5_alloc_block(vnode_s5fs);
                if(direct_index == -ENOSPC)
                {
                    /* Allocation Failure */
                    dbg_print("s5_seek_to_block: Allocation Failure #3\n");
                    return -ENOSPC;
                }

            }
            dbg_print("s5_seek_to_block: rn");
            block_array[indirect_map] = direct_index;
            pframe_dirty(pf);
            pframe_unpin(pf);

            dbg_print("s5_seek_to_block: Case 6\n");
            return direct_index;
        }
    }

        /* NOT_YET_IMPLEMENTED("S5FS: s5_seek_to_block");
        * return -1;
        */
}


/*
 * Locks the mutex for the whole file system
 */
static void
lock_s5(s5fs_t *fs)
{
        kmutex_lock(&fs->s5f_mutex);
}

/*
 * Unlocks the mutex for the whole file system
 */
static void
unlock_s5(s5fs_t *fs)
{
        kmutex_unlock(&fs->s5f_mutex);
}


/*
 * Write len bytes to the given inode, starting at seek bytes from the
 * beginning of the inode. On success, return the number of bytes
 * actually written (which should be 'len', unless there's only enough
 * room for a partial write); on failure, return -errno.
 *
 * This function should allow writing to files or directories, treating
 * them identically.
 *
 * Writing to a sparse block of the file should cause that block to be
 * allocated.  Writing past the end of the file should increase the size
 * of the file. Blocks between the end and where you start writing will
 * be sparse.
 *
 * Do not call s5_seek_to_block() directly from this function.  You will
 * use the vnode's pframe functions, which will eventually result in a
 * call to s5_seek_to_block().
 *
 * You will need pframe_dirty(), pframe_get(), memcpy().
 */
int
s5_write_file(vnode_t *vnode, off_t seek, const char *bytes, size_t len)
{
    dbg_print("s5_write_file: Writing to File, Length: %i\n",len);

    uint32_t to_write = len;

    /* Block Number */
    uint32_t block_index = S5_DATA_BLOCK(seek);
    if(block_index >= S5_MAX_FILE_BLOCKS)
    {
        dbg_print("s5_write_file: Exiting with Value: 0\n");
        return 0;
    }

    /* Offset within block */
    uint32_t block_offset = S5_DATA_OFFSET(seek);
    uint32_t remaining = S5_BLOCK_SIZE - block_offset;
    int total_written = 0;

    s5_inode_t* inode = VNODE_TO_S5INODE(vnode);
    s5fs_t* dir_fs = VNODE_TO_S5FS(vnode);

    if(seek >= vnode->vn_len)
    {
        /* End to Start of Writing should be written as sparse */

    }

    while(to_write > 0)
    {
        pframe_t* pf;
        pframe_get(&(vnode->vn_mmobj),block_index,&pf);
        pframe_pin(pf);

        if(to_write <= remaining)
        {
            memcpy((char*)pf->pf_addr + block_offset,bytes + total_written,to_write);
            total_written += to_write;
            block_offset = 0;
            to_write = 0;
        }
        else
        {
            /* to_write > remaining */
            memcpy((char*)pf->pf_addr + block_offset,bytes + total_written,remaining);
            total_written += remaining;
            block_offset = 0;
            to_write -= remaining;

            block_index++;
            remaining = S5_BLOCK_SIZE;
            if(block_index == S5_MAX_FILE_BLOCKS)
            {
                break;
            }
        }
        pframe_dirty(pf);
        pframe_unpin(pf);
    }

    if(seek + total_written > vnode->vn_len)
    {
        vnode->vn_len = seek + total_written;
        inode->s5_size = seek + total_written;
    }
    s5_dirty_inode(dir_fs,inode);
    return total_written;
}

/*
 * Read up to len bytes from the given inode, starting at seek bytes
 * from the beginning of the inode. On success, return the number of
 * bytes actually read, or 0 if the end of the file has been reached; on
 * failure, return -errno.
 *
 * This function should allow reading from files or directories,
 * treating them identically.
 *
 * Reading from a sparse block of the file should act like reading
 * zeros; it should not cause the sparse blocks to be allocated.
 *
 * Similarly as in s5_write_file(), do not call s5_seek_to_block()
 * directly from this function.
 *
 * If the region to be read would extend past the end of the file, less
 * data will be read than was requested.
 *
 * You probably want to use pframe_get(), memcpy().
 */
int
s5_read_file(struct vnode *vnode, off_t seek, char *dest, size_t len)
{
    dbg_print("s5_read_file: Reading File, Length: %i\n",len);
    if(seek >= vnode->vn_len)
    {
        dbg_print("s5_read_file: Exiting with Value: 0\n");
        return 0;
    }

    int block_index = S5_DATA_BLOCK(seek);
    int block_offset = S5_DATA_OFFSET(seek);
    int length;
    
    if((int)len < vnode->vn_len - seek)
    {
        length = len;
    }
    else
    {
        length = vnode->vn_len - seek;
    }
    
    pframe_t *pf; 
    if(block_index < S5_NDIRECT_BLOCKS)
    {
        /*  Direct Block */    
        dbg_print("s5_read_file: Reading from Direct Block\n");
        pframe_get(&vnode->vn_mmobj, block_index, &pf);
        pframe_pin(pf);
        memcpy(dest,(char*)pf->pf_addr + block_offset, length);
    }
    else
    {
        s5_inode_t* inode = VNODE_TO_S5INODE(vnode);
        if(inode->s5_indirect_block != 0)
        {
            dbg_print("s5_read_file: Reading from INDIRECT Block\n");
            
            pframe_get(&vnode->vn_mmobj, inode->s5_indirect_block, &pf);
            pframe_pin(pf);

            memcpy(dest,(char*)pf->pf_addr + (block_index - S5_NDIRECT_BLOCKS), length);
        }
        else
        {
            return 0;
        }
    }

    pframe_unpin(pf);
    return length;
}

/*
 * Allocate a new disk-block off the block free list and return it. If
 * there are no free blocks, return -ENOSPC.
 *
 * This will not initialize the contents of an allocated block; these
 * contents are undefined.
 *
 * If the super block's s5s_nfree is 0, you need to refill 
 * s5s_free_blocks and reset s5s_nfree.  You need to read the contents 
 * of this page using the pframe system in order to obtain the next set of
 * free block numbers.
 *
 * Don't forget to dirty the appropriate blocks!
 *
 * You'll probably want to use lock_s5(), unlock_s5(), pframe_get(),
 * and s5_dirty_super()
 */
static int
s5_alloc_block(s5fs_t *fs)
{
    dbg_print("s5_alloc_block: Entering Function\n");
    /* get the super block frame */
    s5_super_t* superblock = fs->s5f_super;
    int block_return;
    lock_s5(fs);

    if(((int)superblock->s5s_free_blocks[S5_NBLKS_PER_FNODE - 1] == -1) 
        && (superblock->s5s_nfree == 0))
    {
        dbg_print("s5_alloc_block: No free blocks, returning error");
        unlock_s5(fs);
        return -ENOSPC;
    }
    
    if(superblock->s5s_nfree == 0)
    {
        dbg_print("s5_alloc_block: Out of free blocks, getting more!\n");
        struct mmobj* fs_vmobj = S5FS_TO_VMOBJ(fs);
    
        int next_block = superblock->s5s_free_blocks[S5_NBLKS_PER_FNODE - 1];

        pframe_t* pf;
        pframe_get(fs_vmobj,next_block,&pf);
        pframe_pin(pf);

        memcpy(((char*)superblock->s5s_free_blocks),pf->pf_addr,S5_NBLKS_PER_FNODE * sizeof(uint32_t));
        superblock->s5s_nfree = S5_NBLKS_PER_FNODE - 1;

        block_return = next_block;

        pframe_unpin(pf);
    }
    else
    {
        /* Frre blocks are available */
        dbg_print("s5_alloc_block: Free block available\n");
        superblock->s5s_nfree--;
        block_return = superblock->s5s_free_blocks[superblock->s5s_nfree];
    }

    s5_dirty_super(fs);
    unlock_s5(fs);
    dbg_print("s5_alloc_block: Exiting Function\n");
    return block_return;

        /* NOT_YET_IMPLEMENTED("S5FS: s5_alloc_block");
        * return -1;
        */
}


/*
 * Given a filesystem and a block number, frees the given block in the
 * filesystem.
 *
 * This function may potentially block.
 *
 * The caller is responsible for ensuring that the block being placed on
 * the free list is actually free and is not resident.
 */
static void
s5_free_block(s5fs_t *fs, int blockno)
{
        s5_super_t *s = fs->s5f_super;


        lock_s5(fs);

        KASSERT(S5_NBLKS_PER_FNODE > s->s5s_nfree);

        if ((S5_NBLKS_PER_FNODE - 1) == s->s5s_nfree) {
                /* get the pframe where we will store the free block nums */
                pframe_t *prev_free_blocks = NULL;
                KASSERT(fs->s5f_bdev);
                pframe_get(&fs->s5f_bdev->bd_mmobj, blockno, &prev_free_blocks);
                KASSERT(prev_free_blocks->pf_addr);

                /* copy from the superblock to the new block on disk */
                memcpy(prev_free_blocks->pf_addr, (void *)(s->s5s_free_blocks),
                       S5_NBLKS_PER_FNODE * sizeof(int));
                pframe_dirty(prev_free_blocks);

                /* reset s->s5s_nfree and s->s5s_free_blocks */
                s->s5s_nfree = 0;
                s->s5s_free_blocks[S5_NBLKS_PER_FNODE - 1] = blockno;
        } else {
                s->s5s_free_blocks[s->s5s_nfree++] = blockno;
        }

        s5_dirty_super(fs);

        unlock_s5(fs);
}

/*
 * Creates a new inode from the free list and initializes its fields.
 * Uses S5_INODE_BLOCK to get the page from which to create the inode
 *
 * This function may block.
 */
int
s5_alloc_inode(fs_t *fs, uint16_t type, devid_t devid)
{
        s5fs_t *s5fs = FS_TO_S5FS(fs);
        pframe_t *inodep;
        s5_inode_t *inode;
        int ret = -1;

        KASSERT((S5_TYPE_DATA == type)
                || (S5_TYPE_DIR == type)
                || (S5_TYPE_CHR == type)
                || (S5_TYPE_BLK == type));


        lock_s5(s5fs);

        if (s5fs->s5f_super->s5s_free_inode == (uint32_t) -1) {
                unlock_s5(s5fs);
                return -ENOSPC;
        }

        pframe_get(&s5fs->s5f_bdev->bd_mmobj,
                   S5_INODE_BLOCK(s5fs->s5f_super->s5s_free_inode),
                   &inodep);
        KASSERT(inodep);

        inode = (s5_inode_t *)(inodep->pf_addr)
                + S5_INODE_OFFSET(s5fs->s5f_super->s5s_free_inode);

        KASSERT(inode->s5_number == s5fs->s5f_super->s5s_free_inode);

        ret = inode->s5_number;

        /* reset s5s_free_inode; remove the inode from the inode free list: */
        s5fs->s5f_super->s5s_free_inode = inode->s5_next_free;
        pframe_pin(inodep);
        s5_dirty_super(s5fs);
        pframe_unpin(inodep);


        /* init the newly-allocated inode: */
        inode->s5_size = 0;
        inode->s5_type = type;
        inode->s5_linkcount = 0;
        memset(inode->s5_direct_blocks, 0, S5_NDIRECT_BLOCKS * sizeof(int));
        if ((S5_TYPE_CHR == type) || (S5_TYPE_BLK == type))
                inode->s5_indirect_block = devid;
        else
                inode->s5_indirect_block = 0;

        s5_dirty_inode(s5fs, inode);

        unlock_s5(s5fs);

        return ret;
}


/*
 * Free an inode by freeing its disk blocks and putting it back on the
 * inode free list.
 *
 * You should also reset the inode to an unused state (eg. zero-ing its
 * list of blocks and setting its type to S5_FREE_TYPE).
 *
 * Don't forget to free the indirect block if it exists.
 *
 * You probably want to use s5_free_block().
 */
void
s5_free_inode(vnode_t *vnode)
{
        uint32_t i;
        s5_inode_t *inode = VNODE_TO_S5INODE(vnode);
        s5fs_t *fs = VNODE_TO_S5FS(vnode);

        KASSERT((S5_TYPE_DATA == inode->s5_type)
                || (S5_TYPE_DIR == inode->s5_type)
                || (S5_TYPE_CHR == inode->s5_type)
                || (S5_TYPE_BLK == inode->s5_type));

        /* free any direct blocks */
        for (i = 0; i < S5_NDIRECT_BLOCKS; ++i) {
                if (inode->s5_direct_blocks[i]) {
                        dprintf("freeing block %d\n", inode->s5_direct_blocks[i]);
                        s5_free_block(fs, inode->s5_direct_blocks[i]);

                        s5_dirty_inode(fs, inode);
                        inode->s5_direct_blocks[i] = 0;
                }
        }

        if (((S5_TYPE_DATA == inode->s5_type)
             || (S5_TYPE_DIR == inode->s5_type))
            && inode->s5_indirect_block) {
                pframe_t *ibp;
                uint32_t *b;

                pframe_get(S5FS_TO_VMOBJ(fs),
                           (unsigned)inode->s5_indirect_block,
                           &ibp);
                KASSERT(ibp
                        && "because never fails for block_device "
                        "vm_objects");
                pframe_pin(ibp);

                b = (uint32_t *)(ibp->pf_addr);
                for (i = 0; i < S5_NIDIRECT_BLOCKS; ++i) {
                        KASSERT(b[i] != inode->s5_indirect_block);
                        if (b[i])
                                s5_free_block(fs, b[i]);
                }

                pframe_unpin(ibp);

                s5_free_block(fs, inode->s5_indirect_block);
        }

        inode->s5_indirect_block = 0;
        inode->s5_type = S5_TYPE_FREE;
        s5_dirty_inode(fs, inode);

        lock_s5(fs);
        inode->s5_next_free = fs->s5f_super->s5s_free_inode;
        fs->s5f_super->s5s_free_inode = inode->s5_number;
        unlock_s5(fs);

        s5_dirty_inode(fs, inode);
        s5_dirty_super(fs);
}

/*
 * Locate the directory entry in the given inode with the given name,
 * and return its inode number. If there is no entry with the given
 * name, return -ENOENT.
 *
 * You'll probably want to use s5_read_file and name_match
 *
 * You can either read one dirent at a time or optimize and read more.
 * Either is fine.
 */
    

int
s5_find_dirent(vnode_t *vnode, const char *name, size_t namelen)
{
    dbg_print("s5_find_dirent: Searching for Dir of Name: %s, Length: %i\n", name,namelen);
    s5_dirent_t traverse_dir;
    uint32_t offset = 0;

    int read = s5_read_file(vnode, offset, (char *) &traverse_dir, sizeof(s5_dirent_t));

    /* Loop through the inode blocks until the dirent is found. */
    while (read > 0)
    {
        dbg_print("s5_find_dirent: Temp Name: %s\n",traverse_dir.s5d_name);
        /* Once found, return. */
        if (name_match(traverse_dir.s5d_name, name, namelen))
        {
            dbg_print("s5_find_dirent: Found Directory!\n");
            return traverse_dir.s5d_inode;
        }
        offset += sizeof(s5_dirent_t);

        read = s5_read_file(vnode, offset, (char *) &traverse_dir, sizeof(s5_dirent_t));
    }

    /* If the function has not returned by this point, then dirent with parameter name DNE. */
    dbg_print("s5_find_dirent: Directory Not Found :(\n");
    return -ENOENT;
}



/*
 * Locate the directory entry in the given inode with the given name,
 * and delete it. If there is no entry with the given name, return
 * -ENOENT.
 *
 * In order to ensure that the directory entries are contiguous in the
 * directory file, you will need to move the last directory entry into
 * the remove dirent's place.
 *
 * When this function returns, the inode refcount on the removed file
 * should be decremented.
 *
 * It would be a nice extension to free blocks from the end of the
 * directory file which are no longer needed.
 *
 * Don't forget to dirty appropriate blocks!
 *
 * You probably want to use vget(), vput(), s5_read_file(),
 * s5_write_file(), and s5_dirty_inode().
 */
int
s5_remove_dirent(vnode_t *vnode, const char *name, size_t namelen)
{
    dbg_print("s5_remove_dirent: Removing Dirent");
    int inode = s5_find_dirent(vnode,name,namelen);
    if(inode == -ENOENT)
    {
        dbg_print("s5_remove_dirent: ERROR, Directory not found");
        return -ENOENT;
    }
    else
    {
        vnode_t* vnode_to_remove = vget(vnode->vn_fs,inode);
        s5_inode_t* inode_to_remove = VNODE_TO_S5INODE(vnode_to_remove);
        inode_to_remove->s5_linkcount--;
        vput(vnode_to_remove);

        /* Need to get the offset of where the dirent is */
        int total_count = 0;
        int dir_index = 0;

        /* Loop through the inode blocks until the dirent is found. */
        s5_dirent_t traverse_dir;
        while (s5_read_file(vnode, dir_index * sizeof(s5_dirent_t), (char *) &traverse_dir, sizeof(s5_dirent_t)) > 0)
        {
            /* Once found, return. */
            if (name_match(traverse_dir.s5d_name, name, namelen))
            {
                dir_index = total_count;
            }
            total_count++;
        }
        total_count--;

        if(dir_index != total_count)
        {
            /* swap dir with last */
            char* last_buffer = kmalloc(sizeof(s5_dirent_t));
            s5_dirent_t* dirent = (s5_dirent_t*)last_buffer;
            int read = s5_read_file(vnode,vnode->vn_len - sizeof(s5_dirent_t),last_buffer,sizeof(s5_dirent_t));
            KASSERT(read == sizeof(s5_dirent_t));

            int write = s5_write_file(vnode, dir_index * sizeof(s5_dirent_t), last_buffer, sizeof(s5_dirent_t));
            KASSERT(write == sizeof(s5_dirent_t));
        }

        s5_inode_t* inode = VNODE_TO_S5INODE(vnode);

        vnode->vn_len -= sizeof(s5_dirent_t);
        inode->s5_size -= sizeof(s5_dirent_t);

        s5_dirty_inode(VNODE_TO_S5FS(vnode),inode);

        return 0;
    }
        /* NOT_YET_IMPLEMENTED("S5FS: s5_remove_dirent");
        * return -1;
        */
}

/*
 * Create a new directory entry in directory 'parent' with the given name, which
 * refers to the same file as 'child'.
 *
 * When this function returns, the inode refcount on the file that was linked to
 * should be incremented.
 *
 * Remember to incrament the ref counts appropriately
 *
 * You probably want to use s5_find_dirent(), s5_write_file(), and s5_dirty_inode().
 */
int
s5_link(vnode_t *parent, vnode_t *child, const char *name, size_t namelen)
{
        /* NOT_YET_IMPLEMENTED("S5FS: s5_link");
        * return -1;
        */
        dbg_print("s5_link: Linking!\n");
        s5_inode_t* child_inode = VNODE_TO_S5INODE(child);

        if(s5_find_dirent(parent,name,namelen) >= 0)
        {
            /* Directory already exists */
            dbg_print("s5_link: Directory Already Exists, Name: %s\n",name);
            return -1;
        }
        else
        {
            s5_dirent_t* new_dir = kmalloc(sizeof(s5_dirent_t));
            /* memset(new_dir, 0, sizeof(s5_dirent_t)); */
            new_dir->s5d_inode = child_inode->s5_number;
            /* Copy Over name for Directory */
            memcpy(new_dir->s5d_name,name,namelen);
            new_dir->s5d_name[namelen] = '\0';

            int write_dir = s5_write_file(parent,parent->vn_len,(char*)new_dir,sizeof(s5_dirent_t));
            KASSERT(write_dir == sizeof(s5_dirent_t));

            /* Increment indoe refcount */
            dbg_print("s5_link: Incrementing Linkcount for Inode: %s\n",name);
            child_inode->s5_linkcount++;
            dbg_print("s5_link: Linkcount for Inode: %s is: %i\n",name,child_inode->s5_linkcount);
            s5_dirty_inode(VNODE_TO_S5FS(parent),child_inode);
            kfree(new_dir);
            return 0;
        }
}

/*
 * Return the number of blocks that this inode has allocated on disk.
 * This should include the indirect block, but not include sparse
 * blocks.
 *
 * This is only used by s5fs_stat().
 *
 * You'll probably want to use pframe_get().
 */
int
s5_inode_blocks(vnode_t *vnode)
{
        /* NOT_YET_IMPLEMENTED("S5FS: s5_inode_blocks");
        * return -1;
        */
    s5_inode_t* inode = VNODE_TO_S5INODE(vnode);
    int max_blocks = S5_DATA_BLOCK(vnode->vn_len);
    int count = 0;

    int block_index = 0;

    while(block_index < max_blocks)
    {
        if(block_index < S5_NDIRECT_BLOCKS)
        {
            if(inode->s5_direct_blocks[block_index] != 0)
            {
                /* Not a sparse block */
                count++;
            }   
        }
        else
        {
            /* Indirect block */
            if(inode->s5_indirect_block == 0)
            {
                /* No indirect blocks */
                /* Do Nothing */
            }
            else
            {
                pframe_t* pf;
                pframe_get(S5FS_TO_VMOBJ(VNODE_TO_S5FS(vnode)),inode->s5_indirect_block,&pf);
                pframe_pin(pf);

                uint32_t indirect_map = block_index - S5_NDIRECT_BLOCKS;
                uint32_t* block_array = (uint32_t*)pf->pf_addr;
                if(block_array[indirect_map] != 0)
                {
                    count++;
                }

                pframe_unpin(pf);
            }
        }
        block_index++;
    }
    return count;
}

