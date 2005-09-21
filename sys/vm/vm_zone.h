/*
 * Copyright (c) 1997, 1998 John S. Dyson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	notice immediately at the beginning of the file, without modification,
 *	this list of conditions, and the following disclaimer.
 * 2. Absolutely no warranty of function or purpose is made by the author
 *	John S. Dyson.
 *
 * $FreeBSD: src/sys/vm/vm_zone.h,v 1.13.2.2 2002/10/10 19:50:16 dillon Exp $
 * $DragonFly: src/sys/vm/vm_zone.h,v 1.7 2005/09/21 19:48:05 hsu Exp $
 */

#ifndef _SYS_ZONE_H
#define _SYS_ZONE_H

#define ZONE_INTERRUPT 0x0001	/* If you need to allocate at int time */
#define ZONE_PANICFAIL 0x0002	/* panic if the zalloc fails */
#define ZONE_SPECIAL   0x0004	/* special vm_map_entry zone, see zget() */
#define ZONE_BOOT      0x0010	/* Internal flag used by zbootinit */
#define ZONE_USE_RESERVE 0x0020	/* use reserve memory if necessary */

#include <sys/spinlock.h>
#include <sys/thread.h>

typedef struct vm_zone {
	struct spinlock zlock;		/* lock for data structure */
	void		*zitems;	/* linked list of items */
	int		zfreecnt;	/* free entries */
	int		zfreemin;	/* minimum number of free entries */
	int		znalloc;	/* number of allocations */
	vm_offset_t	zkva;		/* Base kva of zone */
	int		zpagecount;	/* Total # of allocated pages */
	int		zpagemax;	/* Max address space */
	int		zmax;		/* Max number of entries allocated */
	int		ztotal;		/* Total entries allocated now */
	int		zsize;		/* size of each entry */
	int		zalloc;		/* hint for # of pages to alloc */
	int		zflags;		/* flags for zone */
	int		zallocflag;	/* flag for allocation */
	struct vm_object *zobj;		/* object to hold zone */
	char		*zname;		/* name for diags */
	struct vm_zone	*znext;		/* list of zones for sysctl */
} *vm_zone_t;


void		zerror (int) __dead2;
vm_zone_t	zinit (char *name, int size, int nentries, int flags,
			   int zalloc);
int		zinitna (vm_zone_t z, struct vm_object *obj, char *name,
			     int size, int nentries, int flags, int zalloc);
void *		zalloc (vm_zone_t z);
void		zfree (vm_zone_t z, void *item);
void		zbootinit (vm_zone_t z, char *name, int size, void *item,
			       int nitems);

#endif
