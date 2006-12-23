/*-
 * Parts Copyright (c) 1995 Terrence R. Lambert
 * Copyright (c) 1995 Julian R. Elischer
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Terrence R. Lambert.
 * 4. The name Terrence R. Lambert may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Julian R. Elischer ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE TERRENCE R. LAMBERT BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/kern/kern_conf.c,v 1.73.2.3 2003/03/10 02:18:25 imp Exp $
 * $DragonFly: src/sys/kern/kern_conf.c,v 1.20 2006/12/23 00:35:03 swildner Exp $
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/vnode.h>
#include <sys/queue.h>
#include <sys/device.h>
#include <machine/stdarg.h>

#define cdevsw_ALLOCSTART	(NUMCDEVSW/2)

MALLOC_DEFINE(M_DEVT, "cdev_t", "dev_t storage");

/*
 * This is the number of hash-buckets.  Experiements with 'real-life'
 * udev_t's show that a prime halfway between two powers of two works
 * best.
 */
#define DEVT_HASH 83

/* The number of cdev_t's we can create before malloc(9) kick in.  */
#define DEVT_STASH 50

static struct cdev devt_stash[DEVT_STASH];
static LIST_HEAD(, cdev) dev_hash[DEVT_HASH];
static LIST_HEAD(, cdev) dev_free_list;

static int free_devt;
SYSCTL_INT(_debug, OID_AUTO, free_devt, CTLFLAG_RW, &free_devt, 0, "");
int dev_ref_debug = 0;
SYSCTL_INT(_debug, OID_AUTO, dev_refs, CTLFLAG_RW, &dev_ref_debug, 0, "");

/*
 * cdev_t and u_dev_t primitives.  Note that the major number is always
 * extracted from si_udev, not from si_devsw, because si_devsw is replaced
 * when a device is destroyed.
 */
int
major(cdev_t x)
{
	if (x == NOCDEV)
		return NOUDEV;
	return((x->si_udev >> 8) & 0xff);
}

int
minor(cdev_t x)
{
	if (x == NOCDEV)
		return NOUDEV;
	return(x->si_udev & 0xffff00ff);
}

int
lminor(cdev_t x)
{
	int i;

	if (x == NOCDEV)
		return NOUDEV;
	i = minor(x);
	return ((i & 0xff) | (i >> 8));
}

/*
 * This is a bit complex because devices are always created relative to
 * a particular cdevsw, including 'hidden' cdevsw's (such as the raw device
 * backing a disk subsystem overlay), so we have to compare both the
 * devsw and udev fields to locate the correct device.
 *
 * The device is created if it does not already exist.  If SI_ADHOC is not
 * set the device will be referenced (once) and SI_ADHOC will be set.
 * The caller must explicitly add additional references to the device if
 * the caller wishes to track additional references.
 *
 * NOTE: The passed ops vector must normally match the device.  This is
 * because the kernel may create shadow devices that are INVISIBLE TO
 * USERLAND.  For example, the block device backing a disk is created
 * as a shadow underneath the user-visible disklabel management device.
 * Sometimes a device ops vector can be overridden, such as by /dev/console.
 * In this case and this case only we allow a match when the ops vector
 * otherwise would not match.
 */
static
cdev_t
hashdev(struct dev_ops *ops, int x, int y, int allow_intercept)
{
	struct cdev *si;
	udev_t	udev;
	int hash;
	static int stashed;

	udev = makeudev(x, y);
	hash = udev % DEVT_HASH;
	LIST_FOREACH(si, &dev_hash[hash], si_hash) {
		if (si->si_udev == udev) {
			if (si->si_ops == ops)
				return (si);
			if (allow_intercept && (si->si_flags & SI_INTERCEPTED))
				return (si);
		}
	}
	if (stashed >= DEVT_STASH) {
		MALLOC(si, struct cdev *, sizeof(*si), M_DEVT,
		    M_WAITOK|M_USE_RESERVE|M_ZERO);
	} else if (LIST_FIRST(&dev_free_list)) {
		si = LIST_FIRST(&dev_free_list);
		LIST_REMOVE(si, si_hash);
	} else {
		si = devt_stash + stashed++;
		si->si_flags |= SI_STASHED;
	}
	si->si_ops = ops;
	si->si_flags |= SI_HASHED | SI_ADHOC;
	si->si_udev = udev;
	si->si_refs = 1;
	LIST_INSERT_HEAD(&dev_hash[hash], si, si_hash);

	dev_dclone(si);
	if (ops != &dead_dev_ops)
		++ops->head.refs;
	if (dev_ref_debug) {
		kprintf("create    dev %p %s(minor=%08x) refs=%d\n", 
			si, devtoname(si), uminor(si->si_udev),
			si->si_refs);
	}
        return (si);
}

/*
 * Convert a device pointer to a device number
 */
udev_t
dev2udev(cdev_t x)
{
	if (x == NOCDEV)
		return NOUDEV;
	return (x->si_udev);
}

