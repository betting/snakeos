/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/parser.h>
#include <linux/statfs.h>
#include <linux/random.h>
#include <linux/sched.h>

MODULE_AUTHOR("Miklos Szeredi <miklos@szeredi.hu>");
MODULE_DESCRIPTION("Filesystem in Userspace");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

static struct kmem_cache *fuse_inode_cachep;
struct list_head fuse_conn_list;
DEFINE_MUTEX(fuse_mutex);

#define FUSE_SUPER_MAGIC 0x65735546

#ifndef MAX_LFS_FILESIZE
#define MAX_LFS_FILESIZE (((u64)PAGE_CACHE_SIZE << (BITS_PER_LONG-1))-1)
#endif
struct fuse_mount_data {
	int fd;
	unsigned rootmode;
	unsigned user_id;
	unsigned group_id;
	unsigned fd_present : 1;
	unsigned rootmode_present : 1;
	unsigned user_id_present : 1;
	unsigned group_id_present : 1;
	unsigned flags;
	unsigned max_read;
	unsigned blksize;
};

static struct inode *fuse_alloc_inode(struct super_block *sb)
{
	struct inode *inode;
	struct fuse_inode *fi;

	inode = kmem_cache_alloc(fuse_inode_cachep, GFP_KERNEL);
	if (!inode)
		return NULL;

	fi = get_fuse_inode(inode);
	fi->i_time = 0;
	fi->nodeid = 0;
	fi->nlookup = 0;
	fi->forget_req = fuse_request_alloc();
	if (!fi->forget_req) {
		kmem_cache_free(fuse_inode_cachep, inode);
		return NULL;
	}

	return inode;
}

static void fuse_destroy_inode(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	if (fi->forget_req)
		fuse_request_free(fi->forget_req);
#ifndef KERNEL_2_6_18_PLUS
	if (inode->i_flock) {
		WARN_ON(inode->i_flock->fl_next);
		kfree(inode->i_flock);
		inode->i_flock = NULL;
	}
#endif
	kmem_cache_free(fuse_inode_cachep, inode);
}

#ifndef KERNEL_2_6_25_PLUS
static void fuse_read_inode(struct inode *inode)
{
	/* No op */
}
#endif

void fuse_send_forget(struct fuse_conn *fc, struct fuse_req *req,
		      unsigned long nodeid, u64 nlookup)
{
	struct fuse_forget_in *inarg = &req->misc.forget_in;
	inarg->nlookup = nlookup;
	req->in.h.opcode = FUSE_FORGET;
	req->in.h.nodeid = nodeid;
	req->in.numargs = 1;
	req->in.args[0].size = sizeof(struct fuse_forget_in);
	req->in.args[0].value = inarg;
	request_send_noreply(fc, req);
}

static void fuse_clear_inode(struct inode *inode)
{
	if (inode->i_sb->s_flags & MS_ACTIVE) {
		struct fuse_conn *fc = get_fuse_conn(inode);
		struct fuse_inode *fi = get_fuse_inode(inode);
		fuse_send_forget(fc, fi->forget_req, fi->nodeid, fi->nlookup);
		fi->forget_req = NULL;
	}
}

static int fuse_remount_fs(struct super_block *sb, int *flags, char *data)
{
	if (*flags & MS_MANDLOCK)
		return -EINVAL;

	return 0;
}

void fuse_change_attributes(struct inode *inode, struct fuse_attr *attr)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	if (S_ISREG(inode->i_mode) && i_size_read(inode) != attr->size)
#ifdef KERNEL_2_6_21_PLUS
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
#else
		invalidate_inode_pages(inode->i_mapping);
#endif

	inode->i_ino     = attr->ino;
	inode->i_mode    = (inode->i_mode & S_IFMT) + (attr->mode & 07777);
	inode->i_nlink   = attr->nlink;
	inode->i_uid     = attr->uid;
	inode->i_gid     = attr->gid;
	spin_lock(&fc->lock);
	i_size_write(inode, attr->size);
	spin_unlock(&fc->lock);
#ifdef HAVE_I_BLKSIZE
	inode->i_blksize = PAGE_CACHE_SIZE;
