/*
 * Copyright (c) 2007 The DragonFly Project.  All rights reserved.
 * 
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $DragonFly: src/sbin/hammer/hammer.c,v 1.44 2008/11/13 02:04:27 dillon Exp $
 */

#include "hammer.h"
#include <signal.h>
#include <math.h>

static void hammer_parsedevs(const char *blkdevs);
static void sigalrm(int signo);
static void usage(int exit_code);

int RecurseOpt;
int VerboseOpt;
int QuietOpt;
int NoSyncOpt;
int TwoWayPipeOpt;
int TimeoutOpt;
int DelayOpt = 5;
u_int64_t BandwidthOpt;
const char *CyclePath;
const char *LinkPath;

int
main(int ac, char **av)
{
	char *blkdevs = NULL;
	char *ptr;
	u_int32_t status;
	int ch;

	while ((ch = getopt(ac, av, "b:c:dhf:i:qrs:t:v2")) != -1) {
		switch(ch) {
		case '2':
			TwoWayPipeOpt = 1;
			break;
		case 'b':
			BandwidthOpt = strtoull(optarg, &ptr, 0);
			switch(*ptr) {
			case 'g':
			case 'G':
				BandwidthOpt *= 1024;
				/* fall through */
			case 'm':
			case 'M':
				BandwidthOpt *= 1024;
				/* fall through */
			case 'k':
			case 'K':
				BandwidthOpt *= 1024;
				break;
			default:
				usage(1);
			}
			break;
		case 'c':
			CyclePath = optarg;
			break;
		case 'd':
			++DebugOpt;
			break;
		case 'h':
			usage(0);
			/* not reached */
		case 'i':
			DelayOpt = strtol(optarg, NULL, 0);
			break;
		case 'r':
			RecurseOpt = 1;
			break;
		case 'f':
			blkdevs = optarg;
			break;
		case 's':
			LinkPath = optarg;
			break;
		case 't':
			TimeoutOpt = strtol(optarg, NULL, 0);
			break;
		case 'v':
			if (QuietOpt > 0)
				--QuietOpt;
			else
				++VerboseOpt;
			break;
		case 'q':
			if (VerboseOpt > 0)
				--VerboseOpt;
			else
				++QuietOpt;
			break;
		default:
			usage(1);
			/* not reached */
		}
	}
	ac -= optind;
	av += optind;
	if (ac < 1) {
		usage(1);
		/* not reached */
	}

	signal(SIGALRM, sigalrm);

	if (strcmp(av[0], "synctid") == 0) {
		hammer_cmd_synctid(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "namekey2") == 0) {
		int64_t key;
		int32_t crcx;
		int len;
		const char *aname = av[1];

		if (aname == NULL)
			usage(1);
		len = strlen(aname);
		key = (u_int32_t)crc32(aname, len) & 0xFFFFFFFEU;

		switch(len) {
		default:
			crcx = crc32(aname + 3, len - 5);
			crcx = crcx ^ (crcx >> 6) ^ (crcx >> 12);
			key |= (int64_t)(crcx & 0x3F) << 42;
			/* fall through */
		case 5:
		case 4:
			/* fall through */
		case 3:
			key |= ((int64_t)(aname[2] & 0x1F) << 48);
			/* fall through */
		case 2:
			key |= ((int64_t)(aname[1] & 0x1F) << 53) |
			       ((int64_t)(aname[len-2] & 0x1F) << 37);
			/* fall through */
		case 1:
			key |= ((int64_t)(aname[0] & 0x1F) << 58) |
			       ((int64_t)(aname[len-1] & 0x1F) << 32);
			/* fall through */
		case 0:
			break;
		}
		if (key == 0)
			key |= 0x100000000LL;
		printf("0x%016llx\n", key);
		exit(0);
	}
	if (strcmp(av[0], "namekey1") == 0) {
		int64_t key;

		if (av[1] == NULL)
			usage(1);
		key = (int64_t)(crc32(av[1], strlen(av[1])) & 0x7FFFFFFF) << 32;
		if (key == 0)
			key |= 0x100000000LL;
		printf("0x%016llx\n", key);
		exit(0);
	}
	if (strcmp(av[0], "namekey32") == 0) {
		int32_t key;

		if (av[1] == NULL)
			usage(1);
		key = crc32(av[1], strlen(av[1])) & 0x7FFFFFFF;
		if (key == 0)
			++key;
		printf("0x%08x\n", key);
		exit(0);
	}
	if (strcmp(av[0], "pfs-status") == 0) {
		hammer_cmd_pseudofs_status(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "pfs-master") == 0) {
		hammer_cmd_pseudofs_create(av + 1, ac - 1, 0);
		exit(0);
	}
	if (strcmp(av[0], "pfs-slave") == 0) {
		hammer_cmd_pseudofs_create(av + 1, ac - 1, 1);
		exit(0);
	}
	if (strcmp(av[0], "pfs-update") == 0) {
		hammer_cmd_pseudofs_update(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "pfs-upgrade") == 0) {
		hammer_cmd_pseudofs_upgrade(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "pfs-downgrade") == 0) {
		hammer_cmd_pseudofs_downgrade(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "pfs-destroy") == 0) {
		hammer_cmd_pseudofs_destroy(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "status") == 0) {
		hammer_cmd_status(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "prune") == 0) {
		hammer_cmd_softprune(av + 1, ac - 1, 0);
		exit(0);
	}
	if (strcmp(av[0], "cleanup") == 0) {
		hammer_cmd_cleanup(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "prune-everything") == 0) {
		hammer_cmd_softprune(av + 1, ac - 1, 1);
		exit(0);
	}
	if (strcmp(av[0], "snapshot") == 0) {
		hammer_cmd_snapshot(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "bstats") == 0) {
		hammer_cmd_bstats(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "iostats") == 0) {
		hammer_cmd_iostats(av + 1, ac - 1);
		exit(0);
	}

	if (strncmp(av[0], "history", 7) == 0) {
		hammer_cmd_history(av[0] + 7, av + 1, ac - 1);
		exit(0);
	}
	if (strncmp(av[0], "reblock", 7) == 0) {
		signal(SIGINT, sigalrm);
		if (strcmp(av[0], "reblock") == 0)
			hammer_cmd_reblock(av + 1, ac - 1, -1);
		else if (strcmp(av[0], "reblock-btree") == 0)
			hammer_cmd_reblock(av + 1, ac - 1, HAMMER_IOC_DO_BTREE);
		else if (strcmp(av[0], "reblock-inodes") == 0)
			hammer_cmd_reblock(av + 1, ac - 1, HAMMER_IOC_DO_INODES);
		else if (strcmp(av[0], "reblock-dirs") == 0)
			hammer_cmd_reblock(av + 1, ac - 1, HAMMER_IOC_DO_DIRS);
		else if (strcmp(av[0], "reblock-data") == 0)
			hammer_cmd_reblock(av + 1, ac - 1, HAMMER_IOC_DO_DATA);
		else
			usage(1);
		exit(0);
	}
	if (strncmp(av[0], "mirror", 6) == 0) {
		if (strcmp(av[0], "mirror-read") == 0)
			hammer_cmd_mirror_read(av + 1, ac - 1, 0);
		else if (strcmp(av[0], "mirror-read-stream") == 0)
			hammer_cmd_mirror_read(av + 1, ac - 1, 1);
		else if (strcmp(av[0], "mirror-write") == 0)
			hammer_cmd_mirror_write(av + 1, ac - 1);
		else if (strcmp(av[0], "mirror-copy") == 0)
			hammer_cmd_mirror_copy(av + 1, ac - 1, 0);
		else if (strcmp(av[0], "mirror-stream") == 0)
			hammer_cmd_mirror_copy(av + 1, ac - 1, 1);
		else if (strcmp(av[0], "mirror-dump") == 0)
			hammer_cmd_mirror_dump();
		else
			usage(1);
		exit(0);
	}
	if (strcmp(av[0], "version") == 0) {
		hammer_cmd_get_version(av + 1, ac - 1);
		exit(0);
	}
	if (strcmp(av[0], "version-upgrade") == 0) {
		hammer_cmd_set_version(av + 1, ac - 1);
		exit(0);
	}

	uuid_name_lookup(&Hammer_FSType, "DragonFly HAMMER", &status);
	if (status != uuid_s_ok) {
		errx(1, "uuids file does not have the DragonFly "
			"HAMMER filesystem type");
	}

	if (strcmp(av[0], "show") == 0) {
		hammer_off_t node_offset = (hammer_off_t)-1;

		hammer_parsedevs(blkdevs);
		if (ac > 1)
			sscanf(av[1], "%llx", &node_offset);
		hammer_cmd_show(node_offset, 0, NULL, NULL);
		exit(0);
	}
	if (strcmp(av[0], "blockmap") == 0) {
		hammer_parsedevs(blkdevs);
		hammer_cmd_blockmap();
		exit(0);
	}
	usage(1);
	/* not reached */
	return(0);
}

