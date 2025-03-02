/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
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
 *	@(#)stdlib.h	8.5 (Berkeley) 5/19/95
 * $FreeBSD: src/include/stdlib.h,v 1.67 2008/07/22 11:40:42 ache Exp $
 */

#ifndef _STDLIB_H_
#define	_STDLIB_H_

#include <sys/cdefs.h>
#include <sys/_null.h>
#include <sys/types.h>
#ifndef __cplusplus
#include <machine/wchar.h>		/* for ___wchar_t */
#endif

#ifndef _SIZE_T_DECLARED
typedef	__size_t	size_t;		/* _GCC_SIZE_T OK */
#define	_SIZE_T_DECLARED
#endif

#ifndef	__cplusplus
#ifndef _WCHAR_T_DECLARED
typedef	___wchar_t	wchar_t;	/* _GCC_WCHAR_T OK */
#define	_WCHAR_T_DECLARED
#endif
#endif

#if __EXT1_VISIBLE
#ifndef _ERRNO_T_DECLARED
typedef	int		errno_t;
#define	_ERRNO_T_DECLARED
#endif
#endif

typedef struct {
	int	quot;		/* quotient */
	int	rem;		/* remainder */
} div_t;

typedef struct {
	long	quot;
	long	rem;
} ldiv_t;

#define	EXIT_FAILURE	1
#define	EXIT_SUCCESS	0

#define	RAND_MAX	0x7fffffff

__BEGIN_DECLS
#ifdef _XLOCALE_H_
#include <xlocale/_stdlib.h>
#endif
extern int __mb_cur_max;
extern int ___mb_cur_max(void);
#define	MB_CUR_MAX	((size_t)___mb_cur_max())

void	 abort(void) __dead2;
/* void	 abort2(const char *, int, void **) __dead2; */
#if !defined(_KERNEL_VIRTUAL)
int	 abs(int) __pure2;
#endif
int	 atexit(void (*)(void));
double	 atof(const char *);
int	 atoi(const char *);
long	 atol(const char *);
void	*bsearch(const void *, const void *, size_t,
		 size_t, int (*)(const void *, const void *));
void	*calloc(size_t, size_t) __alloc_size2(1, 2) __malloclike __heedresult;
div_t	 div(int, int) __pure2;
void	 exit(int) __dead2;
void	 free(void *);
char	*getenv(const char *);
#if !defined(_KERNEL_VIRTUAL)
long	 labs(long) __pure2;
#endif
ldiv_t	 ldiv(long, long) __pure2;
void	*malloc(size_t) __malloclike __heedresult __alloc_size(1);
int	 mblen(const char *, size_t);
size_t	 mbstowcs(wchar_t * __restrict , const char * __restrict, size_t);
int	 mbtowc(wchar_t * __restrict, const char * __restrict, size_t);
void	 qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int	 rand(void);
void	*realloc(void *, size_t) __heedresult __alloc_size(2);
void	 srand(unsigned);
double	 strtod(const char * __restrict, char ** __restrict);
float	 strtof(const char * __restrict, char ** __restrict);
#if !defined(_KERNEL_VIRTUAL)
long	 strtol(const char * __restrict, char ** __restrict, int);
#endif
long double
	 strtold(const char * __restrict, char ** __restrict);
#if !defined(_KERNEL_VIRTUAL)
unsigned long
	 strtoul(const char * __restrict, char ** __restrict, int);
#endif
int	 system(const char *);
int	 wctomb(char *, wchar_t);
size_t	 wcstombs(char * __restrict, const wchar_t * __restrict, size_t);

/*
 * Functions added in C99 which we make conditionally available in the
 * BSD^C89 namespace if the compiler supports `long long'.
 * The #if test is more complicated than it ought to be because
 * __BSD_VISIBLE implies __ISO_C_VISIBLE == 1999 *even if* `long long'
 * is not supported in the compilation environment (which therefore means
 * that it can't really be ISO C99).
 *
 * (The only other extension made by C99 in this header is _Exit().)
 */
#if __ISO_C_VISIBLE >= 1999 || defined(__cplusplus)
#ifdef __LONG_LONG_SUPPORTED
/* LONGLONG */
typedef struct {
	long long quot;
	long long rem;
} lldiv_t;

/* LONGLONG */
long long
	 atoll(const char *);
/* LONGLONG */
long long
	 llabs(long long) __pure2;
/* LONGLONG */
lldiv_t	 lldiv(long long, long long) __pure2;
/* LONGLONG */
long long
	 strtoll(const char * __restrict, char ** __restrict, int);
/* LONGLONG */
unsigned long long
	 strtoull(const char * __restrict, char ** __restrict, int);
#endif /* __LONG_LONG_SUPPORTED */

void	 _Exit(int) __dead2;
#endif /* __ISO_C_VISIBLE >= 1999 */

/*
 * C11 functions.
 */
#if __ISO_C_VISIBLE >= 2011 || (defined(__cplusplus) && __cplusplus >= 201103L)
void	*aligned_alloc(size_t, size_t) __malloclike __heedresult
	    __alloc_align(1) __alloc_size(2);
int	at_quick_exit(void (*)(void));	/* extra extern case for __cplusplus? */
void	quick_exit(int) __dead2;
#endif /* __ISO_C_VISIBLE >= 2011 */

/*
 * Extensions made by POSIX relative to C.
 */
#if __POSIX_VISIBLE >= 199506
int	 rand_r(unsigned *);			/* (TSF) */
#endif
#if __POSIX_VISIBLE >= 200112
int	 posix_memalign(void **, size_t, size_t) __nonnull(1); /* (ADV) */
int	 setenv(const char *, const char *, int);
int	 unsetenv(const char *);
#endif

#if __POSIX_VISIBLE >= 200809 || __XSI_VISIBLE
int	 getsubopt(char **, char *const *, char **);
#ifndef _MKDTEMP_DECLARED
char	*mkdtemp(char *);
#define	_MKDTEMP_DECLARED
#endif
#ifndef _MKSTEMP_DECLARED
int	 mkstemp(char *);
#define	_MKSTEMP_DECLARED
#endif
#endif /* __POSIX_VISIBLE >= 200809 || __XSI_VISIBLE */

/*
 * The only changes to the XSI namespace in revision 6 were the deletion
 * of the ttyslot() and valloc() functions, which we never declared
 * in this header.  For revision 7, ecvt(), fcvt(), and gcvt(), which
 * we also do not have, and mktemp(), are to be deleted.
 */
#if __XSI_VISIBLE
/* XXX XSI requires pollution from <sys/wait.h> here.  We'd rather not. */
long	 a64l(const char *);
double	 drand48(void);
double	 erand48(unsigned short[3]);
#if 0
#if __BSD_VISIBLE || (__XSI_VISIBLE && __XSI_VISIBLE < 700)
char	*ecvt(double, int, int * __restrict, int * __restrict);	/* LEGACY */
char	*fcvt(double, int, int * __restrict, int * __restrict);	/* LEGACY */
char	*gcvt(double, int, int * __restrict, int * __restrict);	/* LEGACY */
#endif
#endif
int	 grantpt(int);
char	*initstate(unsigned long /* XSI requires u_int */, char *, long);
long	 jrand48(unsigned short[3]);
char	*l64a(long);
void	 lcong48(unsigned short[7]);
long	 lrand48(void);
#if __BSD_VISIBLE || (__XSI_VISIBLE && __XSI_VISIBLE < 700)
#if !defined(_MKTEMP_DECLARED)
char	*mktemp(char *);					/* LEGACY */
#define	_MKTEMP_DECLARED
#endif
#endif
long	 mrand48(void);
long	 nrand48(unsigned short[3]);
int	 posix_openpt(int);
char	*ptsname(int);
int	 putenv(char *);
long	 random(void);
char	*realpath(const char * __restrict, char * __restrict);
unsigned short
	*seed48(unsigned short[3]);
int	 setkey(const char *);
char	*setstate(/* const */ char *);
void	 srand48(long);
void	 srandom(unsigned long);
int	 unlockpt(int);
#endif /* __XSI_VISIBLE */

#if __BSD_VISIBLE
/* extern const char *_malloc_options;
extern void (*_malloc_message)(const char *, const char *, const char *,
	    const char *); */

/*
 * The alloca() function can't be implemented in C, and on some
 * platforms it can't be implemented at all as a callable function.
 * The GNU C compiler provides a built-in alloca() which we can use.
 * On platforms where alloca() is not in libc, programs which use it
 * will fail to link when compiled with non-GNU compilers.
 */
#if __GNUC__ >= 2
#undef  alloca	/* some GNU bits try to get cute and define this on their own */
#define alloca(sz) __builtin_alloca(sz)
#endif

__uint32_t
	 arc4random(void);
void	 arc4random_addrandom(__uint8_t *, size_t);
void	 arc4random_buf(void *, size_t);
void	 arc4random_stir(void);
__uint32_t
	 arc4random_uniform(__uint32_t);
char	*getbsize(int *, long *);

/* getcap(3) functions */
char	*cgetcap(char *, const char *, int);
int	 cgetclose(void);
int	 cgetent(char **, char **, const char *);
int	 cgetfirst(char **, char **);
int	 cgetmatch(const char *, const char *);
int	 cgetnext(char **, char **);
int	 cgetnum(char *, const char *, long *);
int	 cgetset(const char *);
int	 cgetstr(char *, const char *, char **);
int	 cgetustr(char *, const char *, char **);

int	 clearenv(void);
int	 daemon(int, int);
char	*devname(dev_t, mode_t);
char	*devname_r(dev_t, mode_t, char *, size_t);
char	*fdevname(int);
int	 fdevname_r(int, char *, size_t);
void	 freezero(void *, size_t);
int	 getloadavg(double [], int);
const char *
	 getprogname(void);

int	 heapsort(void *, size_t, size_t, int (*)(const void *, const void *));
int	 l64a_r(long, char *, int);
int	 mergesort(void *, size_t, size_t, int (*)(const void *, const void *));
int	 mkostemp(char *, int);
int	 mkostemps(char *, int, int);
void	 qsort_r(void *, size_t, size_t, void *,
		 int (*)(void *, const void *, const void *));
int	 radixsort(const unsigned char **, int, const unsigned char *,
		   unsigned int);
void	*reallocarray(void *, size_t, size_t) __heedresult __alloc_size2(2, 3);
void	*recallocarray(void *, size_t, size_t, size_t) __heedresult
	    __alloc_size2(3, 4);
void	*reallocf(void *, size_t) __heedresult __alloc_size(2);
int	 rpmatch(const char *);
void	 setprogname(const char *);
int	 sradixsort(const unsigned char **, int, const unsigned char *,
		    unsigned int);
void	 sranddev(void);
void	 srandomdev(void);
long long
	 strsuftoll(const char *, const char *, long long, long long);
long long
	 strsuftollx(const char *, const char *, long long, long long, char *,
	    size_t);
long long
	 strtonum(const char *, long long, long long, const char **);

/* Deprecated interfaces. */
#if !defined(_KERNEL_VIRTUAL)
__int64_t
	 strtoq(const char *, char **, int);
__uint64_t
	 strtouq(const char *, char **, int);
#endif

extern char *suboptarg;			/* getsubopt(3) external variable */
#endif /* __BSD_VISIBLE */

#if __EXT1_VISIBLE
/* K.3.6 */
typedef	void (*constraint_handler_t)(const char * __restrict,
    void * __restrict, errno_t);
/* K.3.6.1.1 */
constraint_handler_t set_constraint_handler_s(constraint_handler_t handler);
/* K.3.6.1.2 */
_Noreturn void abort_handler_s(const char * __restrict, void * __restrict,
    errno_t);
/* K3.6.1.3 */
void ignore_handler_s(const char * __restrict, void * __restrict, errno_t);
#endif /* __EXT1_VISIBLE */
__END_DECLS

#endif /* !_STDLIB_H_ */
