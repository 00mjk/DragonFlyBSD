/*
 * Copyright (c) 1990 University of Utah.
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
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
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)device_pager.c	8.1 (Berkeley) 6/11/93
 * $FreeBSD: src/sys/vm/device_pager.c,v 1.46.2.1 2000/08/02 21:54:37 peter Exp $
 * $DragonFly: src/sys/vm/device_pager.c,v 1.12 2006/11/20 20:53:06 dillon Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/mman.h>
#include <sys/device.h>
#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/thread2.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_zone.h>

static void dev_pager_init (void);
static vm_object_t dev_pager_alloc (void *, off_t, vm_prot_t, off_t);
static void dev_pager_dealloc (vm_object_t);
static int dev_pager_getpages (vm_object_t, vm_page_t *, int, int);
static void dev_pager_putpages (vm_object_t, vm_page_t *, int, 
		boolean_t, int *);
static boolean_t dev_pager_haspage (vm_object_t, vm_pindex_t, int *,
		int *);

/* list of device pager objects */
static struct pagerlst dev_pager_object_list;
static TAILQ_HEAD(, vm_page) dev_freepages_list;
static MALLOC_DEFINE(M_FICTITIOUS_PAGES, "device-mapped pages", "Device mapped pages");

static vm_page_t dev_pager_getfake (vm_paddr_t);
static void dev_pager_putfake (vm_page_t);

static int dev_pager_alloc_lock, dev_pager_alloc_lock_want;

struct pagerops devicepagerops = {
	dev_pager_init,
	dev_pager_alloc,
	dev_pager_dealloc,
	dev_pager_getpages,
	dev_pager_putpages,
	dev_pager_haspage,
	NULL
};

static void
dev_pager_init(void)
{
	TAILQ_INIT(&dev_pager_object_list);
	TAILQ_INIT(&dev_freepages_list);
}

static vm_object_t
dev_pager_alloc(void *handle, off_t size, vm_prot_t prot, off_t foff)
{
	cdev_t dev;
	vm_object_t object;
	unsigned int npages;
	vm_offset_t off;

	/*
	 * Make sure this device can be mapped.
	 */
	dev = handle;

	/*
	 * Offset should be page aligned.
	 */
	if (foff & PAGE_MASK)
		return (NULL);

	size = round_page(size);

	/*
	 * Check that the specified range of the device allows the desired
	 * protection.
	 *
	 * XXX assumes VM_PROT_* == PROT_*
	 */
	npages = OFF_TO_IDX(size);
	for (off = foff; npages--; off += PAGE_SIZE) {
		if (dev_dmmap(dev, off, (int)prot) == -1)
			return (NULL);
	}

	/*
	 * Lock to prevent object creation race condition.
	 */
	while (dev_pager_alloc_lock) {
		dev_pager_alloc_lock_want++;
		tsleep(&dev_pager_alloc_lock, 0, "dvpall", 0);
		dev_pager_alloc_lock_want--;
	}
	dev_pager_alloc_lock = 1;

	/*
	 * Look up pager, creating as necessary.
	 */
	object = vm_pager_object_lookup(&dev_pager_object_list, handle);
	if (object == NULL) {
		/*
		 * Allocate object and associate it with the pager.
		 */
		object = vm_object_allocate(OBJT_DEVICE,
			OFF_TO_IDX(foff + size));
		object->handle = handle;
		TAILQ_INIT(&object->un_pager.devp.devp_pglist);
		TAILQ_INSERT_TAIL(&dev_pager_object_list, object, pager_object_list);
	} else {
		/*
		 * Gain a reference to the object.
		 */
		vm_object_reference(object);
		if (OFF_TO_IDX(foff + size) > object->size)
			object->size = OFF_TO_IDX(foff + size);
	}

	dev_pager_alloc_lock = 0;
	if (dev_pager_alloc_lock_want)
		wakeup(&dev_pager_alloc_lock);

	return (object);
}

static void
dev_pager_dealloc(vm_object_t object)
{
	vm_page_t m;

	TAILQ_REMOVE(&dev_pager_object_list, object, pager_object_list);
	/*
	 * Free up our fake pages.
	 */
	while ((m = TAILQ_FIRST(&object->un_pager.devp.devp_pglist)) != 0) {
		TAILQ_REMOVE(&object->un_pager.devp.devp_pglist, m, pageq);
		dev_pager_putfake(m);
	}
}

static int
dev_pager_getpages(vm_object_t object, vm_page_t *m, int count, int reqpage)
{
	vm_offset_t offset;
	vm_paddr_t paddr;
	vm_page_t page;
	cdev_t dev;
	int prot;
	int i;

	dev = object->handle;
	offset = m[reqpage]->pindex;
	prot = PROT_READ;	/* XXX should pass in? */

	paddr = pmap_phys_address(dev_dmmap(dev, (vm_offset_t) offset << PAGE_SHIFT, prot));
	KASSERT(paddr != -1,("dev_pager_getpage: map function returns error"));

	if (m[reqpage]->flags & PG_FICTITIOUS) {
		/*
		 * If the passed in reqpage page is a fake page, update it
		 * with the new physical address.
		 */
		m[reqpage]->phys_addr = paddr;
	} else {
		/*
		 * Replace the passed in reqpage page with our own fake page
		 * and free up all the original pages.
		 */
		page = dev_pager_getfake(paddr);
		TAILQ_INSERT_TAIL(&object->un_pager.devp.devp_pglist, page, pageq);
		crit_enter();
		vm_page_free(m[reqpage]);
		vm_page_insert(page, object, offset);
		crit_exit();
	}
	for (i = 0; i < count; i++) {
		if (i != reqpage)
			vm_page_free(m[i]);
	}
	return (VM_PAGER_OK);
}

static void
dev_pager_putpages(vm_object_t object, vm_page_t *m, int count, boolean_t sync,
    int *rtvals)
{
	panic("dev_pager_putpage called");
}

static boolean_t
dev_pager_haspage(vm_object_t object, vm_pindex_t pindex, int *before,
    int *after)
{
	if (before != NULL)
		*before = 0;
	if (after != NULL)
		*after = 0;
	return (TRUE);
}

static vm_page_t
dev_pager_getfake(vm_paddr_t paddr)
{
	vm_page_t m;

	if ((m = TAILQ_FIRST(&dev_freepages_list)) != NULL) {
		TAILQ_REMOVE(&dev_freepages_list, m, pageq);
	} else {
		m = kmalloc(sizeof(*m), M_FICTITIOUS_PAGES, M_WAITOK);
	}
	bzero(m, sizeof(*m));

	m->flags = PG_BUSY | PG_FICTITIOUS;
	m->valid = VM_PAGE_BITS_ALL;
	m->dirty = 0;
	m->busy = 0;
	m->queue = PQ_NONE;
	m->object = NULL;

	m->wire_count = 1;
	m->hold_count = 0;
	m->phys_addr = paddr;

	return (m);
}

/*
 * Synthesized VM pages must be structurally stable for lockless lookups to
 * work properly.
 */
static void
dev_pager_putfake(vm_page_t m)
{
	if (!(m->flags & PG_FICTITIOUS))
		panic("dev_pager_putfake: bad page");
	KKASSERT(m->object == NULL);
	TAILQ_INSERT_HEAD(&dev_freepages_list, m, pageq);
}