#endif
	inode->i_blocks  = attr->blocks;
	inode->i_atime.tv_sec   = attr->atime;
	inode->i_atime.tv_nsec  = attr->atimensec;
	inode->i_mtime.tv_sec   = attr->mtime;
	inode->i_mtime.tv_nsec  = attr->mtimensec;
	inode->i_ctime.tv_sec   = attr->ctime;
	inode->i_ctime.tv_nsec  = attr->ctimensec;
}

static void fuse_init_inode(struct inode *inode, struct fuse_attr *attr)
{
	inode->i_mode = attr->mode & S_IFMT;
	inode->i_size = attr->size;
	if (S_ISREG(inode->i_mode)) {
		fuse_init_common(inode);
		fuse_init_file_inode(inode);
	} else if (S_ISDIR(inode->i_mode))
		fuse_init_dir(inode);
	else if (S_ISLNK(inode->i_mode))
		fuse_init_symlink(inode);
	else if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode) ||
		 S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
		fuse_init_common(inode);
		init_special_inode(inode, inode->i_mode,
				   new_decode_dev(attr->rdev));
	} else
		BUG();
}

static int fuse_inode_eq(struct inode *inode, void *_nodeidp)
{
	unsigned long nodeid = *(unsigned long *) _nodeidp;
	if (get_node_id(inode) == nodeid)
		return 1;
	else
		return 0;
}

static int fuse_inode_set(struct inode *inode, void *_nodeidp)
{
	unsigned long nodeid = *(unsigned long *) _nodeidp;
	get_fuse_inode(inode)->nodeid = nodeid;
	return 0;
}

struct inode *fuse_iget(struct super_block *sb, unsigned long nodeid,
			int generation, struct fuse_attr *attr)
{
	struct inode *inode;
	struct fuse_inode *fi;
	struct fuse_conn *fc = get_fuse_conn_super(sb);

 retry:
	inode = iget5_locked(sb, nodeid, fuse_inode_eq, fuse_inode_set, &nodeid);
	if (!inode)
		return NULL;

	if ((inode->i_state & I_NEW)) {
		inode->i_flags |= S_NOATIME|S_NOCMTIME;
		inode->i_generation = generation;
		inode->i_data.backing_dev_info = &fc->bdi;
		fuse_init_inode(inode, attr);
		unlock_new_inode(inode);
	} else if ((inode->i_mode ^ attr->mode) & S_IFMT) {
		/* Inode has changed type, any I/O on the old should fail */
		make_bad_inode(inode);
		iput(inode);
		goto retry;
	}

	fi = get_fuse_inode(inode);
	spin_lock(&fc->lock);
	fi->nlookup ++;
	spin_unlock(&fc->lock);
	fuse_change_attributes(inode, attr);
	return inode;
}

#ifdef UMOUNT_BEGIN_VFSMOUNT
static void fuse_umount_begin(struct vfsmount *vfsmnt, int flags)
{
	if (flags & MNT_FORCE)
		fuse_abort_conn(get_fuse_conn_super(vfsmnt->mnt_sb));
}
#else
static void fuse_umount_begin(struct super_block *sb)
{
	fuse_abort_conn(get_fuse_conn_super(sb));
}
#endif

static void fuse_send_destroy(struct fuse_conn *fc)
{
	struct fuse_req *req = fc->destroy_req;
	if (req && fc->conn_init) {
		fc->destroy_req = NULL;
		req->in.h.opcode = FUSE_DESTROY;
		req->force = 1;
		request_send(fc, req);
		fuse_put_request(fc, req);
	}
}

static void fuse_put_super(struct super_block *sb)
{
	struct fuse_conn *fc = get_fuse_conn_super(sb);

	fuse_send_destroy(fc);
	spin_lock(&fc->lock);
	fc->connected = 0;
	fc->blocked = 0;
	spin_unlock(&fc->lock);
	/* Flush all readers on this fs */
	kill_fasync(&fc->fasync, SIGIO, POLL_IN);
	wake_up_all(&fc->waitq);
	wake_up_all(&fc->blocked_waitq);
	wake_up_all(&fc->reserved_req_waitq);
	mutex_lock(&fuse_mutex);
	list_del(&fc->entry);
	fuse_ctl_remove_conn(fc);
	mutex_unlock(&fuse_mutex);
	fuse_conn_put(fc);
}