/*
 * Convert a device number to a device pointer.  The device is referenced
 * ad-hoc, meaning that the caller should call reference_dev() if it wishes
 * to keep ahold of the returned structure long term.
 *
 * The returned device is associated with the currently installed cdevsw
 * for the requested major number.  NOCDEV is returned if the major number
 * has not been registered.
 */
cdev_t
udev2dev(udev_t x, int b)
{
	cdev_t dev;
	struct dev_ops *ops;

	if (x == NOUDEV || b != 0)
		return(NOCDEV);
	ops = dev_ops_get(umajor(x), uminor(x));
	if (ops == NULL)
		return(NOCDEV);
	dev = hashdev(ops, umajor(x), uminor(x), TRUE);
	return(dev);
}

int
dev_is_good(cdev_t dev)
{
	if (dev != NOCDEV && dev->si_ops != &dead_dev_ops)
		return(1);
	return(0);
}

/*
 * Various user device number extraction and conversion routines
 */
int
uminor(udev_t dev)
{
	return(dev & 0xffff00ff);
}

int
umajor(udev_t dev)
{
	return((dev & 0xff00) >> 8);
}

udev_t
makeudev(int x, int y)
{
        return ((x << 8) | y);
}

/*
 * Create an internal or external device.
 *
 * Device majors can be overloaded and used directly by the kernel without
 * conflict, but userland will only see the particular device major that
 * has been installed with dev_ops_add().
 *
 * This routine creates and returns an unreferenced ad-hoc entry for the
 * device which will remain intact until the device is destroyed.  If the
 * caller intends to store the device pointer it must call reference_dev()
 * to retain a real reference to the device.
 *
 * If an entry already exists, this function will set (or override)
 * its cred requirements and name (XXX DEVFS interface).
 */
cdev_t
make_dev(struct dev_ops *ops, int minor, uid_t uid, gid_t gid, 
	int perms, const char *fmt, ...)
{
	cdev_t	dev;
	__va_list ap;
	int i;

	/*
	 * compile the cdevsw and install the device
	 */
	compile_dev_ops(ops);
	dev = hashdev(ops, ops->head.maj, minor, FALSE);

	/*
	 * Set additional fields (XXX DEVFS interface goes here)
	 */
	__va_start(ap, fmt);
	i = kvcprintf(fmt, NULL, dev->si_name, 32, ap);
	dev->si_name[i] = '\0';
	__va_end(ap);

	return (dev);
}

/*
 * This function is similar to make_dev() but no cred information or name
 * need be specified.
 */
cdev_t
make_adhoc_dev(struct dev_ops *ops, int minor)
{
	cdev_t dev;

	dev = hashdev(ops, ops->head.maj, minor, FALSE);
	return(dev);
}

/*
 * This function is similar to make_dev() except the new device is created
 * using an old device as a template.
 */
cdev_t
make_sub_dev(cdev_t odev, int minor)
{
	cdev_t	dev;

	dev = hashdev(odev->si_ops, umajor(odev->si_udev), minor, FALSE);

	/*
	 * Copy cred requirements and name info XXX DEVFS.
	 */
	if (dev->si_name[0] == 0 && odev->si_name[0])
		bcopy(odev->si_name, dev->si_name, sizeof(dev->si_name));
	return (dev);
}

/*
 * destroy_dev() removes the adhoc association for a device and revectors
 * its ops to &dead_dev_ops.
 *
 * This routine releases the reference count associated with the ADHOC
 * entry, plus releases the reference count held by the caller.  What this
 * means is that you should not call destroy_dev(make_dev(...)), because
 * make_dev() does not bump the reference count (beyond what it needs to
 * create the ad-hoc association).  Any procedure that intends to destroy
 * a device must have its own reference to it first.
 */
void
destroy_dev(cdev_t dev)
{
	int hash;

	if (dev == NOCDEV)
		return;
	if ((dev->si_flags & SI_ADHOC) == 0) {
		release_dev(dev);
		return;
	}
	if (dev_ref_debug) {
		kprintf("destroy   dev %p %s(minor=%08x) refs=%d\n", 
			dev, devtoname(dev), uminor(dev->si_udev),
			dev->si_refs);
	}
	if (dev->si_refs < 2) {
		kprintf("destroy_dev(): too few references on device! "
			"%p %s(minor=%08x) refs=%d\n",
		    dev, devtoname(dev), uminor(dev->si_udev),
		    dev->si_refs);
	}
	dev->si_flags &= ~SI_ADHOC;
	if (dev->si_flags & SI_HASHED) {
		hash = dev->si_udev % DEVT_HASH;
		LIST_REMOVE(dev, si_hash);
		dev->si_flags &= ~SI_HASHED;
	}

	/*
	 * We have to release the ops reference before we replace the
	 * device switch with dead_dev_ops.
	 */
	if (dead_dev_ops.d_strategy == NULL)
		compile_dev_ops(&dead_dev_ops);
	if (dev->si_ops && dev->si_ops != &dead_dev_ops)
		dev_ops_release(dev->si_ops);
	dev->si_drv1 = NULL;
	dev->si_drv2 = NULL;
	dev->si_ops = &dead_dev_ops;
	--dev->si_refs;		/* release adhoc association reference */
	release_dev(dev);	/* release callers reference */
}

