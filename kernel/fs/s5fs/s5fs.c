/*
 *   FILE: s5fs.c
 * AUTHOR: afenn
 *  DESCR: S5FS entry points
 */

#include "kernel.h"
#include "types.h"
#include "globals.h"
#include "errno.h"

#include "util/string.h"
#include "util/printf.h"
#include "util/debug.h"

#include "proc/kmutex.h"

#include "fs/s5fs/s5fs_subr.h"
#include "fs/s5fs/s5fs.h"
#include "fs/dirent.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/file.h"
#include "fs/stat.h"

#include "drivers/dev.h"
#include "drivers/blockdev.h"

#include "mm/kmalloc.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "mm/mm.h"
#include "mm/mman.h"

#include "vm/vmmap.h"
#include "vm/shadow.h"

/* Diagnostic/Utility: */
static int s5_check_super(s5_super_t *super);
static int s5fs_check_refcounts(fs_t *fs);

/* fs_t entry points: */
static void s5fs_read_vnode(vnode_t *vnode);
static void s5fs_delete_vnode(vnode_t *vnode);
static int  s5fs_query_vnode(vnode_t *vnode);
static int  s5fs_umount(fs_t *fs);
static int  s5fs_free_blocks(fs_t *fs);

/* vnode_t entry points: */
static int  s5fs_read(vnode_t *vnode, off_t offset, void *buf, size_t len);
static int  s5fs_write(vnode_t *vnode, off_t offset, const void *buf, size_t len);
static int  s5fs_mmap(vnode_t *file, vmarea_t *vma, mmobj_t **ret);
static int  s5fs_create(vnode_t *vdir, const char *name, size_t namelen, vnode_t **result);
static int  s5fs_mknod(struct vnode *dir, const char *name, size_t namelen, int mode, devid_t devid);
static int  s5fs_lookup(vnode_t *base, const char *name, size_t namelen, vnode_t **result);
static int  s5fs_link(vnode_t *src, vnode_t *dir, const char *name, size_t namelen);
static int  s5fs_unlink(vnode_t *vdir, const char *name, size_t namelen);
static int  s5fs_mkdir(vnode_t *vdir, const char *name, size_t namelen);
static int  s5fs_rmdir(vnode_t *parent, const char *name, size_t namelen);
static int  s5fs_readdir(vnode_t *vnode, int offset, struct dirent *d);
static int  s5fs_stat(vnode_t *vnode, struct stat *ss);
static int  s5fs_fillpage(vnode_t *vnode, off_t offset, void *pagebuf);
static int  s5fs_dirtypage(vnode_t *vnode, off_t offset);
static int  s5fs_cleanpage(vnode_t *vnode, off_t offset, void *pagebuf);

fs_ops_t s5fs_fsops = {
        s5fs_read_vnode,
        s5fs_delete_vnode,
        s5fs_query_vnode,
        s5fs_umount
};

/* vnode operations table for directory files: */
static vnode_ops_t s5fs_dir_vops = {
        .read = NULL,
        .write = NULL,
        .mmap = NULL,
        .create = s5fs_create,
        .mknod = s5fs_mknod,
        .lookup = s5fs_lookup,
        .link = s5fs_link,
        .unlink = s5fs_unlink,
        .mkdir = s5fs_mkdir,
        .rmdir = s5fs_rmdir,
        .readdir = s5fs_readdir,
        .stat = s5fs_stat,
        .fillpage = s5fs_fillpage,
        .dirtypage = s5fs_dirtypage,
        .cleanpage = s5fs_cleanpage
};

/* vnode operations table for regular files: */
static vnode_ops_t s5fs_file_vops = {
        .read = s5fs_read,
        .write = s5fs_write,
        .mmap = s5fs_mmap,
        .create = NULL,
        .mknod = NULL,
        .lookup = NULL,
        .link = NULL,
        .unlink = NULL,
        .mkdir = NULL,
        .rmdir = NULL,
        .readdir = NULL,
        .stat = s5fs_stat,
        .fillpage = s5fs_fillpage,
        .dirtypage = s5fs_dirtypage,
        .cleanpage = s5fs_cleanpage
};