static void convert_fuse_statfs(struct kstatfs *stbuf, struct fuse_kstatfs *attr)
{
	stbuf->f_type    = FUSE_SUPER_MAGIC;
	stbuf->f_bsize   = attr->bsize;
	stbuf->f_frsize  = attr->frsize;
	stbuf->f_blocks  = attr->blocks;
	stbuf->f_bfree   = attr->bfree;
	stbuf->f_bavail  = attr->bavail;
	stbuf->f_files   = attr->files;
	stbuf->f_ffree   = attr->ffree;
	stbuf->f_namelen = attr->namelen;
	/* fsid is left zero */
}

#ifdef KERNEL_2_6_18_PLUS
static int fuse_statfs(struct dentry *dentry, struct kstatfs *buf)
#else
static int fuse_statfs(struct super_block *sb, struct kstatfs *buf)
#endif
{
#ifdef KERNEL_2_6_18_PLUS
	struct super_block *sb = dentry->d_sb;
#endif
	struct fuse_conn *fc = get_fuse_conn_super(sb);
	struct fuse_req *req;
	struct fuse_statfs_out outarg;
	int err;

	if (!fuse_allow_task(fc, current)) {
		buf->f_type = FUSE_SUPER_MAGIC;
		return 0;
	}

	req = fuse_get_req(fc);
	if (IS_ERR(req))
		return PTR_ERR(req);

	memset(&outarg, 0, sizeof(outarg));
	req->in.numargs = 0;
	req->in.h.opcode = FUSE_STATFS;
#ifdef KERNEL_2_6_18_PLUS
	req->in.h.nodeid = get_node_id(dentry->d_inode);
#endif
	req->out.numargs = 1;
	req->out.args[0].size =
		fc->minor < 4 ? FUSE_COMPAT_STATFS_SIZE : sizeof(outarg);
	req->out.args[0].value = &outarg;
	request_send(fc, req);
	err = req->out.h.error;
	if (!err)
		convert_fuse_statfs(buf, &outarg.st);
	fuse_put_request(fc, req);
	return err;
}

enum {
	OPT_FD,
	OPT_ROOTMODE,
	OPT_USER_ID,
	OPT_GROUP_ID,
	OPT_DEFAULT_PERMISSIONS,
	OPT_ALLOW_OTHER,
	OPT_MAX_READ,
	OPT_BLKSIZE,
	OPT_ERR
};

static match_table_t tokens = {
	{OPT_FD,			"fd=%u"},
	{OPT_ROOTMODE,			"rootmode=%o"},
	{OPT_USER_ID,			"user_id=%u"},
	{OPT_GROUP_ID,			"group_id=%u"},
	{OPT_DEFAULT_PERMISSIONS,	"default_permissions"},
	{OPT_ALLOW_OTHER,		"allow_other"},
	{OPT_MAX_READ,			"max_read=%u"},
	{OPT_BLKSIZE,			"blksize=%u"},
	{OPT_ERR,			NULL}
};

static int parse_fuse_opt(char *opt, struct fuse_mount_data *d, int is_bdev)
{
	char *p;
	memset(d, 0, sizeof(struct fuse_mount_data));
	d->max_read = ~0;
	d->blksize = 512;

	/*
	 * For unprivileged mounts use current uid/gid.  Still allow
	 * "user_id" and "group_id" options for compatibility, but
	 * only if they match these values.
	 */
	if (!capable(CAP_SYS_ADMIN)) {
		d->user_id = current->uid;
		d->user_id_present = 1;
		d->group_id = current->gid;
		d->group_id_present = 1;

	}

	while ((p = strsep(&opt, ",")) != NULL) {
		int token;
		int value;
		substring_t args[MAX_OPT_ARGS];
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case OPT_FD:
			if (match_int(&args[0], &value))
				return 0;
			d->fd = value;
			d->fd_present = 1;
			break;

		case OPT_ROOTMODE:
			if (match_octal(&args[0], &value))
				return 0;
			if (!fuse_valid_type(value))
				return 0;
			d->rootmode = value;
			d->rootmode_present = 1;
			break;

		case OPT_USER_ID:
			if (match_int(&args[0], &value))
				return 0;
			if (d->user_id_present && d->user_id != value)
				return 0;
			d->user_id = value;
			d->user_id_present = 1;
			break;

		case OPT_GROUP_ID:
			if (match_int(&args[0], &value))
				return 0;
			if (d->group_id_present && d->group_id != value)
				return 0;
			d->group_id = value;
			d->group_id_present = 1;
			break;

		case OPT_DEFAULT_PERMISSIONS:
			d->flags |= FUSE_DEFAULT_PERMISSIONS;
			break;

		case OPT_ALLOW_OTHER:
			d->flags |= FUSE_ALLOW_OTHER;
			break;

		case OPT_MAX_READ:
			if (match_int(&args[0], &value))
				return 0;
			d->max_read = value;
			break;

		case OPT_BLKSIZE:
			if (!is_bdev || match_int(&args[0], &value))
				return 0;
			d->blksize = value;
			break;

		default:
			return 0;
		}
	}

	if (!d->fd_present || !d->rootmode_present ||
	    !d->user_id_present || !d->group_id_present)
		return 0;

	return 1;
}

