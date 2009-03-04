/*-
 * Copyright (c) 2002 Jake Burkholder
 * Copyright (c) 2004 Robert Watson
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
 * $FreeBSD: src/usr.bin/ktrdump/ktrdump.c,v 1.10 2005/05/21 09:55:06 ru Exp $
 * $DragonFly: src/usr.bin/ktrdump/ktrdump.c,v 1.13 2008/11/10 02:05:31 swildner Exp $
 */

#include <sys/cdefs.h>

#include <sys/types.h>
#include <sys/ktr.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/queue.h>

#include <err.h>
#include <fcntl.h>
#include <kvm.h>
#include <limits.h>
#include <nlist.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	SBUFLEN	128

struct ktr_buffer {
	struct ktr_entry *ents;
	int modified;
	int reset;
	int beg_idx;		/* Beginning index */
	int end_idx;		/* Ending index */
};

static struct nlist nl[] = {
	{ .n_name = "_ktr_version" },
	{ .n_name = "_ktr_entries" },
	{ .n_name = "_ktr_idx" },
	{ .n_name = "_ktr_buf" },
	{ .n_name = "_ncpus" },
	{ .n_name = NULL }
};
static struct nlist nl2[] = {
	{ .n_name = "_tsc_frequency" },
	{ .n_name = NULL }
};

static int cflag;
static int fflag;
static int iflag;
static int lflag;
static int nflag;
static int qflag;
static int rflag;
static int sflag;
static int tflag;
static int xflag;
static int pflag;
static int Mflag;
static int Nflag;
static double tsc_frequency;
static double correction_factor = 0.0;

static char corefile[PATH_MAX];
static char execfile[PATH_MAX];

static char errbuf[_POSIX2_LINE_MAX];
static int ncpus;
static kvm_t *kd;
static int entries_per_buf;
static int fifo_mask;

static void usage(void);
static int earliest_ts(struct ktr_buffer *);
static void print_header(FILE *, int);
static void print_entry(FILE *, int, int, struct ktr_entry *, u_int64_t *);
static struct ktr_info *kvm_ktrinfo(void *);
static const char *kvm_string(const char *);
static const char *trunc_path(const char *, int);
static void read_symbols(const char *);
static const char *address_to_symbol(void *);
static struct ktr_buffer *ktr_bufs_init(void);
static void get_indices(int *);
static void load_bufs(struct ktr_buffer *, struct ktr_entry **);
static void print_buf(FILE *, struct ktr_buffer *, int, u_int64_t *);
static void print_bufs_timesorted(FILE *, struct ktr_buffer *, u_int64_t *);


/*
 * Reads the ktr trace buffer from kernel memory and prints the trace entries.
 */