static
void
hammer_parsedevs(const char *blkdevs)
{
	char *copy;
	char *volname;

	if (blkdevs == NULL) {
		errx(1, "A -f blkdev[:blkdev]* specification is required "
			"for this command");
	}

	copy = strdup(blkdevs);
	while ((volname = copy) != NULL) {
		if ((copy = strchr(copy, ':')) != NULL)
			*copy++ = 0;
		setup_volume(-1, volname, 0, O_RDONLY);
	}
}

static
void
sigalrm(int signo __unused)
{
	/* do nothing (interrupts HAMMER ioctl) */
}

static
void
usage(int exit_code)
{
	fprintf(stderr, 
		"hammer -h\n"
		"hammer [-2qrv] [-b bandwidth] [-c cyclefile] [-f blkdev[:blkdev]*]\n"
		"       [-i delay ] [-t seconds] command [argument ...]\n"
		"hammer synctid <filesystem> [quick]\n"
		"hammer bstats [interval]\n"
		"hammer iostats [interval]\n"
		"hammer history[@offset[,len]] <file> ...\n"
		"hammer -f blkdev[:blkdev]* [-r] show [offset]\n"
#if 0
		"hammer -f blkdev[:blkdev]* blockmap\n"
#endif
		"hammer namekey1 <path>\n"
		"hammer namekey2 <path>\n"
		"hammer cleanup [<filesystem> ...]\n"
		"hammer snapshot [<filesystem>] <snapshot-dir>\n"
		"hammer prune <softlink-dir>\n"
		"hammer prune-everything <filesystem>\n"
		"hammer reblock[-btree/inodes/dirs/data] "
			"<filesystem> [fill_percentage]\n"
		"hammer pfs-status <dirpath> ...\n"
		"hammer pfs-master <dirpath> [options]\n"
		"hammer pfs-slave <dirpath> [options]\n"
		"hammer pfs-update <dirpath> [options]\n"
		"hammer pfs-upgrade <dirpath>\n"
		"hammer pfs-downgrade <dirpath>\n"
		"hammer pfs-destroy <dirpath>\n"
		"hammer mirror-read <filesystem> [begin-tid]\n"
		"hammer mirror-read-stream <filesystem> [begin-tid]\n"
		"hammer mirror-write <filesystem>\n"
		"hammer mirror-dump\n"
		"hammer mirror-copy [[user@]host:]<filesystem>"
				  " [[user@]host:]<filesystem>\n"
		"hammer mirror-stream [[user@]host:]<filesystem>"
				    " [[user@]host:]<filesystem>\n"
		"hammer version <filesystem>\n"
		"hammer version-upgrade <filesystem> version# [force]\n"
	);
	exit(exit_code);
}