static int fuse_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	struct fuse_conn *fc = get_fuse_conn_super(mnt->mnt_sb);

	seq_printf(m, ",user_id=%u", fc->user_id);
	seq_printf(m, ",group_id=%u", fc->group_id);
	if (fc->flags & FUSE_DEFAULT_PERMISSIONS)
		seq_puts(m, ",default_permissions");
	if (fc->flags & FUSE_ALLOW_OTHER)
		seq_puts(m, ",allow_other");
	if (fc->max_read != ~0)
		seq_printf(m, ",max_read=%u", fc->max_read);
	return 0;
}

#ifndef HAVE_KZALLOC
static void *kzalloc(size_t size, int flags)
{
	void *ret = kmalloc(size, flags);
	if (ret)
		memset(ret, 0, size);
	return ret;
}
#endif
static struct fuse_conn *new_conn(void)
{
	struct fuse_conn *fc;

	fc = kzalloc(sizeof(*fc), GFP_KERNEL);
	if (fc) {
#ifdef KERNEL_2_6_24_PLUS
		int err = bdi_init(&fc->bdi);
		if (err) {
			kfree(fc);
			return NULL;
		}
#endif
		spin_lock_init(&fc->lock);
		mutex_init(&fc->inst_mutex);
		atomic_set(&fc->count, 1);
		init_waitqueue_head(&fc->waitq);
		init_waitqueue_head(&fc->blocked_waitq);
		init_waitqueue_head(&fc->reserved_req_waitq);
		INIT_LIST_HEAD(&fc->pending);
		INIT_LIST_HEAD(&fc->processing);
		INIT_LIST_HEAD(&fc->io);
		INIT_LIST_HEAD(&fc->interrupts);
		atomic_set(&fc->num_waiting, 0);
		fc->bdi.ra_pages = (VM_MAX_READAHEAD * 1024) / PAGE_CACHE_SIZE;
		fc->bdi.unplug_io_fn = default_unplug_io_fn;
		fc->reqctr = 0;
		fc->blocked = 1;
		get_random_bytes(&fc->scramble_key, sizeof(fc->scramble_key));
	}
	return fc;
}

void fuse_conn_put(struct fuse_conn *fc)
{
	if (atomic_dec_and_test(&fc->count)) {
		if (fc->destroy_req)
			fuse_request_free(fc->destroy_req);
		mutex_destroy(&fc->inst_mutex);
#ifdef KERNEL_2_6_24_PLUS
		bdi_destroy(&fc->bdi);
#endif
		kfree(fc);
	}
}

struct fuse_conn *fuse_conn_get(struct fuse_conn *fc)
{
	atomic_inc(&fc->count);
	return fc;
}

static struct inode *get_root_inode(struct super_block *sb, unsigned mode)
{
	struct fuse_attr attr;
	memset(&attr, 0, sizeof(attr));

	attr.mode = mode;
	attr.ino = FUSE_ROOT_ID;
	attr.nlink = 1;
	return fuse_iget(sb, 1, 0, &attr);
}
#ifndef FUSE_MAINLINE
#ifdef HAVE_EXPORTFS_H
#include <linux/exportfs.h>
#endif

struct fuse_inode_handle
{
	u64 nodeid;
	u32 generation;
};

static struct dentry *fuse_get_dentry(struct super_block *sb,
				      struct fuse_inode_handle *handle)
{
	struct inode *inode;
	struct dentry *entry;