int
main(int ac, char **av)
{
	struct ktr_buffer *ktr_bufs;
	struct ktr_entry **ktr_kbuf;
	FILE *fo;
	int64_t tts;
	int *ktr_start_index;
	int version;
	int c;
	int n;

	/*
	 * Parse commandline arguments.
	 */
	fo = stdout;
	while ((c = getopt(ac, av, "acfinqrtxpslA:N:M:o:")) != -1) {
		switch (c) {
		case 'a':
			cflag = 1;
			iflag = 1;
			rflag = 1;
			xflag = 1;
			pflag = 1;
			rflag = 1;
			sflag = 1;
			break;
		case 'c':
			cflag = 1;
			break;
		case 'N':
			if (strlcpy(execfile, optarg, sizeof(execfile))
			    >= sizeof(execfile))
				errx(1, "%s: File name too long", optarg);
			Nflag = 1;
			break;
		case 'f':
			fflag = 1;
			break;
		case 'l':
			lflag = 1;
			break;
		case 'i':
			iflag = 1;
			break;
		case 'A':
			correction_factor = strtod(optarg, NULL);
			break;
		case 'M':
			if (strlcpy(corefile, optarg, sizeof(corefile))
			    >= sizeof(corefile))
				errx(1, "%s: File name too long", optarg);
			Mflag = 1;
			break;
		case 'n':
			nflag = 1;
			break;
		case 'o':
			if ((fo = fopen(optarg, "w")) == NULL)
				err(1, "%s", optarg);
			break;
		case 'p':
			pflag++;
			break;
		case 'q':
			qflag++;
			break;
		case 'r':
			rflag = 1;
			break;
		case 's':
			sflag = 1;	/* sort across the cpus */
			break;
		case 't':
			tflag = 1;
			break;
		case 'x':
			xflag = 1;
			break;
		case '?':
		default:
			usage();
		}
	}
	if (cflag + iflag + tflag + xflag + fflag + pflag == 0) {
		cflag = 1;
		iflag = 1;
		tflag = 1;
		pflag = 1;
	}
	if (correction_factor != 0.0 && (rflag == 0 || nflag)) {
		fprintf(stderr, "Correction factor can only be applied with -r and without -n\n");
		exit(1);
	}
	ac -= optind;
	av += optind;
	if (ac != 0)
		usage();

	/*
	 * Open our execfile and corefile, resolve needed symbols and read in
	 * the trace buffer.
	 */
	if ((kd = kvm_openfiles(Nflag ? execfile : NULL,
	    Mflag ? corefile : NULL, NULL, O_RDONLY, errbuf)) == NULL)
		errx(1, "%s", errbuf);
	if (kvm_nlist(kd, nl) != 0)
		errx(1, "%s", kvm_geterr(kd));
	if (kvm_read(kd, nl[0].n_value, &version, sizeof(version)) == -1)
		errx(1, "%s", kvm_geterr(kd));
	if (kvm_read(kd, nl[4].n_value, &ncpus, sizeof(ncpus)) == -1)
		errx(1, "%s", kvm_geterr(kd));
	ktr_start_index = malloc(sizeof(*ktr_start_index) * ncpus);
	if (version >= 3 && kvm_nlist(kd, nl2) == 0) {
		if (kvm_read(kd, nl2[0].n_value, &tts, sizeof(tts)) == -1)
			errx(1, "%s", kvm_geterr(kd));
		tsc_frequency = (double)tts;
	}
	if (version > KTR_VERSION)
		errx(1, "ktr version too high for us to handle");
	if (kvm_read(kd, nl[1].n_value, &entries_per_buf,
				sizeof(entries_per_buf)) == -1)
		errx(1, "%s", kvm_geterr(kd));
	fifo_mask = entries_per_buf - 1;

	printf("TSC frequency is %6.3f MHz\n", tsc_frequency / 1000000.0);

	ktr_kbuf = malloc(sizeof(*ktr_kbuf) * ncpus);

	if (nflag == 0)
		read_symbols(Nflag ? execfile : NULL);

	if (kvm_read(kd, nl[3].n_value, ktr_kbuf, sizeof(*ktr_kbuf) * ncpus) == -1)
		errx(1, "%s", kvm_geterr(kd));

	ktr_bufs = ktr_bufs_init();

	if (sflag) {
		u_int64_t last_timestamp = 0;
		do {
			load_bufs(ktr_bufs, ktr_kbuf);
			print_bufs_timesorted(fo, ktr_bufs, &last_timestamp);
			if (lflag)
				usleep(1000000 / 10);
		} while (lflag);
	} else {
		u_int64_t *last_timestamp = calloc(sizeof(u_int64_t), ncpus);
		do {
			load_bufs(ktr_bufs, ktr_kbuf);
			for (n = 0; n < ncpus; ++n)
				print_buf(fo, ktr_bufs, n, &last_timestamp[n]);
			if (lflag)
				usleep(1000000 / 10);
		} while (lflag);
	}
	return (0);
}

