#	from @(#)Makefile.inc	8.3 (Berkeley) 2/4/95
# $FreeBSD: src/lib/libc/stdlib/Makefile.inc,v 1.56 2008/10/17 08:30:20 netchild Exp $

# machine-independent stdlib sources
.PATH: ${.CURDIR}/../libc/${MACHINE_ARCH}/stdlib ${.CURDIR}/../libc/stdlib

CMAPS+=	${.CURDIR}/stdlib/Symbol.map

MISRCS+=a64l.c abort.c abs.c atexit.c \
	atof.c atoi.c atol.c atoll.c \
	bsearch.c cxa_thread_atexit_impl.c \
	div.c exit.c freezero.c \
	getenv.c getopt.c getopt_long.c \
	getsubopt.c hcreate.c heapsort.c imaxabs.c imaxdiv.c \
	insque.c l64a.c labs.c ldiv.c llabs.c lldiv.c lsearch.c \
	merge.c posix_fadvise.c ptsname.c qsort.c qsort_r.c quick_exit.c \
	radixsort.c rand.c random.c \
	reallocarray.c reallocf.c realpath.c recallocarray.c remque.c \
	set_constraint_handler_s.c strfmon.c strsuftoll.c strtoimax.c \
	strtol.c strtoll.c strtonum.c strtoq.c strtoul.c strtoull.c \
	strtoumax.c strtouq.c system.c tdelete.c tfind.c tsearch.c twalk.c

# dmalloc has a leak somewhere and is eating up too much
# memory in long-running programs, and also doesn't do cache
# coloring for large allocations.  Use nmalloc for now.
#
#.if ${MACHINE_ARCH} == "x86_64"
#MISRCS+= dmalloc.c
#.else
MISRCS+= nmalloc.c
#.endif

# machine-dependent stdlib sources
.if exists(${.CURDIR}/../libc/${MACHINE_ARCH}/stdlib/Makefile.inc)
.include "${.CURDIR}/../libc/${MACHINE_ARCH}/stdlib/Makefile.inc"
.endif

.if ${LIB} == "c"
MAN+=	a64l.3 abort.3 abs.3 alloca.3 atexit.3 atof.3 atoi.3 atol.3 \
	at_quick_exit.3 \
	bsearch.3 \
	div.3 exit.3 getenv.3 getopt.3 getopt_long.3 getsubopt.3 \
	hcreate.3 imaxabs.3 imaxdiv.3 insque.3 labs.3 ldiv.3 llabs.3 lldiv.3 \
	lsearch.3 malloc.3 memory.3 \
	posix_fadvise.3 posix_memalign.3 posix_openpt.3 ptsname.3 \
	qsort.3 quick_exit.3 \
	radixsort.3 rand.3 random.3 realpath.3 reallocarray.3 \
	set_constraint_handler_s.3 strfmon.3 strsuftoll.3 strtod.3 \
	strtol.3 strtonum.3 strtoul.3 system.3 \
	tsearch.3

MLINKS+=a64l.3 l64a.3 \
	a64l.3 l64a_r.3
MLINKS+=atof.3 atof_l.3
MLINKS+=atoi.3 atoi_l.3
MLINKS+=atol.3 atol_l.3 \
	atol.3 atoll.3 \
	atol.3 atoll_l.3
MLINKS+=exit.3 _Exit.3
MLINKS+=getenv.3 clearenv.3 \
	getenv.3 putenv.3 \
	getenv.3 setenv.3 \
	getenv.3 unsetenv.3
MLINKS+=getopt_long.3 getopt_long_only.3
MLINKS+=hcreate.3 hdestroy.3 \
	hcreate.3 hsearch.3
MLINKS+=insque.3 remque.3
MLINKS+=lsearch.3 lfind.3
MLINKS+=malloc.3 calloc.3 \
	malloc.3 free.3 \
	malloc.3 freezero.3 \
	malloc.3 realloc.3 \
	malloc.3 reallocf.3
MLINKS+=posix_memalign.3 aligned_alloc.3
MLINKS+=ptsname.3 grantpt.3 \
	ptsname.3 unlockpt.3
MLINKS+=qsort.3 heapsort.3 \
	qsort.3 mergesort.3 \
	qsort.3 qsort_r.3
MLINKS+=radixsort.3 sradixsort.3
MLINKS+=reallocarray.3 recallocarray.3
MLINKS+=rand.3 rand_r.3 \
	rand.3 srand.3 \
	rand.3 sranddev.3
MLINKS+=random.3 initstate.3 \
	random.3 setstate.3 \
	random.3 srandom.3 \
	random.3 srandomdev.3
MLINKS+=set_constraint_handler_s.3 abort_handler_s.3 \
	set_constraint_handler_s.3 ignore_handler_s.3
MLINKS+=strfmon.3 strfmon_l.3
MLINKS+=strsuftoll.3 strsuftollx.3
MLINKS+=strtod.3 strtod_l.3 \
	strtod.3 strtof.3 \
	strtod.3 strtof_l.3 \
	strtod.3 strtold.3 \
	strtod.3 strtold_l.3
MLINKS+=strtol.3 strtoimax.3 \
	strtol.3 strtoimax_l.3 \
	strtol.3 strtol_l.3 \
	strtol.3 strtoll.3 \
	strtol.3 strtoll_l.3 \
	strtol.3 strtoq.3
MLINKS+=strtoul.3 strtoul_l.3 \
	strtoul.3 strtoull.3 \
	strtoul.3 strtoull_l.3 \
	strtoul.3 strtoumax.3 \
	strtoul.3 strtoumax_l.3 \
	strtoul.3 strtouq.3
MLINKS+=tsearch.3 tdelete.3 \
	tsearch.3 tfind.3 \
	tsearch.3 twalk.3
.endif
