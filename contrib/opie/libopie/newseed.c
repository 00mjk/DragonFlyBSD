/* newseed.c: The opienewseed() library function.

%%% copyright-cmetz-96
This software is Copyright 1996-2001 by Craig Metz, All Rights Reserved.
The Inner Net License Version 3 applies to this software.
You should have received a copy of the license with this software. If
you didn't get a copy, you may request one from <license@inner.net>.

	History:

	Modified by cmetz for OPIE 2.4. Greatly simplified increment. Now does
		not add digits. Reformatted the code.
	Modified by cmetz for OPIE 2.32. Added syslog.h if DEBUG.
	Modified by cmetz for OPIE 2.31. Added time.h.
	Created by cmetz for OPIE 2.22.

$FreeBSD: src/contrib/opie/libopie/newseed.c,v 1.2.6.2 2002/07/15 14:48:47 des Exp $
$DragonFly: src/contrib/opie/libopie/newseed.c,v 1.2 2003/06/17 04:24:05 dillon Exp $
*/

#include "opie_cfg.h"
#if HAVE_TIME_H
#include <time.h>
#endif /* HAVE_TIME_H */
#if HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */
#include <ctype.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#if HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif /* HAVE_SYS_UTSNAME_H */
#include <errno.h>
#if DEBUG
#include <syslog.h>
#endif /* DEBUG */
#include "opie.h"

int opienewseed FUNCTION((seed), char *seed)
{
	if (!seed)
		return -1;

	if (seed[0]) {
		char *c, *end;
		unsigned int i, max;
		size_t slen;

		if ((slen = strlen(seed)) > OPIE_SEED_MAX)
			slen = OPIE_SEED_MAX;

		for (c = end = seed + slen - 1, max = 1;
				(c >= seed) && isdigit(*c); c--)
			max *= 10;

		/* c either points before seed or to an alpha, so skip */
		++c;

		/* keep alphas, only look at numbers */
		slen -= c - seed;

		if ((i = strtoul(c, (char **)0, 10)) < max) {
			if (++i >= max)
				i = 1;

			/*
			 * If we roll over, we will have to generate a
			 * seed which is at least as long as the previous one
			 * was.  snprintf() will add a NUL character as well.
			 */
			snprintf(c, slen + 1, "%0*d", slen, i);
			seed[OPIE_SEED_MAX] = 0;
			return 0;
		}
	}

	{
		time_t now;

		time(&now);
		srand(now);
	}

	{
		struct utsname utsname;

		if (uname(&utsname) < 0) {
#if DEBUG
			syslog(LOG_DEBUG, "uname: %s(%d)", strerror(errno),
				errno);
#endif /* DEBUG */
			utsname.nodename[0] = 'k';
			utsname.nodename[1] = 'e';
		}
		utsname.nodename[2] = 0;

		if (snprintf(seed, OPIE_SEED_MAX+1, "%s%04d", utsname.nodename,
				(rand() % 9999) + 1) >= OPIE_SEED_MAX+1)
			return -1;
		return 0;
	}
}