static void
print_header(FILE *fo, int row)
{
	if (qflag == 0 && (u_int32_t)row % 20 == 0) {
		fprintf(fo, "%-6s ", "index");
		if (cflag)
			fprintf(fo, "%-3s ", "cpu");
		if (tflag || rflag)
			fprintf(fo, "%-16s ", "timestamp");
		if (xflag) {
			if (nflag)
			    fprintf(fo, "%-10s %-10s", "caller2", "caller1");
			else
			    fprintf(fo, "%-20s %-20s", "caller2", "caller1");
		}
		if (iflag)
			fprintf(fo, "%-20s ", "ID");
		if (fflag)
			fprintf(fo, "%10s%-30s ", "", "file and line");
		if (pflag)
			fprintf(fo, "%s", "trace");
		fprintf(fo, "\n");
	}
}

static void
print_entry(FILE *fo, int n, int row, struct ktr_entry *entry,
	    u_int64_t *last_timestamp)
{
	struct ktr_info *info = NULL;

	fprintf(fo, " %06x ", row & 0x00FFFFFF);
	if (cflag)
		fprintf(fo, "%-3d ", n);
	if (tflag || rflag) {
		if (rflag && !nflag && tsc_frequency != 0.0) {
			fprintf(fo, "%13.3f uS ",
				(double)(entry->ktr_timestamp - *last_timestamp) * 1000000.0 / tsc_frequency - correction_factor);
		} else if (rflag) {
			fprintf(fo, "%-16ju ",
			    (uintmax_t)(entry->ktr_timestamp - *last_timestamp));
		} else {
			fprintf(fo, "%-16ju ",
			    (uintmax_t)entry->ktr_timestamp);
		}
	}
	if (xflag) {
		if (nflag) {
		    fprintf(fo, "%p %p ", 
			    entry->ktr_caller2, entry->ktr_caller1);
		} else {
		    fprintf(fo, "%-20s ", 
			    address_to_symbol(entry->ktr_caller2));
		    fprintf(fo, "%-20s ", 
			    address_to_symbol(entry->ktr_caller1));
		}
	}
	if (iflag) {
		info = kvm_ktrinfo(entry->ktr_info);
		if (info)
			fprintf(fo, "%-20s ", kvm_string(info->kf_name));
		else
			fprintf(fo, "%-20s ", "<empty>");
	}
	if (fflag)
		fprintf(fo, "%34s:%-4d ", trunc_path(kvm_string(entry->ktr_file), 34), entry->ktr_line);
	if (pflag) {
		if (info == NULL)
			info = kvm_ktrinfo(entry->ktr_info);
		if (info)
			vfprintf(fo, kvm_string(info->kf_format), (void *)&entry->ktr_data);
	}
	fprintf(fo, "\n");
	*last_timestamp = entry->ktr_timestamp;
}

static
struct ktr_info *
kvm_ktrinfo(void *kptr)
{
	static struct ktr_info save_info;
	static void *save_kptr;

	if (kptr == NULL)
		return(NULL);
	if (save_kptr != kptr) {
		if (kvm_read(kd, (uintptr_t)kptr, &save_info, sizeof(save_info)) == -1) {
			bzero(&save_info, sizeof(save_info));
		} else {
			save_kptr = kptr;
		}
	}
	return(&save_info);
}

static
const char *
kvm_string(const char *kptr)
{
	static char save_str[128];
	static const char *save_kptr;
	u_int l;
	u_int n;

	if (kptr == NULL)
		return("?");
	if (save_kptr != kptr) {
		save_kptr = kptr;
		l = 0;
		while (l < sizeof(save_str) - 1) {
			n = 256 - ((intptr_t)(kptr + l) & 255);
			if (n > sizeof(save_str) - l - 1)
				n = sizeof(save_str) - l - 1;
			if (kvm_read(kd, (uintptr_t)(kptr + l), save_str + l, n) < 0)
				break;
			while (l < sizeof(save_str) && n) {
			    if (save_str[l] == 0)
				    break;
			    --n;
			    ++l;
			}
			if (n)
			    break;
		}
		save_str[l] = 0;
	}
	return(save_str);
}

