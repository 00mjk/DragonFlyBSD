/*-
 * Copyright (c) 2000 Mitsaru Iwasaki
 * Copyright (c) 2000 Michael Smith
 * Copyright (c) 2000 BSDi
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
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/dev/acpica/Osd/OsdMemory.c,v 1.11 2004/04/14 03:39:08 njl Exp $
 * $DragonFly: src/sys/dev/acpica5/Osd/OsdMemory.c,v 1.4 2006/09/05 00:55:36 dillon Exp $
 */

/*
 * 6.2 : Memory Management
 */

#include "acpi.h"

#include <sys/kernel.h>
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/pmap.h>

MALLOC_DEFINE(M_ACPICA, "acpica", "ACPI CA memory pool");

struct acpi_memtrack {
    struct acpi_memtrack *next;
    void *base;
    ACPI_SIZE size;
};

typedef struct acpi_memtrack *acpi_memtrack_t;

static acpi_memtrack_t acpi_mapbase;

void *
AcpiOsAllocate(ACPI_SIZE Size)
{
    return (kmalloc(Size, M_ACPICA, M_INTWAIT));
}

void
AcpiOsFree(void *Memory)
{
    kfree(Memory, M_ACPICA);
}

ACPI_STATUS
AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS PhysicalAddress, ACPI_SIZE Length,
    void **LogicalAddress)
{
    acpi_memtrack_t track;

    *LogicalAddress = pmap_mapdev((vm_offset_t)PhysicalAddress, Length);
    if (*LogicalAddress == NULL)
	return(AE_BAD_ADDRESS);
    else {
	track = kmalloc(sizeof(struct acpi_memtrack), M_ACPICA, M_INTWAIT);
	track->next = acpi_mapbase;
	track->base = *LogicalAddress;
	track->size = Length;
	acpi_mapbase = track;
    }
    return(AE_OK);
}

void
AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE Length)
{
    struct acpi_memtrack **ptrack;
    acpi_memtrack_t track;

again:
    for (ptrack = &acpi_mapbase; (track = *ptrack); ptrack = &track->next) {
	/*
	 * Exact match, degenerate case
	 */
	if (track->base == LogicalAddress && track->size == Length) {
	    *ptrack = track->next;
	    pmap_unmapdev((vm_offset_t)track->base, track->size);
	    kfree(track, M_ACPICA);
	    return;
	}
	/*
	 * Completely covered
	 */
	if ((char *)LogicalAddress <= (char *)track->base &&
	    (char *)LogicalAddress + Length >= (char *)track->base + track->size
	) {
	    *ptrack = track->next;
	    pmap_unmapdev((vm_offset_t)track->base, track->size);
	    printf("AcpiOsUnmapMemory: Warning, deallocation request too"
		   " large! %p/%08x (actual was %p/%08x)\n",
		   LogicalAddress, Length,
		   track->base, track->size);
	    kfree(track, M_ACPICA);
	    goto again;
	}

	/*
	 * Overlapping
	 */
	if ((char *)LogicalAddress + Length >= (char *)track->base &&
	    (char *)LogicalAddress < (char *)track->base + track->size
	) {
	    printf("AcpiOsUnmapMemory: Warning, deallocation did not "
		   "track allocation: %p/%08x (actual was %p/%08x)\n",
		   LogicalAddress, Length,
		   track->base, track->size);
	}
    }
    printf("AcpiOsUnmapMemory: Warning, broken ACPI, bad unmap: %p/%08x\n",
	LogicalAddress, Length);
}

ACPI_STATUS
AcpiOsGetPhysicalAddress(void *LogicalAddress,
    ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
{
    /* We can't necessarily do this, so cop out. */
    return (AE_BAD_ADDRESS);
}

/*
 * There is no clean way to do this.  We make the charitable assumption
 * that callers will not pass garbage to us.
 */
BOOLEAN
AcpiOsReadable (void *Pointer, ACPI_SIZE Length)
{
    return (TRUE);
}

BOOLEAN
AcpiOsWritable (void *Pointer, ACPI_SIZE Length)
{
    return (TRUE);
}

ACPI_STATUS
AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT32 *Value, UINT32 Width)
{
    void	*LogicalAddress;

    if (AcpiOsMapMemory(Address, Width / 8, &LogicalAddress) != AE_OK)
	return (AE_NOT_EXIST);

    switch (Width) {
    case 8:
	*(u_int8_t *)Value = (*(volatile u_int8_t *)LogicalAddress);
	break;
    case 16:
	*(u_int16_t *)Value = (*(volatile u_int16_t *)LogicalAddress);
	break;
    case 32:
	*(u_int32_t *)Value = (*(volatile u_int32_t *)LogicalAddress);
	break;
    case 64:
	*(u_int64_t *)Value = (*(volatile u_int64_t *)LogicalAddress);
	break;
    default:
	/* debug trap goes here */
	break;
    }

    AcpiOsUnmapMemory(LogicalAddress, Width / 8);

    return (AE_OK);
}

ACPI_STATUS
AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT32 Value, UINT32 Width)
{
    void	*LogicalAddress;

    if (AcpiOsMapMemory(Address, Width / 8, &LogicalAddress) != AE_OK)
	return (AE_NOT_EXIST);

    switch (Width) {
    case 8:
	(*(volatile u_int8_t *)LogicalAddress) = Value & 0xff;
	break;
    case 16:
	(*(volatile u_int16_t *)LogicalAddress) = Value & 0xffff;
	break;
    case 32:
	(*(volatile u_int32_t *)LogicalAddress) = Value & 0xffffffff;
	break;
    case 64:
	(*(volatile u_int64_t *)LogicalAddress) = Value;
	break;
    default:
	/* debug trap goes here */
	break;
    }

    AcpiOsUnmapMemory(LogicalAddress, Width / 8);

    return (AE_OK);
}
