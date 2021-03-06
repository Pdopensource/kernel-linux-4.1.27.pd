/*
 *  linux/fs/nfs/localio.c
 *
 *  Copyright (C) 2014  Weston Andros Adamson <dros@primarydata.com>
 *
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/vfs.h>
#include <linux/file.h>
#include <linux/inet.h>
#include <linux/sunrpc/addr.h>
#include <linux/inetdevice.h>
#include <net/addrconf.h>
#include <linux/module.h>

#include <linux/nfs.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_xdr.h>

#include "internal.h"
#include "pnfs.h"

#define NFSDBG_FACILITY		NFSDBG_VFS

/*
 * The localio code needs to call into nfsd to do the filehandle -> struct path
 * mapping, but cannot be statically linked, because that will make the nfs
 * module depend on the nfsd module.
 *
 * Instead, do dynamic linking to the nfsd module. This way the nfs module
 * will only hold a reference on nfsd when it's actually in use. This also
 * allows some sanity checking, like giving up on localio if nfsd isn't loaded.
 */

struct nfs_local_open_ctx {
	spinlock_t lock;
	nfs_to_nfsd_open_t open_f;
	struct module *mod;
	atomic_t refcount;
};

static struct nfs_local_open_ctx __local_open_ctx __read_mostly;

static bool localio_enabled __read_mostly = true;
module_param(localio_enabled, bool, 0644);

static bool localio_datasync __read_mostly;
module_param(localio_datasync, bool, 0644);

bool nfs_server_is_local(const struct nfs_client *clp)
{
	return test_bit(NFS_CS_LOCAL_IO, &clp->cl_flags) != 0 &&
		localio_enabled;
}
EXPORT_SYMBOL_GPL(nfs_server_is_local);

void
nfs_local_init(void)
{
	struct nfs_local_open_ctx *ctx = &__local_open_ctx;

	ctx->open_f = NULL;
	ctx->mod = NULL;
	spin_lock_init(&ctx->lock);
	atomic_set(&ctx->refcount, 0);
}

static bool
nfs_local_get_lookup_ctx(void)
{
	struct nfs_local_open_ctx *ctx = &__local_open_ctx;
	const unsigned long *crc = NULL;
	struct module *mod;
	const struct kernel_symbol *sym;

	atomic_inc(&ctx->refcount);

	spin_lock(&ctx->lock);
	if (ctx->open_f == NULL) {
		spin_unlock(&ctx->lock);

		mutex_lock(&module_mutex);
		mod = find_module("nfsd");
		if (!mod || !try_module_get(mod)) {
			mutex_unlock(&module_mutex);
			goto out_bad;
		}
		sym = find_symbol("nfsd_open_local_fh", &ctx->mod, &crc,
				  true, true);
		mutex_unlock(&module_mutex);
		if (!sym)
			goto out_bad_put;

		dprintk("create lookup context %lu\n", sym->value);

		spin_lock(&ctx->lock);

		/* catch race */
		if (ctx->open_f != NULL)
			goto out_bad_unlock;

		ctx->open_f = (nfs_to_nfsd_open_t) sym->value;
		ctx->mod = mod;
	}
	spin_unlock(&ctx->lock);

	return true;

out_bad_unlock:
	spin_unlock(&ctx->lock);
out_bad_put:
	module_put(mod);
out_bad:
	atomic_dec(&ctx->refcount);
	return false;
}

static void
nfs_local_put_lookup_ctx(void)
{
	struct nfs_local_open_ctx *ctx = &__local_open_ctx;
	struct module *mod;

	if (atomic_dec_and_lock(&ctx->refcount, &ctx->lock)) {
		ctx->open_f = NULL;
		mod = ctx->mod;
		ctx->mod = NULL;
		spin_unlock(&ctx->lock);
		module_put(mod);
		dprintk("destroy lookup context\n");
	}
}

/*
 * nfs_local_enable - attempt to enable local i/o for an nfs_client
 */
void
nfs_local_enable(struct nfs_client *clp)
{
	if (nfs_local_get_lookup_ctx()) {
		dprintk("enabled local i/o\n");
		set_bit(NFS_CS_LOCAL_IO, &clp->cl_flags);
	}
}
EXPORT_SYMBOL_GPL(nfs_local_enable);

/*
 * nfs_local_disable - disable local i/o for an nfs_client
 */
void
nfs_local_disable(struct nfs_client *clp)
{
	if (test_and_clear_bit(NFS_CS_LOCAL_IO, &clp->cl_flags)) {
		dprintk("disabled local i/o\n");
		nfs_local_put_lookup_ctx();
	}
}
EXPORT_SYMBOL_GPL(nfs_local_disable);

/*
 * nfs_local_probe - probe local i/o support for an nfs_client
 */