static
const char *
trunc_path(const char *str, int maxlen)
{
	int len = strlen(str);

	if (len > maxlen)
		return(str + len - maxlen);
	else
		return(str);
}

struct symdata {
	TAILQ_ENTRY(symdata) link;
	const char *symname;
	char *symaddr;
	char symtype;
};

static TAILQ_HEAD(symlist, symdata) symlist;
static struct symdata *symcache;
static char *symbegin;
static char *symend;

static
void
read_symbols(const char *file)
{
	char buf[256];
	char cmd[256];
	size_t buflen = sizeof(buf);
	FILE *fp;
	struct symdata *sym;
	char *s1;
	char *s2;
	char *s3;

	TAILQ_INIT(&symlist);

	if (file == NULL) {
		if (sysctlbyname("kern.bootfile", buf, &buflen, NULL, 0) < 0)
			file = "/boot/kernel";
		else
			file = buf;
	}
	snprintf(cmd, sizeof(cmd), "nm -n %s", file);
	if ((fp = popen(cmd, "r")) != NULL) {
		while (fgets(buf, sizeof(buf), fp) != NULL) {
		    s1 = strtok(buf, " \t\n");
		    s2 = strtok(NULL, " \t\n");
		    s3 = strtok(NULL, " \t\n");
		    if (s1 && s2 && s3) {
			sym = malloc(sizeof(struct symdata));
			sym->symaddr = (char *)strtoul(s1, NULL, 16);
			sym->symtype = s2[0];
			sym->symname = strdup(s3);
			if (strcmp(s3, "kernbase") == 0)
				symbegin = sym->symaddr;
			if (strcmp(s3, "end") == 0)
				symend = sym->symaddr;
			TAILQ_INSERT_TAIL(&symlist, sym, link);
		    }
		}
		pclose(fp);
	}
	symcache = TAILQ_FIRST(&symlist);
}

static
const char *
address_to_symbol(void *kptr)
{
	static char buf[64];

	if (symcache == NULL ||
	   (char *)kptr < symbegin || (char *)kptr >= symend
	) {
		snprintf(buf, sizeof(buf), "%p", kptr);
		return(buf);
	}
	while ((char *)symcache->symaddr < (char *)kptr) {
		if (TAILQ_NEXT(symcache, link) == NULL)
			break;
		symcache = TAILQ_NEXT(symcache, link);
	}
	while ((char *)symcache->symaddr > (char *)kptr) {
		if (symcache != TAILQ_FIRST(&symlist))
			symcache = TAILQ_PREV(symcache, symlist, link);
	}
	snprintf(buf, sizeof(buf), "%s+%d", symcache->symname,
		(int)((char *)kptr - symcache->symaddr));
	return(buf);
}

static
struct ktr_buffer *
ktr_bufs_init(void)
{
	struct ktr_buffer *ktr_bufs, *it;
	int i;

	ktr_bufs = malloc(sizeof(*ktr_bufs) * ncpus);
	if (!ktr_bufs)
		err(1, "can't allocate data structures\n");
	for (i = 0; i < ncpus; ++i) {
		it = ktr_bufs + i;
		it->ents = malloc(sizeof(struct ktr_entry) * entries_per_buf);
		if (it->ents == NULL)
			err(1, "can't allocate data structures\n");
		it->reset = 1;
		it->beg_idx = -1;
		it->end_idx = -1;
	}
	return ktr_bufs;
}

static
void
get_indices(int *idx)
{
	if (kvm_read(kd, nl[2].n_value, idx, sizeof(*idx) * ncpus) == -1)
		errx(1, "%s", kvm_geterr(kd));
}

/*
 * Get the trace buffer data from the kernel
 */