/*
 * Read fs->fs_dev and set fs_op, fs_root, and fs_i.
 *
 * Point fs->fs_i to an s5fs_t*, and initialize it.  Be sure to
 * verify the superblock (using s5_check_super()).  Use vget() to get
 * the root vnode for fs_root.
 *
 * Return 0 on success, negative on failure.
 */
int
s5fs_mount(struct fs *fs)
{
        int num;
        blockdev_t *dev;
        s5fs_t *s5;
        pframe_t *vp;

        KASSERT(fs);

        if (sscanf(fs->fs_dev, "disk%d", &num) != 1) {
                return -EINVAL;
        }

        if (!(dev = blockdev_lookup(MKDEVID(1, num)))) {
                return -EINVAL;
        }

        /* allocate and initialize an s5fs_t: */
        s5 = (s5fs_t *)kmalloc(sizeof(s5fs_t));

        if (!s5)
                return -ENOMEM;

        /*     init s5f_disk: */
        s5->s5f_bdev  = dev;

        /*     init s5f_super: */
        pframe_get(S5FS_TO_VMOBJ(s5), S5_SUPER_BLOCK, &vp);

        KASSERT(vp);

        s5->s5f_super = (s5_super_t *)(vp->pf_addr);

        if (s5_check_super(s5->s5f_super)) {
                /* corrupt */
                kfree(s5);
                return -EINVAL;
        }

        pframe_pin(vp);

        /*     init s5f_mutex: */
        kmutex_init(&s5->s5f_mutex);

        /*     init s5f_fs: */
        s5->s5f_fs = fs;


        /* Init the members of fs that we (the fs-implementation) are
         * responsible for initializing: */
        fs->fs_i = s5;
        fs->fs_op = &s5fs_fsops;
        fs->fs_root = vget(fs, s5->s5f_super->s5s_root_inode);

        return 0;
}

/* Implementation of fs_t entry points: */

/*
 * MACROS
 *
 * There are several macros which we have defined for you that
 * will make your life easier. Go find them, and use them.
 * Hint: Check out s5fs(_subr).h
 */


/*
 * See the comment in vfs.h for what is expected of this function.
 *
 * When this function returns, the inode link count should be incremented.
 * Note that most UNIX filesystems don't do this, they have a separate
 * flag to indicate that the VFS is using a file. However, this is
 * simpler to implement.
 *
 * To get the inode you need to use pframe_get then use the pf_addr
 * and the S5_INODE_OFFSET(vnode) to get the inode
 *
 * Don't forget to update linkcounts and pin the page.
 *
 * Note that the devid is stored in the indirect_block in the case of
 * a char or block device
 *
 * Finally, the main idea is to do special initialization based on the
 * type of inode (i.e. regular, directory, char/block device, etc').
 *
 */
static void
s5fs_read_vnode(vnode_t *vnode)
{
    dbg_print("s5fs_read_vnode: Reading VNODE\n");
    s5fs_t* vnode_fs = VNODE_TO_S5FS(vnode);
    struct mmobj* vnode_vmobj = S5FS_TO_VMOBJ(vnode_fs);

    /* Get the block of the vnode's inode */
    int inode_block = S5_INODE_BLOCK(vnode->vn_vno);

    pframe_t* pf;
    pframe_get(vnode_vmobj,inode_block,&pf);

    pframe_pin(pf);

    /* Get the offset of the inode and use this to get the inode*/
    int inode_offset = S5_INODE_OFFSET(vnode->vn_vno);
    s5_inode_t* inode = ((s5_inode_t*)pf->pf_addr + inode_offset);
    
    /* Update linkcounts and associations */
    inode->s5_linkcount++;
    vnode->vn_len = inode->s5_size;
    vnode->vn_i = inode;

    /* Get the type of the inode and initialize based on that */
    uint16_t inode_type = inode->s5_type;
    if(inode_type == S5_TYPE_DATA)
    {
        vnode->vn_mode = S_IFREG;
        vnode->vn_ops = &s5fs_file_vops;
    }
    else if(inode_type == S5_TYPE_DIR)
    {
        vnode->vn_mode = S_IFDIR;
        vnode->vn_ops = &s5fs_dir_vops;
    }
    else if(inode_type == S5_TYPE_CHR)
    {
        vnode->vn_mode = S_IFCHR;
        devid_t devid = inode->s5_indirect_block;
        vnode->vn_devid = devid;
        vnode->vn_cdev = bytedev_lookup(devid);
    }
    else if(inode_type == S5_TYPE_BLK)
    {
        vnode->vn_mode = S_IFBLK;
        devid_t devid = inode->s5_indirect_block;
        vnode->vn_devid = devid;
        vnode->vn_bdev = blockdev_lookup(devid);
    }

    s5_dirty_inode(vnode_fs,inode);

    /* NOT_YET_IMPLEMENTED("S5FS: s5fs_read_vnode"); */
}

