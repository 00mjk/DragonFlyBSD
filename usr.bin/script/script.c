/*
 * Copyright (c) 1980, 1992, 1993
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
 * @(#) Copyright (c) 1980, 1992, 1993 The Regents of the University of California.  All rights reserved.
 * @(#)script.c	8.1 (Berkeley) 6/6/93
 * $FreeBSD: /usr/local/www/cvsroot/FreeBSD/src/usr.bin/script/script.c,v 1.11.2.2 2004/03/13 09:21:00 cperciva Exp $
 * $DragonFly: src/usr.bin/script/script.c,v 1.7 2005/03/01 20:05:14 cpressey Exp $
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static FILE	*fscript;
static int	master, slave;
static pid_t	child;
static const char *fname;
static int	qflg, ttyflg;

struct	termios tt;

static void	done(int) __dead2;
static void	doshell(char **);
static void	fail(void);
static void	finish(void);
static void	usage(void);

int
main(int argc, char **argv)
{
	int cc;
	struct termios rtt, stt;
	struct winsize win;
	int aflg, kflg, ch, n;
	struct timeval tv, *tvp;
	time_t tvec, start;
	char obuf[BUFSIZ];
	char ibuf[BUFSIZ];
	fd_set rfd;
	int flushtime = 30;

	aflg = kflg = 0;
	while ((ch = getopt(argc, argv, "aqkt:")) != -1) {
		switch(ch) {
		case 'a':
			aflg = 1;
			break;
		case 'q':
			qflg = 1;
			break;
		case 'k':
			kflg = 1;
			break;
		case 't':
			flushtime = atoi(optarg);
			if (flushtime < 0)
				err(1, "invalid flush time %d", flushtime);
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc > 0) {
		fname = argv[0];
		argv++;
		argc--;
	} else
		fname = "typescript";

	if ((fscript = fopen(fname, aflg ? "a" : "w")) == NULL)
		err(1, "%s", fname);

	if ((ttyflg = isatty(STDIN_FILENO)) != 0) {
		if (tcgetattr(STDIN_FILENO, &tt) == -1)
			err(1, "tcgetattr");
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &win) == -1)
			err(1, "ioctl");
		if (openpty(&master, &slave, NULL, &tt, &win) == -1)
			err(1, "openpty");
	} else {
		if (openpty(&master, &slave, NULL, NULL, NULL) == -1)
			err(1, "openpty");
	}

	if (!qflg) {
		tvec = time(NULL);
		printf("Script started, output file is %s\n", fname);
		fprintf(fscript, "Script started on %s", ctime(&tvec));
		fflush(fscript);
	}

	if (ttyflg) {
		rtt = tt;
		cfmakeraw(&rtt);
		rtt.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &rtt);
	}

	child = fork();
	if (child < 0) {
		warn("fork");
		done(1);
	}
	if (child == 0)
		doshell(argv);

	if (flushtime > 0)
		tvp = &tv;
	else
		tvp = NULL;

	start = time(0);
	FD_ZERO(&rfd);
	for (;;) {
		FD_SET(master, &rfd);
		FD_SET(STDIN_FILENO, &rfd);
		if (flushtime > 0) {
			tv.tv_sec = flushtime;
			tv.tv_usec = 0;
		}
		n = select(master + 1, &rfd, 0, 0, tvp);
		if (n < 0 && errno != EINTR)
			break;
		if (n > 0 && FD_ISSET(STDIN_FILENO, &rfd)) {
			cc = read(STDIN_FILENO, ibuf, sizeof(ibuf));
			if (cc < 0)
				break;
			if (cc == 0)
				write(master, ibuf, 0);
			if (cc > 0) {
				write(master, ibuf, cc);
				if (kflg && tcgetattr(master, &stt) >= 0 &&
				    ((stt.c_lflag & ECHO) == 0)) {
					fwrite(ibuf, 1, cc, fscript);
				}
			}
		}
		if (n > 0 && FD_ISSET(master, &rfd)) {
			cc = read(master, obuf, sizeof(obuf));
			if (cc <= 0)
				break;
			write(STDOUT_FILENO, obuf, cc);
			fwrite(obuf, 1, cc, fscript);
		}
		tvec = time(0);
		if (tvec - start >= flushtime) {
			fflush(fscript);
			start = tvec;
		}
	}
	finish();
	done(0);
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: script [-a] [-q] [-k] [-t time] [file] [command]\n");
	exit(1);
}

static void
finish(void)
{
	int die, e;
	pid_t pid;
	union wait status;

	die = e = 0;
	while ((pid = wait3((int *)&status, WNOHANG, NULL)) > 0) {
	        if (pid == child) {
			die = 1;
			if (WIFEXITED(status))
				e = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				e = WTERMSIG(status);
			else /* can't happen */
				e = 1;
		}
	}

	if (die)
		done(e);
}

static void
doshell(char **av)
{
	const char *shell;

	shell = getenv("SHELL");
	if (shell == NULL)
		shell = _PATH_BSHELL;

	close(master);
	fclose(fscript);
	login_tty(slave);
	if (av[0] != NULL) {
		execvp(av[0], av);
		warn("%s", av[0]);
	} else {
		execl(shell, shell, "-i", NULL);
		warn("%s", shell);
	}
	fail();
}

static void
fail(void)
{
	kill(0, SIGTERM);
	done(1);
}

static void
done(int eno)
{
	time_t tvec;

	if (ttyflg)
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &tt);
	tvec = time(NULL);
	if (!qflg) {
		fprintf(fscript,"\nScript done on %s", ctime(&tvec));
		printf("\nScript done, output file is '%s'\n", fname);
	}
	fclose(fscript);
	close(master);
	exit(eno);
}