	if (handle->nodeid == 0)
		return ERR_PTR(-ESTALE);

	inode = ilookup5(sb, handle->nodeid, fuse_inode_eq, &handle->nodeid);
	if (!inode)
		return ERR_PTR(-ESTALE);
	if (inode->i_generation != handle->generation) {
		iput(inode);
		return ERR_PTR(-ESTALE);
	}

	entry = d_alloc_anon(inode);
	if (!entry) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	entry->d_op = &fuse_dentry_operations;

	return entry;
}

static int fuse_encode_fh(struct dentry *dentry, u32 *fh, int *max_len,
			   int connectable)
{
	struct inode *inode = dentry->d_inode;
	int len = *max_len;
	int type = 1;
	u64 nodeid;
	u32 generation;

	if (len < 3 || (connectable && len < 6))
		return  255;

	nodeid = get_fuse_inode(inode)->nodeid;
	generation = inode->i_generation;

	len = 3;
	fh[0] = (u32)(nodeid >> 32);
	fh[1] = (u32)(nodeid & 0xffffffff);
	fh[2] = generation;

	if (connectable && !S_ISDIR(inode->i_mode)) {
		struct inode *parent;

		spin_lock(&dentry->d_lock);
		parent = dentry->d_parent->d_inode;
		nodeid = get_fuse_inode(parent)->nodeid;
		generation = parent->i_generation;

		fh[3] = (u32)(nodeid >> 32);
		fh[4] = (u32)(nodeid & 0xffffffff);
		fh[5] = generation;
		spin_unlock(&dentry->d_lock);

		len = 6;
		type = 2;
	}

	*max_len = len;
	return type;
}

#ifdef KERNEL_2_6_24_PLUS
static struct dentry *fuse_fh_to_dentry(struct super_block *sb,
		struct fid *fid, int fh_len, int fh_type)
{
	struct fuse_inode_handle handle;

	if (fh_len < 3 || fh_type > 2)
		return NULL;

	handle.nodeid = (u64) fid->raw[0] << 32;
	handle.nodeid |= (u64) fid->raw[1];
	handle.generation = fid->raw[2];
	return fuse_get_dentry(sb, &handle);
}

static struct dentry *fuse_fh_to_parent(struct super_block *sb,
		struct fid *fid, int fh_len, int fh_type)
{
	struct fuse_inode_handle parent;

	if (fh_type != 2 || fh_len < 6)
		return NULL;

	parent.nodeid = (u64) fid->raw[3] << 32;
	parent.nodeid |= (u64) fid->raw[4];
	parent.generation = fid->raw[5];
	return fuse_get_dentry(sb, &parent);
}


static const struct export_operations fuse_export_operations = {
	.fh_to_dentry	= fuse_fh_to_dentry,
	.fh_to_parent	= fuse_fh_to_parent,
	.encode_fh	= fuse_encode_fh,
};
#else
static struct dentry *fuse_get_dentry_old(struct super_block *sb, void *objp)
{
	return fuse_get_dentry(sb, objp);
}

static struct export_operations fuse_export_operations;

static struct dentry *fuse_decode_fh(struct super_block *sb, u32 *fh,
			int fh_len, int fileid_type,
			int (*acceptable)(void *context, struct dentry *de),
			void *context)
{
	struct fuse_inode_handle handle;
	struct fuse_inode_handle parent;

	if (fh_len < 3 || fileid_type > 2)
		return NULL;

	if (fileid_type == 2) {
		if (fh_len < 6)
			return NULL;

		parent.nodeid = (u64) fh[3] << 32;
		parent.nodeid |= (u64) fh[4];
		parent.generation = fh[5];
	} else {
		parent.nodeid = 0;
		parent.generation = 0;
	}

	handle.nodeid = (u64) fh[0] << 32;
	handle.nodeid |= (u64) fh[1];
	handle.generation = fh[2];

	return fuse_export_operations.
		find_exported_dentry(sb, &handle, &parent, acceptable, context);
}

static struct export_operations fuse_export_operations = {
	.get_dentry	= fuse_get_dentry_old,
	.encode_fh      = fuse_encode_fh,
	.decode_fh	= fuse_decode_fh,
};
#endif
#endif