/*
 * See the comment in vfs.h for what is expected of this function.
 *
 * When this function returns, the inode refcount should be decremented.
 *
 * You probably want to use s5_free_inode() if there are no more links to
 * the inode, and dont forget to unpin the page
 */
static void
s5fs_delete_vnode(vnode_t *vnode)
{
    /* Get all the info like read did */
    s5fs_t* vnode_s5fs = VNODE_TO_S5FS(vnode);
    struct mmobj* vnode_vmobj = S5FS_TO_VMOBJ(vnode_s5fs);

    /* Get the block of the vnode's inode */
    int inode_block = S5_INODE_BLOCK(vnode->vn_vno);

    pframe_t* pf;
    pframe_get(vnode_vmobj,inode_block,&pf);

    /* Get the inode and decrement linkcount */
    s5_inode_t* inode = VNODE_TO_S5INODE(vnode);
    inode->s5_linkcount--;

    if(inode->s5_linkcount == 0)
    {
        s5_free_inode(vnode);
    }

    pframe_unpin(pf);
    s5_dirty_inode(vnode_s5fs,inode);

    /* NOT_YET_IMPLEMENTED("S5FS: s5fs_delete_vnode"); */
}

/*
 * See the comment in vfs.h for what is expected of this function.
 *
 * The vnode still exists on disk if it has a linkcount greater than 1.
 * (Remember, VFS takes a reference on the inode as long as it uses it.)
 *
 */
static int
s5fs_query_vnode(vnode_t *vnode)
{
    dbg_print("s5fs_query_vnode: Querying Node\n");
    s5_inode_t* inode = VNODE_TO_S5INODE(vnode);
    if(inode->s5_linkcount > 1)
    {
        return 1;
    }
    else
    {
        return 0;
    }
    /* NOT_YET_IMPLEMENTED("S5FS: s5fs_query_vnode"); */
    /* return 0; */
}

/*
 * s5fs_check_refcounts()
 * vput root vnode
 */
static int
s5fs_umount(fs_t *fs)
{
        s5fs_t *s5 = (s5fs_t *)fs->fs_i;
        blockdev_t *bd = s5->s5f_bdev;
        pframe_t *sbp;
        int ret;

        if (s5fs_check_refcounts(fs)) {
                dbg(DBG_PRINT, "s5fs_umount: WARNING: linkcount corruption "
                    "discovered in fs on block device with major %d "
                    "and minor %d!!\n", MAJOR(bd->bd_id), MINOR(bd->bd_id));
        }
        if (s5_check_super(s5->s5f_super)) {
                dbg(DBG_PRINT, "s5fs_umount: WARNING: corrupted superblock "
                    "discovered on fs on block device with major %d "
                    "and minor %d!!\n", MAJOR(bd->bd_id), MINOR(bd->bd_id));
        }

        vnode_flush_all(fs);

        vput(fs->fs_root);
	dbg(DBG_PRINT, "s5fs_umount: Free data blocks %d\n", 
		s5fs_free_blocks(fs));

        if (0 > (ret = pframe_get(S5FS_TO_VMOBJ(s5), S5_SUPER_BLOCK, &sbp))) {
                panic("s5fs_umount: failed to pframe_get super block. "
                      "This should never happen (the page should already "
                      "be resident and pinned, and even if it wasn't, "
                      "block device readpage entry point does not "
                      "fail.\n");
        }

        KASSERT(sbp);

        pframe_unpin(sbp);

        kfree(s5);

        blockdev_flush_all(bd);

        return 0;
}




/* Implementation of vnode_t entry points: */

/*
 * Unless otherwise mentioned, these functions should leave all refcounts net
 * unchanged.
 */