/*
 * Destroy all ad-hoc device associations associated with a domain within a
 * device switch.  Only the minor numbers are included in the mask/match
 * values. 
 *
 * Unlike the ops functions whos link structures do not contain
 * any major bits, this function scans through the dev list via si_udev
 * which is a 32 bit field that contains both major and minor bits.
 * Because of this, we must mask the minor bits in the passed mask variable
 * to allow -1 to be specified generically.
 *
 * The caller must not include any major bits in the match value.
 */
void
destroy_all_devs(struct dev_ops *ops, u_int mask, u_int match)
{
	int i;
	cdev_t dev;
	cdev_t ndev;

	mask = uminor(mask);
	for (i = 0; i < DEVT_HASH; ++i) {
		ndev = LIST_FIRST(&dev_hash[i]);
		while ((dev = ndev) != NULL) {
		    ndev = LIST_NEXT(dev, si_hash);
		    KKASSERT(dev->si_flags & SI_ADHOC);
		    if (dev->si_ops == ops && 
			(dev->si_udev & mask) == match
		    ) {
			++dev->si_refs;
			destroy_dev(dev);
		    }
		}
	}
}

/*
 * Add a reference to a device.  Callers generally add their own references
 * when they are going to store a device node in a variable for long periods
 * of time, to prevent a disassociation from free()ing the node.
 *
 * Also note that a caller that intends to call destroy_dev() must first
 * obtain a reference on the device.  The ad-hoc reference you get with
 * make_dev() and friends is NOT sufficient to be able to call destroy_dev().
 */
cdev_t
reference_dev(cdev_t dev)
{
	if (dev != NOCDEV) {
		++dev->si_refs;
		if (dev_ref_debug) {
			kprintf("reference dev %p %s(minor=%08x) refs=%d\n", 
			    dev, devtoname(dev), uminor(dev->si_udev),
			    dev->si_refs);
		}
	}
	return(dev);
}

/*
 * release a reference on a device.  The device will be freed when the last
 * reference has been released.
 *
 * NOTE: we must use si_udev to figure out the original (major, minor),
 * because si_ops could already be pointing at dead_dev_ops.
 */
void
release_dev(cdev_t dev)
{
	if (dev == NOCDEV)
		return;
	if (free_devt) {
		KKASSERT(dev->si_refs > 0);
	} else {
		if (dev->si_refs <= 0) {
			kprintf("Warning: extra release of dev %p(%s)\n",
			    dev, devtoname(dev));
			free_devt = 0;	/* prevent bad things from occuring */
		}
	}
	--dev->si_refs;
	if (dev_ref_debug) {
		kprintf("release   dev %p %s(minor=%08x) refs=%d\n", 
			dev, devtoname(dev), uminor(dev->si_udev),
			dev->si_refs);
	}
	if (dev->si_refs == 0) {
		if (dev->si_flags & SI_ADHOC) {
			kprintf("Warning: illegal final release on ADHOC"
				" device %p(%s), the device was never"
				" destroyed!\n",
				dev, devtoname(dev));
		}
		if (dev->si_flags & SI_HASHED) {
			kprintf("Warning: last release on device, no call"
				" to destroy_dev() was made! dev %p(%s)\n",
				dev, devtoname(dev));
			dev->si_refs = 3;
			destroy_dev(dev);
			dev->si_refs = 0;
		}
		if (SLIST_FIRST(&dev->si_hlist) != NULL) {
			kprintf("Warning: last release on device, vnode"
				" associations still exist! dev %p(%s)\n",
				dev, devtoname(dev));
			free_devt = 0;	/* prevent bad things from occuring */
		}
		if (dev->si_ops && dev->si_ops != &dead_dev_ops) {
			dev_ops_release(dev->si_ops);
			dev->si_ops = NULL;
		}
		if (free_devt) {
			if (dev->si_flags & SI_STASHED) {
				bzero(dev, sizeof(*dev));
				LIST_INSERT_HEAD(&dev_free_list, dev, si_hash);
			} else {
				FREE(dev, M_DEVT);
			}
		}
	}
}

const char *
devtoname(cdev_t dev)
{
	int mynor;
	int len;
	char *p;
	const char *dname;

	if (dev == NOCDEV)
		return("#nodev");
	if (dev->si_name[0] == '#' || dev->si_name[0] == '\0') {
		p = dev->si_name;
		len = sizeof(dev->si_name);
		if ((dname = dev_dname(dev)) != NULL)
			ksnprintf(p, len, "#%s/", dname);
		else
			ksnprintf(p, len, "#%d/", major(dev));
		len -= strlen(p);
		p += strlen(p);
		mynor = minor(dev);
		if (mynor < 0 || mynor > 255)
			ksnprintf(p, len, "%#x", (u_int)mynor);
		else
			ksnprintf(p, len, "%d", mynor);
	}
	return (dev->si_name);
}