static struct super_operations fuse_super_operations = {
	.alloc_inode    = fuse_alloc_inode,
	.destroy_inode  = fuse_destroy_inode,
#ifndef KERNEL_2_6_25_PLUS
	.read_inode	= fuse_read_inode,
#endif
	.clear_inode	= fuse_clear_inode,
	.drop_inode	= generic_delete_inode,
	.remount_fs	= fuse_remount_fs,
	.put_super	= fuse_put_super,
	.umount_begin	= fuse_umount_begin,
	.statfs		= fuse_statfs,
	.show_options	= fuse_show_options,
};

static void process_init_reply(struct fuse_conn *fc, struct fuse_req *req)
{
	struct fuse_init_out *arg = &req->misc.init_out;

	if (req->out.h.error || arg->major != FUSE_KERNEL_VERSION)
		fc->conn_error = 1;
	else {
		unsigned long ra_pages;

		if (arg->minor >= 6) {
			ra_pages = arg->max_readahead / PAGE_CACHE_SIZE;
			if (arg->flags & FUSE_ASYNC_READ)
				fc->async_read = 1;
			if (!(arg->flags & FUSE_POSIX_LOCKS))
				fc->no_lock = 1;
		} else {
			ra_pages = fc->max_read / PAGE_CACHE_SIZE;
			fc->no_lock = 1;
		}

		fc->bdi.ra_pages = min(fc->bdi.ra_pages, ra_pages);
		fc->minor = arg->minor;
		fc->max_write = arg->minor < 5 ? 4096 : arg->max_write;
		fc->conn_init = 1;
	}
	fuse_put_request(fc, req);
	fc->blocked = 0;
	wake_up_all(&fc->blocked_waitq);
}

static void fuse_send_init(struct fuse_conn *fc, struct fuse_req *req)
{
	struct fuse_init_in *arg = &req->misc.init_in;

	arg->major = FUSE_KERNEL_VERSION;
	arg->minor = FUSE_KERNEL_MINOR_VERSION;
	arg->max_readahead = fc->bdi.ra_pages * PAGE_CACHE_SIZE;
	arg->flags |= FUSE_ASYNC_READ | FUSE_POSIX_LOCKS;
	req->in.h.opcode = FUSE_INIT;
	req->in.numargs = 1;
	req->in.args[0].size = sizeof(*arg);
	req->in.args[0].value = arg;
	req->out.numargs = 1;
	/* Variable length arguement used for backward compatibility
	   with interface version < 7.5.  Rest of init_out is zeroed
	   by do_get_request(), so a short reply is not a problem */
	req->out.argvar = 1;
	req->out.args[0].size = sizeof(struct fuse_init_out);
	req->out.args[0].value = &req->misc.init_out;
	req->end = process_init_reply;
	request_send_background(fc, req);
}

static u64 conn_id(void)
{
	static u64 ctr = 1;
	return ctr++;
}