void
nfs_local_probe(struct nfs_client *clp)
{
	struct sockaddr_in *sin;
	struct sockaddr_in6 *sin6;
	struct nfs_local_addr *addr;
	struct sockaddr *sap;
	bool enable = false;

	switch (clp->cl_addr.ss_family) {
	case AF_INET:
		sin = (struct sockaddr_in *)&clp->cl_addr;
		if (ntohs(sin->sin_port) != NFS_PORT)
			goto out;
		if (ipv4_is_loopback(sin->sin_addr.s_addr)) {
			dprintk("%s: detected IPv4 loopback address\n",
				__func__);
			enable = true;
		}
		break;
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)&clp->cl_addr;
		if (ntohs(sin6->sin6_port) != NFS_PORT)
			goto out;
		if (memcmp(&sin6->sin6_addr, &in6addr_loopback,
		    sizeof(struct in6_addr)) == 0) {
			dprintk("%s: detected IPv6 loopback address\n",
				__func__);
			enable = true;
		}
		break;
	default:
		break;
	}

	if (enable)
		goto out;

	list_for_each_entry(addr, &clp->cl_local_addrs, cl_addrs) {
		sap = (struct sockaddr *)&addr->address;
		if (rpc_cmp_addr((struct sockaddr *)&clp->cl_addr,
				      sap, false)) {
			dprintk("%s: detected local server.\n", __func__);
			enable = true;
			break;
		}
	}

out:
	if (enable)
		nfs_local_enable(clp);
}
EXPORT_SYMBOL_GPL(nfs_local_probe);

/*
 * nfs_local_open_fh - open a local filehandle
 *
 * Returns a pointer to a struct file or an ERR_PTR
 */
struct file *
nfs_local_open_fh(struct nfs_client *clp, struct rpc_cred *cred,
		  struct nfs_fh *fh, const fmode_t mode)
{
	struct nfs_local_open_ctx *ctx = &__local_open_ctx;
	struct file *filp;
	int flags = O_LARGEFILE;
	int status;

	switch (mode & (FMODE_READ | FMODE_WRITE)) {
	case FMODE_READ | FMODE_WRITE:
		flags |= O_RDWR;
		break;
	case FMODE_READ:
		flags |= O_RDONLY;
		break;
	case FMODE_WRITE:
		flags |= O_WRONLY;
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	status = ctx->open_f(clp->cl_rpcclient, cred, fh, mode, &filp);
	if (status < 0)
		filp = ERR_PTR(status);


	dprintk("%s: open local file %p", __func__, filp);
	return filp;
}
EXPORT_SYMBOL_GPL(nfs_local_open_fh);

static int
nfs_do_local_read(struct nfs_pgio_header *hdr, struct file *filp)
{
	struct inode *ino = file_inode(filp);
	struct page *page, **ppage;
	unsigned int len, pgbase;
	unsigned int remainder = hdr->args.count;
	unsigned int bytes = 0;
	ssize_t status = 0;
	loff_t pos;
	mm_segment_t oldfs;

	pos = hdr->args.offset;

	dprintk("%s: vfs_read count=%u pos=%llu\n",
		__func__, hdr->args.count, pos);

	ppage = &hdr->args.pages[hdr->args.pgbase >> PAGE_SHIFT];
	pgbase = hdr->args.pgbase & ~PAGE_MASK;
	hdr->res.eof = false;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	do {
		page = *ppage;
		len = min_t(unsigned int, remainder, PAGE_SIZE - pgbase);

		status = vfs_read(filp, kmap(page) + pgbase, len, &pos);
		kunmap(page);

		if (status != len) {
			if (status >= 0) {
				bytes += status;
				hdr->res.eof = true;
			}
			break;
		}
		bytes += status;
		remainder -= status;
		ppage++;
		pgbase = 0;
	} while (remainder != 0);
	set_fs(oldfs);

	if (hdr->args.offset + bytes >= i_size_read(ino))
		hdr->res.eof = true;

	dprintk("%s: read %u bytes eof %d.\n", __func__, bytes, hdr->res.eof);

	/* return bytes read on partial reads */
	if (!bytes)
		return status;
	return bytes;
}

static void
nfs_set_local_verifier(struct nfs_writeverf *verf, enum nfs3_stable_how how)
{
	memset(verf->verifier.data, 0xaa, sizeof(verf->verifier.data));
	verf->committed = how;
}

static void
nfs_get_vfs_attr(struct file *filp, struct nfs_fattr *fattr)
{
	struct kstat stat;

	if (fattr != NULL && vfs_getattr(&filp->f_path, &stat) == 0) {
		fattr->valid = NFS_ATTR_FATTR_CHANGE | NFS_ATTR_FATTR_SIZE |
			NFS_ATTR_FATTR_ATIME | NFS_ATTR_FATTR_MTIME |
			NFS_ATTR_FATTR_CTIME;
		fattr->size = stat.size;
		fattr->atime = stat.atime;
		fattr->mtime = stat.mtime;
		fattr->ctime = stat.ctime;
		fattr->change_attr = nfs_timespec_to_change_attr(&fattr->ctime);
	}
}

static int
nfs_do_local_write(struct nfs_pgio_header *hdr, struct file *filp)
{
	struct page *page, **ppage;
	unsigned int len, pgbase;
	unsigned int remainder = hdr->args.count;
	unsigned int bytes = 0;
	ssize_t status = 0;
	loff_t pos;
	mm_segment_t oldfs;

	pos = hdr->args.offset;

	dprintk("%s: vfs_write count=%u pos=%llu %s\n",
		__func__, hdr->args.count, pos,
		(hdr->args.stable == NFS_UNSTABLE) ?  "unstable" : "stable");

	ppage = &hdr->args.pages[hdr->args.pgbase >> PAGE_SHIFT];
	pgbase = hdr->args.pgbase & ~PAGE_MASK;

	/* It is always better to defer the commit */
	hdr->args.stable = NFS_UNSTABLE;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	do {
		page = *ppage;
		len = min_t(unsigned int, remainder, PAGE_SIZE - pgbase);

		status = vfs_write(filp, ((char __user *)kmap(page)) + pgbase,
				   len, &pos);
		kunmap(page);

		if (status != len) {
			if (status > 0)
				bytes += status;
			break;
		}
		bytes += status;
		remainder -= status;
		ppage++;
		pgbase = 0;
	} while (remainder != 0);
	set_fs(oldfs);

	dprintk("%s: wrote %u bytes.\n", __func__, bytes);

	nfs_set_local_verifier(hdr->res.verf, hdr->args.stable);
	nfs_get_vfs_attr(filp, hdr->res.fattr);

	/* return bytes written on partial writes */
	if (!bytes)
		return status;
	return bytes;
}

static struct file *
nfs_local_file_open_cached(struct nfs_client *clp, struct rpc_cred *cred,
			   struct nfs_fh *fh, const fmode_t mode,
			   struct nfs_open_context *ctx)
{
	struct file *filp = ctx->local_filp;