/*
 * You will need to lock the vnode's mutex before doing anything that can block.
 * pframe functions can block, so probably what you want to do
 * is just lock the mutex in the s5fs_* functions listed below, and then not
 * worry about the mutexes in s5fs_subr.c.
 *
 * Note that you will not be calling pframe functions directly, but
 * s5fs_subr.c functions will be, so you need to lock around them.
 *
 * DO NOT TRY to do fine grained locking your first time through,
 * as it will break, and you will cry.
 *
 * Finally, you should read and understand the basic overview of
 * the s5fs_subr functions. All of the following functions might delegate,
 * and it will make your life easier if you know what is going on.
 */


/* Simply call s5_read_file. */
static int
s5fs_read(vnode_t *vnode, off_t offset, void *buf, size_t len)
{
    dbg_print("s5fs_read: Starting Read\n");
    kmutex_lock(&vnode->vn_mutex);
    int read_return = s5_read_file(vnode,offset,buf,len);
    kmutex_unlock(&vnode->vn_mutex);
    dbg_print("s5fs_read: End of Read\n");
    return read_return;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_read"); */
        /* return -1; */
}

/* Simply call s5_write_file. */
static int
s5fs_write(vnode_t *vnode, off_t offset, const void *buf, size_t len)
{
    kmutex_lock(&vnode->vn_mutex);
    int write_return = s5_write_file(vnode,offset,buf,len);
    kmutex_unlock(&vnode->vn_mutex);
    return write_return;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_write"); */
        /* return -1; */
}

/* This function is deceptivly simple, just return the vnode's
 * mmobj_t through the ret variable. Remember to watch the
 * refcount.
 *
 * Don't worry about this until VM.
 */