static int fuse_fill_super(struct super_block *sb, void *data, int silent)
{
	struct fuse_conn *fc;
	struct inode *root;
	struct fuse_mount_data d;
	struct file *file;
	struct dentry *root_dentry;
	struct fuse_req *init_req;
	int err;
	int is_bdev = sb->s_bdev != NULL;

	if (sb->s_flags & MS_MANDLOCK)
		return -EINVAL;

	if (!parse_fuse_opt((char *) data, &d, is_bdev))
		return -EINVAL;

	/* This is a privileged option */
	if ((d.flags & FUSE_ALLOW_OTHER) && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (is_bdev) {
#ifdef CONFIG_BLOCK
		if (!sb_set_blocksize(sb, d.blksize))
			return -EINVAL;
#endif
	} else {
		sb->s_blocksize = PAGE_CACHE_SIZE;
		sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	}
	sb->s_magic = FUSE_SUPER_MAGIC;
	sb->s_op = &fuse_super_operations;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
#ifndef FUSE_MAINLINE
	sb->s_export_op = &fuse_export_operations;
#endif

	file = fget(d.fd);
	if (!file)
		return -EINVAL;

	if (file->f_op != &fuse_dev_operations)
		return -EINVAL;

	fc = new_conn();
	if (!fc)
		return -ENOMEM;

	fc->flags = d.flags;
	fc->user_id = d.user_id;
	fc->group_id = d.group_id;
	fc->max_read = d.max_read;

	/* Used by get_root_inode() */
	sb->s_fs_info = fc;

	err = -ENOMEM;
	root = get_root_inode(sb, d.rootmode);
	if (!root)
		goto err;

	root_dentry = d_alloc_root(root);
	if (!root_dentry) {
		iput(root);
		goto err;
	}

	init_req = fuse_request_alloc();
	if (!init_req)
		goto err_put_root;

	if (is_bdev) {
		fc->destroy_req = fuse_request_alloc();
		if (!fc->destroy_req)
			goto err_put_root;
	}

	mutex_lock(&fuse_mutex);
	err = -EINVAL;
	if (file->private_data)
		goto err_unlock;

	fc->id = conn_id();
	err = fuse_ctl_add_conn(fc);
	if (err)
		goto err_unlock;

	list_add_tail(&fc->entry, &fuse_conn_list);
	sb->s_root = root_dentry;
	fc->connected = 1;
	file->private_data = fuse_conn_get(fc);
	mutex_unlock(&fuse_mutex);
	/*
	 * atomic_dec_and_test() in fput() provides the necessary
	 * memory barrier for file->private_data to be visible on all
	 * CPUs after this
	 */
	fput(file);

	fuse_send_init(fc, init_req);

	return 0;

 err_unlock:
	mutex_unlock(&fuse_mutex);
	fuse_request_free(init_req);
 err_put_root:
	dput(root_dentry);
 err:
	fput(file);
	fuse_conn_put(fc);
	return err;
}

#ifdef KERNEL_2_6_18_PLUS
static int fuse_get_sb(struct file_system_type *fs_type,
		       int flags, const char *dev_name,
		       void *raw_data, struct vfsmount *mnt)
{
	return get_sb_nodev(fs_type, flags, raw_data, fuse_fill_super, mnt);
}
#else
static struct super_block *fuse_get_sb(struct file_system_type *fs_type,
				       int flags, const char *dev_name,
				       void *raw_data)
{
	return get_sb_nodev(fs_type, flags, raw_data, fuse_fill_super);
}
#endif

static struct file_system_type fuse_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "fuse",
	.get_sb		= fuse_get_sb,
	.kill_sb	= kill_anon_super,
	.fs_flags	= FS_HAS_SUBTYPE | FS_SAFE,
};

#ifdef CONFIG_BLOCK
#ifdef KERNEL_2_6_18_PLUS
static int fuse_get_sb_blk(struct file_system_type *fs_type,
			   int flags, const char *dev_name,
			   void *raw_data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, raw_data, fuse_fill_super,
			   mnt);
}
#else
static struct super_block *fuse_get_sb_blk(struct file_system_type *fs_type,
					   int flags, const char *dev_name,
					   void *raw_data)
{
	return get_sb_bdev(fs_type, flags, dev_name, raw_data,
			   fuse_fill_super);
}
#endif

static struct file_system_type fuseblk_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "fuseblk",
	.get_sb		= fuse_get_sb_blk,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV | FS_HAS_SUBTYPE,
};

static inline int register_fuseblk(void)
{
	return register_filesystem(&fuseblk_fs_type);
}

static inline void unregister_fuseblk(void)
{
	unregister_filesystem(&fuseblk_fs_type);
}
#else
static inline int register_fuseblk(void)
{
	return 0;
}

static inline void unregister_fuseblk(void)
{
}
#endif

#ifdef KERNEL_2_6_25_PLUS
static struct kobject *fuse_kobj;
static struct kobject *connections_kobj;
#else
#ifndef HAVE_FS_SUBSYS
static decl_subsys(fs, NULL, NULL);
#endif
static decl_subsys(fuse, NULL, NULL);
static decl_subsys(connections, NULL, NULL);
#endif

#ifdef KERNEL_2_6_24_PLUS
static void fuse_inode_init_once(struct kmem_cache *cachep, void *foo)
#else
static void fuse_inode_init_once(void *foo, struct kmem_cache *cachep,
				 unsigned long flags)
#endif
{
	struct inode * inode = foo;

#ifndef KERNEL_2_6_22_PLUS
	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR)
#endif
	inode_init_once(inode);
}