	if (!filp) {
		struct file *new = nfs_local_open_fh(clp, cred, fh, ctx->mode);
		if (IS_ERR_OR_NULL(new))
			return new;
		/* try to put this one in the slot */
		filp = cmpxchg(&ctx->local_filp, NULL, new);
		if (filp != NULL)
			fput(new);
		else
			filp = new;
	}
	return get_file(filp);
}

static struct file *
nfs_local_file_open(struct nfs_client *clp, struct rpc_cred *cred,
		    struct nfs_fh *fh, const fmode_t mode,
		    struct nfs_open_context *ctx,
		    struct pnfs_layout_segment *lseg, u32 ds_idx)
{
	struct nfs_server *s = NFS_SERVER(ctx->dentry->d_inode);

	if (lseg)
		return pnfs_local_open_fh(s, lseg, ds_idx, clp, cred, fh, mode);
	else
		return nfs_local_file_open_cached(clp, cred, fh, mode, ctx);
}

static struct file *
nfs_local_file_open_cdata(struct nfs_client *clp, struct rpc_cred *cred,
			  struct nfs_fh *fh, const fmode_t mode,
			  struct nfs_commit_data *cdata)
{
        return nfs_local_file_open(clp, cred, fh, mode, cdata->context,
				   cdata->lseg, cdata->ds_commit_index);
}

static struct file *
nfs_local_file_open_hdr(struct nfs_client *clp, struct rpc_cred *cred,
			struct nfs_fh *fh, const fmode_t mode,
			struct nfs_pgio_header *hdr)
{
        return nfs_local_file_open(clp, cred, fh, mode, hdr->args.context,
				   hdr->lseg, hdr->ds_commit_idx);
}

int
nfs_local_doio(struct nfs_client *clp, struct rpc_cred *cred,
	       struct nfs_pgio_header *hdr)
{
	struct file *filp;
	int status = 0;
	fmode_t mode;

	mode = hdr->rw_ops->rw_mode;

	filp = nfs_local_file_open_hdr(clp, cred, hdr->args.fh, mode, hdr);
	if (IS_ERR(filp))
		return PTR_ERR(filp);
	if (!filp)
		return -EBADF;

	switch (mode) {
	case FMODE_READ:
		status = nfs_do_local_read(hdr, filp);
		break;
	case FMODE_WRITE:
		status = nfs_do_local_write(hdr, filp);
		break;
	default:
		dprintk("%s: invalid mode: %d\n", __func__,
			hdr->rw_ops->rw_mode);
		fput(filp);
		return -EINVAL;
	}