static
void
load_bufs(struct ktr_buffer *ktr_bufs, struct ktr_entry **kbufs)
{
	static int *kern_idx;
	struct ktr_buffer *kbuf;
	int i;

	if (!kern_idx) {
		kern_idx = malloc(sizeof(*kern_idx) * ncpus);
		if (!kern_idx) {
			err(1, "can't allocate data structures\n");
		}
	}

	get_indices(kern_idx);
	for (i = 0; i < ncpus; ++i) {
		kbuf = &ktr_bufs[i];
		if (kern_idx[i] == kbuf->end_idx)
			continue;
		kbuf->end_idx = kern_idx[i];

		/*
		 * If we do not have a notion of the beginning index, assume
		 * it is entries_per_buf before the ending index.  Don't
		 * worry about underflows/negative numbers, the indices will
		 * be masked.
		 */
		if (kbuf->reset) {
			kbuf->beg_idx = kbuf->end_idx - entries_per_buf + 1;
			kbuf->reset = 0;
		}
		if (kvm_read(kd, (uintptr_t)kbufs[i], ktr_bufs[i].ents,
				sizeof(struct ktr_entry) * entries_per_buf)
									== -1)
			errx(1, "%s", kvm_geterr(kd));
		kbuf->modified = 1;
		kbuf->beg_idx = earliest_ts(kbuf);
	}

}

/*
 * Locate the earliest timestamp iterating backwards from end_idx, but
 * not going further back then beg_idx.  We have to do this because
 * the kernel uses a circulating buffer.
 */
static
int
earliest_ts(struct ktr_buffer *buf)
{
	struct ktr_entry *save;
	int count, scan, i, earliest;

	count = 0;
	earliest = buf->end_idx - 1;
	save = &buf->ents[earliest & fifo_mask];
	for (scan = buf->end_idx - 1; scan != buf->beg_idx -1; --scan) {
		i = scan & fifo_mask;
		if (buf->ents[i].ktr_timestamp <= save->ktr_timestamp)
			earliest = scan;
		/*
		 * We may have gotten so far behind that beg_idx wrapped
		 * more then once around the buffer.  Just stop
		 */
		if (++count == entries_per_buf)
			break;
	}
	return earliest;
}

static
void
print_buf(FILE *fo, struct ktr_buffer *ktr_bufs, int cpu,
	  u_int64_t *last_timestamp)
{
	struct ktr_buffer *buf = ktr_bufs + cpu;

	if (buf->modified == 0)
		return;
	if (*last_timestamp == 0) {
		*last_timestamp =
			buf->ents[buf->beg_idx & fifo_mask].ktr_timestamp;
	}
	while (buf->beg_idx != buf->end_idx) {
		print_header(fo, buf->beg_idx);
		print_entry(fo, cpu, buf->beg_idx,
			    &buf->ents[buf->beg_idx & fifo_mask],
			    last_timestamp);
		++buf->beg_idx;
	}
	buf->modified = 0;
}

static
void
print_bufs_timesorted(FILE *fo, struct ktr_buffer *ktr_bufs,
		      u_int64_t *last_timestamp)
{
	struct ktr_entry *ent;
	struct ktr_buffer *buf;
	int n, bestn;
	u_int64_t ts;
	static int row = 0;

	for (;;) {
		ts = 0;
		bestn = -1;
		for (n = 0; n < ncpus; ++n) {
			buf = ktr_bufs + n;
			if (buf->beg_idx == buf->end_idx)
				continue;
			ent = &buf->ents[buf->beg_idx & fifo_mask];
			if (ts == 0 || (ts >= ent->ktr_timestamp)) {
				ts = ent->ktr_timestamp;
				bestn = n;
			}
		}
		if ((bestn < 0) || (ts < *last_timestamp))
			break;
		buf = ktr_bufs + bestn;
		print_header(fo, row);
		print_entry(fo, bestn, row,
			    &buf->ents[buf->beg_idx & fifo_mask],
			    last_timestamp);
		++buf->beg_idx;
		*last_timestamp = ts;
		++row;
	}
}

static void
usage(void)
{
	fprintf(stderr, "usage: ktrdump [-acfilnpqrstx] [-A factor] [-N execfile] "
			"[-M corefile] [-o outfile]\n");
	exit(1);
}