static int __init fuse_fs_init(void)
{
	int err;

	err = register_filesystem(&fuse_fs_type);
	if (err)
		goto out;

	err = register_fuseblk();
	if (err)
		goto out_unreg;

#ifdef KERNEL_2_6_23_PLUS
	fuse_inode_cachep = kmem_cache_create("fuse_inode",
					      sizeof(struct fuse_inode),
					      0, SLAB_HWCACHE_ALIGN,
					      fuse_inode_init_once);
#else
	fuse_inode_cachep = kmem_cache_create("fuse_inode",
					      sizeof(struct fuse_inode),
					      0, SLAB_HWCACHE_ALIGN,
					      fuse_inode_init_once, NULL);
#endif

	err = -ENOMEM;
	if (!fuse_inode_cachep)
		goto out_unreg2;

	return 0;

 out_unreg2:
	unregister_fuseblk();
 out_unreg:
	unregister_filesystem(&fuse_fs_type);
 out:
	return err;
}

static void fuse_fs_cleanup(void)
{
	unregister_filesystem(&fuse_fs_type);
	unregister_fuseblk();
	kmem_cache_destroy(fuse_inode_cachep);
}

static int fuse_sysfs_init(void)
{
	int err;

#ifdef KERNEL_2_6_25_PLUS
	fuse_kobj = kobject_create_and_add("fuse", fs_kobj);
	if (!fuse_kobj) {
		err = -ENOMEM;
		goto out_err;
	}

	connections_kobj = kobject_create_and_add("connections", fuse_kobj);
	if (!connections_kobj) {
		err = -ENOMEM;
		goto out_fuse_unregister;
	}

	return 0;

 out_fuse_unregister:
	kobject_put(fuse_kobj);
 out_err:
#else
#ifndef HAVE_FS_SUBSYS
	err = subsystem_register(&fs_subsys);
	if (err)
		return err;
#endif
#ifdef KERNEL_2_6_22_PLUS
	kobj_set_kset_s(&fuse_subsys, fs_subsys);
#else
	kset_set_kset_s(&fuse_subsys, fs_subsys);
#endif
	err = subsystem_register(&fuse_subsys);
	if (err)
		goto out_err;

#ifdef KERNEL_2_6_22_PLUS
	kobj_set_kset_s(&connections_subsys, fuse_subsys);
#else
	kset_set_kset_s(&connections_subsys, fuse_subsys);
#endif
	err = subsystem_register(&connections_subsys);
	if (err)
		goto out_fuse_unregister;

	return 0;

 out_fuse_unregister:
	subsystem_unregister(&fuse_subsys);
 out_err:
#ifndef HAVE_FS_SUBSYS
	subsystem_unregister(&fs_subsys);
#endif
#endif
	return err;
}

static void fuse_sysfs_cleanup(void)
{
#ifdef KERNEL_2_6_25_PLUS
	kobject_put(connections_kobj);
	kobject_put(fuse_kobj);
#else
	subsystem_unregister(&connections_subsys);
	subsystem_unregister(&fuse_subsys);
#ifndef HAVE_FS_SUBSYS
	subsystem_unregister(&fs_subsys);
#endif
#endif
}

static int __init fuse_init(void)
{
	int res;

	printk("fuse init (API version %i.%i)\n",
	       FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION);
#ifndef FUSE_MAINLINE
	printk("fuse distribution version: 2.7.4\n");
#endif

	INIT_LIST_HEAD(&fuse_conn_list);
	res = fuse_fs_init();
	if (res)
		goto err;

	res = fuse_dev_init();
	if (res)
		goto err_fs_cleanup;

	res = fuse_sysfs_init();
	if (res)
		goto err_dev_cleanup;

	res = fuse_ctl_init();
	if (res)
		goto err_sysfs_cleanup;

	return 0;

 err_sysfs_cleanup:
	fuse_sysfs_cleanup();
 err_dev_cleanup:
	fuse_dev_cleanup();
 err_fs_cleanup:
	fuse_fs_cleanup();
 err:
	return res;
}

static void __exit fuse_exit(void)
{
	printk(KERN_DEBUG "fuse exit\n");

	fuse_ctl_cleanup();
	fuse_sysfs_cleanup();
	fuse_fs_cleanup();
	fuse_dev_cleanup();
}

module_init(fuse_init);
module_exit(fuse_exit);