static int
s5fs_mmap(vnode_t *file, vmarea_t *vma, mmobj_t **ret)
{
    /* Don't know what to do about th */
       NOT_YET_IMPLEMENTED("VM: s5fs_mmap");
    
       return 0;
    
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the inode refcount of the file should be 2
 * and the vnode refcount should be 1.
 *
 * You probably want to use s5_alloc_inode(), s5_link(), and vget().
 */
static int
s5fs_create(vnode_t *dir, const char *name, size_t namelen, vnode_t **result)
{
    dbg_print("s5fs_create: Creating File, Name: %s\n",name);
    kmutex_lock(&dir->vn_mutex);

    int inode = s5_alloc_inode(dir->vn_fs,S5_TYPE_DATA,0);
    if(inode == -ENOSPC)
    {
        /* Error in alloc inode */
        kmutex_unlock(&dir->vn_mutex);
        return -ENOSPC;
    }
    else
    {
        vnode_t* child = vget(dir->vn_fs,inode);
        s5_inode_t* child_inode = VNODE_TO_S5INODE(child);
        int link = s5_link(dir,child,name,namelen);
        if(link == -1)
        {
            /* Error in link */ 
            kmutex_unlock(&dir->vn_mutex);
            return -1;
        }
        *result = child;
        /* s5_dirty_inode(child_inode); */
    } 

    kmutex_unlock(&dir->vn_mutex);
    return 0;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_create");
        * return -1;
        */
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * This function is similar to s5fs_create, but it creates a special
 * file specified by 'devid'.
 *
 * You probably want to use s5_alloc_inode, s5_link(), vget(), and vput().
 */
static int
s5fs_mknod(vnode_t *dir, const char *name, size_t namelen, int mode, devid_t devid)
{
    dbg_print("s5fs_mknod: Creating Device, Name: %s\n",name);

    KASSERT(mode == S_IFCHR || mode == S_IFBLK);

    kmutex_lock(&dir->vn_mutex);

    fs_t* dir_fs = dir->vn_fs;
    int inode;

    if(mode == S_IFCHR)
    {
        inode = s5_alloc_inode(dir_fs,S5_TYPE_CHR,devid);
    }
    else /* (mode == S_IFBLK) */
    {
        inode = s5_alloc_inode(dir_fs,S5_TYPE_BLK,devid);
    }

    if(inode == -ENOSPC)
    {
        /* Error in alloc inode */
        kmutex_unlock(&dir->vn_mutex);
        return -ENOSPC;
    }
    else
    {
        vnode_t* child = vget(dir_fs,inode);
        s5_inode_t* child_inode = VNODE_TO_S5INODE(child);
        int link = s5_link(dir,child,name,namelen);
        if(link == -1)
        {
            /* Error in link */ 
            kmutex_unlock(&dir->vn_mutex);
            return -1;
        }
        /* vput(child); */
        /* s5_dirty_inode(child_inode); */
    }

    kmutex_unlock(&dir->vn_mutex);
    return 0;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_mknod");
        * return -1;
        */
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * You probably want to use s5_find_dirent() and vget().
 */
int
s5fs_lookup(vnode_t *base, const char *name, size_t namelen, vnode_t **result)
{
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_lookup");
        * return -1;
        */
        dbg_print("s5fs_lookup: Entering Function, Name: %s\n",name);
        kmutex_lock(&base->vn_mutex);
        int vnode_dir_inode = s5_find_dirent(base,name,namelen);
        /* dbg_print("s5fs_lookup: a\n"); */
        if(vnode_dir_inode != -ENOENT)
        {
            dbg_print("s5fs_lookup: Find Dirent Worked!\n");
            *result = vget(base->vn_fs,vnode_dir_inode);
            kmutex_unlock(&base->vn_mutex);
            return 0;
        }
        /* dbg_print("s5fs_lookup: b\n"); */
        kmutex_unlock(&base->vn_mutex);
        /* dbg_print("s5fs_lookup: c\n"); */
        dbg_print("s5fs_lookup: Lookup Didn't Find\n");
        return -ENOENT;
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the inode refcount of the linked file
 * should be incremented.
 *
 * You probably want to use s5_link().
 */
static int
s5fs_link(vnode_t *src, vnode_t *dir, const char *name, size_t namelen)
{
    dbg_print("s5fs_link: Linking, Name: %s\n", name);
    kmutex_lock(&dir->vn_mutex);
    int link = s5_link(dir,src,name,namelen);
    if(link == -1)
    {
        kmutex_unlock(&dir->vn_mutex);
        return -1;
    }
    
    kmutex_unlock(&dir->vn_mutex);
    return link;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_link");
        * return -1;
        */
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the inode refcount of the unlinked file
 * should be decremented.
 *
 * You probably want to use s5_remove_dirent().
 */
static int
s5fs_unlink(vnode_t *dir, const char *name, size_t namelen)
{
    dbg_print("s5fs_unlink: Unlinkin, Name: %s\n", name);

    kmutex_lock(&dir->vn_mutex);
    int link = s5_remove_dirent(dir,name,namelen);
    kmutex_unlock(&dir->vn_mutex);

    return link;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_unlink");
        * return -1;
        */
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * You need to create the "." and ".." directory entries in the new
 * directory. These are simply links to the new directory and its
 * parent.
 *
 * When this function returns, the inode refcount on the parent should
 * be incremented, and the inode refcount on the new directory should be
 * 1. It might make more sense for the inode refcount on the new
 * directory to be 2 (since "." refers to it as well as its entry in the
 * parent dir), but convention is that empty directories have only 1
 * link.
 *
 * You probably want to use s5_alloc_inode, and s5_link().
 *
 * Assert, a lot.
 */
static int
s5fs_mkdir(vnode_t *dir, const char *name, size_t namelen)
{
    dbg_print("s5fs_mkdir: Creating Directory, Name: %s\n",name);

    if(dir->vn_len >= (int) S5_MAX_FILE_BLOCKS * S5_BLOCK_SIZE)
    {
        return -ENOSPC;
    }

    /* create a new directory inode */
    kmutex_lock(&dir->vn_mutex);
    
    int new_inode = s5_alloc_inode(dir->vn_fs, S5_TYPE_DIR, 0);
    KASSERT(new_inode >= 0);
    
    /* write entries to the new dir */
    vnode_t* created_vnode = vget(dir->vn_fs, new_inode);
    
    /* Create link to self */
    dbg_print("s5fs_mkdir: Linking To Self\n");
    int self_link = s5_link(created_vnode, created_vnode, ".", 1);
    if(self_link == -1)
    {
        kmutex_unlock(&dir->vn_mutex);
        return -1;
    }
    
    /* Create Link to Parent */
    dbg_print("s5fs_mkdir: Linking to Parent\n");
    int parent_link = s5_link(created_vnode, dir, "..", 2);
    if(parent_link == -1)
    {
        kmutex_unlock(&dir->vn_mutex);
        return -1;
    }
    
    /* Link Parent to new Dir */
    dbg_print("s5fs_mkdir: Linking from Parent Directory to new Directory\n");
    int parent_to_self = s5_link(dir, created_vnode, name, namelen);
    if(parent_to_self == -1)
    {
        kmutex_unlock(&dir->vn_mutex);
        return -1;
    }

    vput(created_vnode);
    
    s5_inode_t* ino = VNODE_TO_S5INODE(created_vnode);
    ino->s5_linkcount--;
    
    kmutex_unlock(&dir->vn_mutex);
    return 0;
    /* NOT_YET_IMPLEMENTED("S5FS: s5fs_mkdir");
    * return -1;
    */
}

/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * When this function returns, the inode refcount on the parent should be
 * decremented (since ".." in the removed directory no longer references
 * it). Remember that the directory must be empty (except for "." and
 * "..").
 *
 * You probably want to use s5_find_dirent() and s5_remove_dirent().
 */
static int
s5fs_rmdir(vnode_t *parent, const char *name, size_t namelen)
{
    dbg_print("s5fs_rmdir: Removing Directory");
    kmutex_lock(&parent->vn_mutex);
    
    int inode_to_remove = s5_find_dirent(parent,name,namelen);
    vnode_t* vnode_to_remove = vget(parent->vn_fs,inode_to_remove);

    /* Make sure that this vnode is a dir */
    if(vnode_to_remove->vn_mode == S_IFDIR)
    {
        /* Make sure that this is only a dirent, nothing else */
        if(vnode_to_remove->vn_len == 64)
        {  
            dbg_print("s5fs_rmdir: Removing Dir!\n");
            s5_remove_dirent(parent,name,namelen);
            s5_inode_t* parent_inode = VNODE_TO_S5INODE(parent);
            parent_inode->s5_linkcount--;
            vput(vnode_to_remove);
            kmutex_unlock(&parent->vn_mutex);
            return 0;
        }
        else
        {
            /* This isn't empty */
            dbg_print("s5fs_rmdir: Dir not empty D:\n");
            vput(vnode_to_remove);
            kmutex_unlock(&parent->vn_mutex);
            return -ENOTEMPTY;
        }
    }
    else
    {
        /* This isn't a dir */
        dbg_print("s5fs_rmdir: Tring to rmdir on a not dir\n");
        vput(vnode_to_remove);
        kmutex_unlock(&parent->vn_mutex);
        return -ENOTDIR;
    }
    
    /*     NOT_YET_IMPLEMENTED("S5FS: s5fs_rmdir");
    *    return -1;
    */  
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * Here you need to use s5_read_file() to read a s5_dirent_t from a directory
 * and copy that data into the given dirent. The value of d_off is dependent on
 * your implementation and may or may not b e necessary.  Finally, return the
 * number of bytes read.
 */
static int
s5fs_readdir(vnode_t *vnode, off_t offset, struct dirent *d)
{
    dbg_print("s5fs_readdir: Entering Function\n");

    kmutex_lock(&vnode->vn_mutex);

    s5_dirent_t read_dir;
    int total_read = s5_read_file(vnode,offset,(char*)&read_dir,sizeof(read_dir));

    d->d_ino = read_dir.s5d_inode;
    d->d_ino = offset;
    strcpy(d->d_name,read_dir.s5d_name);

    kmutex_unlock(&vnode->vn_mutex);

    if(total_read != sizeof(s5_dirent_t))
    {
        return -1;
    }
    else
    {
        return total_read;
    }
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_readdir");
        * return -1;
        */
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * Don't worry if you don't know what some of the fields in struct stat
 * mean. The ones you should be sure to set are st_mode, st_ino,
 * st_nlink, st_size, st_blksize, and st_blocks.
 *
 * You probably want to use s5_inode_blocks().
 */
static int
s5fs_stat(vnode_t *vnode, struct stat *ss)
{
    dbg_print("s5fs_stat: Getting the stats of a file\n");

    kmutex_lock(&vnode->vn_mutex);

    s5_inode_t* vn_inode = VNODE_TO_S5INODE(vnode);

    ss->st_mode = vnode->vn_mode;
    ss->st_ino = (int)vn_inode;
    ss->st_nlink = vn_inode->s5_linkcount;
    ss->st_size = vn_inode->s5_size;
    ss->st_blksize = S5_BLOCK_SIZE;
    ss->st_blocks = s5_inode_blocks(vnode);

    kmutex_unlock(&vnode->vn_mutex);

    return 0;
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_stat");
        * return -1;
        */
}


/*
 * See the comment in vnode.h for what is expected of this function.
 *
 * You'll probably want to use s5_seek_to_block and the device's
 * read_block function.
 */
static int
s5fs_fillpage(vnode_t *vnode, off_t offset, void *pagebuf)
{
    dbg_print("s5fs_fillpage: Filling Page...\n");
    int block_num = s5_seek_to_block(vnode,offset,0);
    KASSERT(block_num >= 0);
    if(block_num != 0)
    {
       /* Seek to Block found the block */
        s5fs_t* fs = VNODE_TO_S5FS(vnode);
        blockdev_t* bdev = fs->s5f_bdev;
        return bdev->bd_ops->read_block(bdev,pagebuf,block_num,1);
    }
    else
    {
        /* Sparse Block */  
        memset(pagebuf,0,PAGE_SIZE);
        return 0;
    }
    
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_fillpage");
        * return -1;
        */
        
}


/*
 * if this offset is NOT within a sparse region of the file
 *     return 0;
 *
 * attempt to make the region containing this offset no longer
 * sparse
 *     - attempt to allocate a free block
 *     - if no free blocks available, return -ENOSPC
 *     - associate this block with the inode; alter the inode as
 *       appropriate
 *         - dirty the page containing this inode
 *
 * Much of this can be done with s5_seek_to_block()
 */
static int
s5fs_dirtypage(vnode_t *vnode, off_t offset)
{
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_dirtypage");
        * return -1;
        */
    dbg_print("s5fs_dirtypage: Dirtying Page\n");
    int block_num = s5_seek_to_block(vnode,offset,0);
    s5fs_t* vnode_fs = VNODE_TO_S5FS(vnode);
    s5_inode_t* inode = VNODE_TO_S5INODE(vnode);
    KASSERT(block_num >= 0);

    if(block_num == 0)
    {
        /* Allocate Free Block */
        int block_num = s5_seek_to_block(vnode,offset,1);
        if(block_num == -ENOSPC)
        {
            return -ENOSPC;
        }

        s5_dirty_inode(vnode_fs,inode);
        return block_num;
    }
    else
    {
        return 0;
    }
}

/*
 * Like fillpage, but for writing.
 */
static int
s5fs_cleanpage(vnode_t *vnode, off_t offset, void *pagebuf)
{
    dbg_print("s5fs_cleanpage: Cleaning Page...\n");
    int block_num = s5_seek_to_block(vnode,offset,0);
    KASSERT(block_num >= 0);
    if(block_num != 0)
    {
       /* Seek to Block found the block */
        s5fs_t* fs = VNODE_TO_S5FS(vnode);
        blockdev_t* bdev = fs->s5f_bdev;
        return bdev->bd_ops->write_block(bdev,pagebuf,block_num,1);
    }
    else
    {
        /* Sparse Block */  
        /* memset(pagebuf,0,PAGE_SIZE); */
        /* Don't think you need to do this */
        return 0;
    }
        /* NOT_YET_IMPLEMENTED("S5FS: s5fs_cleanpage");
        * return -1;
        */
}

/* Diagnostic/Utility: */

/*
 * verify the superblock.
 * returns -1 if the superblock is corrupt, 0 if it is OK.
 */
static int
s5_check_super(s5_super_t *super)
{
        if (!(super->s5s_magic == S5_MAGIC
              && (super->s5s_free_inode < super->s5s_num_inodes
                  || super->s5s_free_inode == (uint32_t) - 1)
              && super->s5s_root_inode < super->s5s_num_inodes))
                return -1;
        if (super->s5s_version != S5_CURRENT_VERSION) {
                dbg(DBG_PRINT, "Filesystem is version %d; "
                    "only version %d is supported.\n",
                    super->s5s_version, S5_CURRENT_VERSION);
                return -1;
        }
        return 0;
}

static void
calculate_refcounts(int *counts, vnode_t *vnode)
{
        int ret;

        counts[vnode->vn_vno]++;
        dbg(DBG_S5FS, "calculate_refcounts: Incrementing count of inode %d to"
            " %d\n", vnode->vn_vno, counts[vnode->vn_vno]);
        /*
         * We only consider the children of this directory if this is the
         * first time we have seen it.  Otherwise, we would recurse forever.
         */
        if (counts[vnode->vn_vno] == 1 && S_ISDIR(vnode->vn_mode)) {
                int offset = 0;
                struct dirent d;
                vnode_t *child;

                while (0 < (ret = s5fs_readdir(vnode, offset, &d))) {
                        /*
                         * We don't count '.', because we don't increment the
                         * refcount for this (an empty directory only has a
                         * link count of 1).
                         */
                        if (0 != strcmp(d.d_name, ".")) {
                                child = vget(vnode->vn_fs, d.d_ino);
                                calculate_refcounts(counts, child);
                                vput(child);
                        }
                        offset += ret;
                }

                KASSERT(ret == 0);
        }
}

/*
 * This will check the refcounts for the filesystem.  It will ensure that that
 * the expected number of refcounts will equal the actual number.  To do this,
 * we have to create a data structure to hold the counts of all the expected
 * refcounts, and then walk the fs to calculate them.
 */
int
s5fs_check_refcounts(fs_t *fs)
{
        s5fs_t *s5fs = (s5fs_t *)fs->fs_i;
        int *refcounts;
        int ret = 0;
        uint32_t i;

        refcounts = kmalloc(s5fs->s5f_super->s5s_num_inodes * sizeof(int));
        KASSERT(refcounts);
        memset(refcounts, 0, s5fs->s5f_super->s5s_num_inodes * sizeof(int));

        calculate_refcounts(refcounts, fs->fs_root);
        --refcounts[fs->fs_root->vn_vno]; /* the call on the preceding line
                                           * caused this to be incremented
                                           * not because another fs link to
                                           * it was discovered */

        dbg(DBG_PRINT, "Checking refcounts of s5fs filesystem on block "
            "device with major %d, minor %d\n",
            MAJOR(s5fs->s5f_bdev->bd_id), MINOR(s5fs->s5f_bdev->bd_id));

        for (i = 0; i < s5fs->s5f_super->s5s_num_inodes; i++) {
                vnode_t *vn;

                if (!refcounts[i]) continue;

                vn = vget(fs, i);
                KASSERT(vn);

                if (refcounts[i] != VNODE_TO_S5INODE(vn)->s5_linkcount - 1) {
                        dbg(DBG_PRINT, "   Inode %d, expecting %d, found %d\n", i,
                            refcounts[i], VNODE_TO_S5INODE(vn)->s5_linkcount - 1);
                        ret = -1;
                }
                vput(vn);
        }

        dbg(DBG_PRINT, "Refcount check of s5fs filesystem on block "
            "device with major %d, minor %d completed %s.\n",
            MAJOR(s5fs->s5f_bdev->bd_id), MINOR(s5fs->s5f_bdev->bd_id),
            (ret ? "UNSUCCESSFULLY" : "successfully"));

        kfree(refcounts);
        return ret;
}
/* Count free blocks in the system */
int
s5fs_free_blocks(fs_t *fs) {
    s5fs_t *s5fs = FS_TO_S5FS(fs);
    mmobj_t *disk = S5FS_TO_VMOBJ(s5fs);
    s5_super_t *su = s5fs->s5f_super;
    pframe_t *pf = NULL;
    uint32_t *freel = NULL;
    int tot = 0;

    kmutex_lock(&s5fs->s5f_mutex);
    tot = su->s5s_nfree;
    freel = su->s5s_free_blocks;
    if ( freel[S5_NBLKS_PER_FNODE-1] !=  ~0u) tot += 1;
    while ( freel[S5_NBLKS_PER_FNODE-1] !=  ~0u) {
	tot += S5_NBLKS_PER_FNODE;
	pframe_get(disk, freel[S5_NBLKS_PER_FNODE-1], &pf);
	KASSERT(pf);
	freel = pf->pf_addr;
	dbg(DBG_S5FS, "next pointer %x\n", freel[S5_NBLKS_PER_FNODE-1]);
	/* Subtract out the last null */
	if ( !freel[S5_NBLKS_PER_FNODE-1]!= ~0) tot--;
    }
    kmutex_unlock(&s5fs->s5f_mutex);
    return tot;
}