	fput(filp);

	if (status >= 0) {
		hdr->res.count = status;
		hdr->task.tk_status = 0;
	} else {
		nfs_local_disable(clp);
		hdr->task.tk_status = status;
	}

	return status;
}
EXPORT_SYMBOL_GPL(nfs_local_doio);

int
nfs_local_commit(struct nfs_client *clp, struct rpc_cred *cred,
		 struct nfs_commit_data *data)
{
	struct file *filp;
	loff_t end;
	int status = 0;
	fmode_t mode;

	mode = FMODE_WRITE;

	filp = nfs_local_file_open_cdata(clp, cred, data->args.fh, mode, data);

	if (IS_ERR(filp))
		return PTR_ERR(filp);
	if (!filp)
		return -EBADF;

	dprintk("%s: commit %llu - %u\n", __func__,
		data->args.offset, data->args.count);

	end = data->args.count ? data->args.offset + data->args.count : -1;
	status = vfs_fsync_range(filp, data->args.offset, end, localio_datasync);
	fput(filp);
	if (status >= 0) {
		nfs_set_local_verifier(data->res.verf, NFS_FILE_SYNC);
		data->task.tk_status = 0;
	} else {
		nfs_local_disable(clp);
		data->task.tk_status = status;
	}

	return status;
}
EXPORT_SYMBOL_GPL(nfs_local_commit);

static int
nfs_client_add_addr(struct nfs_client *clnt, char *buf, gfp_t flags)
{
	struct nfs_local_addr *addr;
	struct sockaddr *sap;

	dprintk("%s: adding new local IP %s\n", __func__, buf);
	addr = kmalloc(sizeof(*addr), flags);
	if (!addr) {
		printk(KERN_WARNING "NFS: cannot alloc new addr\n");
		return -ENOMEM;
	}
	sap = (struct sockaddr *)&addr->address;
	addr->addrlen = rpc_pton(clnt->cl_net, buf, strlen(buf),
				sap, sizeof(addr->address));
	if (!addr->addrlen) {
		printk(KERN_WARNING "NFS: cannot parse new addr %s\n",
				buf);
		kfree(addr);
		return -EINVAL;
	}
	list_add(&addr->cl_addrs, &clnt->cl_local_addrs);

	return 0;
}

static int
nfs_client_add_v4_addr(struct nfs_client *clnt, struct in_device *indev,
		       char *buf, size_t buflen)
{
	int ret;

	for_ifa(indev) {
		snprintf(buf, buflen, "%pI4", &ifa->ifa_local);
		ret = nfs_client_add_addr(clnt, buf, GFP_KERNEL);
		if (ret < 0)
			return ret;
	} endfor_ifa(indev);

	return 0;
}

#if IS_ENABLED(CONFIG_IPV6)
static int
nfs_client_add_v6_addr(struct nfs_client *clnt, struct inet6_dev *in6dev,
		       char *buf, size_t buflen)
{
	struct inet6_ifaddr *ifp;
	int ret = 0;

	read_lock_bh(&in6dev->lock);
	list_for_each_entry(ifp, &in6dev->addr_list, if_list) {
		rpc_ntop6_addr_noscopeid(&ifp->addr, buf, buflen);
		ret = nfs_client_add_addr(clnt, buf, GFP_ATOMIC);
		if (ret < 0)
			goto out;
	}
out:
	read_unlock_bh(&in6dev->lock);
	return ret;
}
#else /* CONFIG_IPV6 */
static int
nfs_client_add_v6_addr(struct nfs_client *clnt, struct inet6_dev *in6dev,
		       char *buf, size_t buflen)
{
	return 0;
}
#endif

/* Find out all local IP addresses. Ignore errors
 * because local IO can be optional.
 */
void
nfs_probe_local_addr(struct nfs_client *clnt)
{
	struct net_device *dev;
	struct in_device *indev;
	struct inet6_dev *in6dev;
	char buf[INET6_ADDRSTRLEN + IPV6_SCOPE_ID_LEN];
	size_t buflen = sizeof(buf);

	rtnl_lock();

	for_each_netdev(clnt->cl_net, dev) {
		if (dev->type == ARPHRD_LOOPBACK ||
		    !(dev->flags & IFF_UP))
			continue;
		indev = __in_dev_get_rtnl(dev);
		if (indev &&
		    nfs_client_add_v4_addr(clnt, indev, buf, buflen) < 0)
			break;
		in6dev = __in6_dev_get(dev);
		if (in6dev &&
		    nfs_client_add_v6_addr(clnt, in6dev, buf, buflen) < 0)
			break;
	}

	rtnl_unlock();
}
