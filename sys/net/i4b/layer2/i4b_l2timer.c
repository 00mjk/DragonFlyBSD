/*
 * Copyright (c) 1997, 2000 Hellmuth Michaelis. All rights reserved.
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
 *---------------------------------------------------------------------------
 *
 *	i4b_l2timer.c - layer 2 timer handling
 *	--------------------------------------
 *
 *	$Id: i4b_l2timer.c,v 1.20 2000/08/24 11:48:58 hm Exp $ 
 *
 * $FreeBSD: src/sys/i4b/layer2/i4b_l2timer.c,v 1.6.2.1 2001/08/10 14:08:41 obrien Exp $
 * $DragonFly: src/sys/net/i4b/layer2/i4b_l2timer.c,v 1.7 2005/06/14 21:19:19 joerg Exp $
 *
 *      last edit-date: [Thu Aug 24 12:48:52 2000]
 *
 *---------------------------------------------------------------------------*/

#include "use_i4bq921.h"
#if NI4BQ921 > 0

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/socket.h>
#include <sys/thread2.h>
#include <net/if.h>

#include <net/i4b/include/machine/i4b_debug.h>
#include "../include/i4b_global.h"

#include "i4b_l2.h"
#include "i4b_l2fsm.h"

/*---------------------------------------------------------------------------*
 *	Q.921 timer T200 timeout function
 *---------------------------------------------------------------------------*/
static void
i4b_T200_timeout(l2_softc_t *l2sc)
{
	NDBGL2(L2_T_ERR, "unit %d, RC = %d", l2sc->unit, l2sc->RC);
	i4b_next_l2state(l2sc, EV_T200EXP);
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T200 start
 *---------------------------------------------------------------------------*/
void
i4b_T200_start(l2_softc_t *l2sc)
{
	if(l2sc->T200 == TIMER_ACTIVE)
		return;
		
	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
	l2sc->T200 = TIMER_ACTIVE;

	callout_reset(&l2sc->T200_timeout, T200DEF, 
			(void *)i4b_T200_timeout, l2sc);
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T200 stop
 *---------------------------------------------------------------------------*/
void
i4b_T200_stop(l2_softc_t *l2sc)
{
	CRIT_VAR;
	CRIT_BEG;
	if(l2sc->T200 != TIMER_IDLE)
	{
		callout_stop(&l2sc->T200_timeout);
		l2sc->T200 = TIMER_IDLE;
	}
	CRIT_END;
	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T200 restart
 *---------------------------------------------------------------------------*/
void
i4b_T200_restart(l2_softc_t *l2sc)
{
	CRIT_VAR;
	CRIT_BEG;
	if(l2sc->T200 != TIMER_IDLE)
	{
		callout_stop(&l2sc->T200_timeout);
	}
	else
	{
		l2sc->T200 = TIMER_ACTIVE;
	}

	callout_reset(&l2sc->T200_timeout, T200DEF, 
			(void *)i4b_T200_timeout, l2sc);
	CRIT_END;
	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T202 timeout function
 *---------------------------------------------------------------------------*/
static void
i4b_T202_timeout(l2_softc_t *l2sc)
{
	NDBGL2(L2_T_ERR, "unit %d, N202 = %d", l2sc->unit, l2sc->N202);
	
	if(--(l2sc->N202))
	{
		(*l2sc->T202func)(l2sc);
	}
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T202 start
 *---------------------------------------------------------------------------*/
void
i4b_T202_start(l2_softc_t *l2sc)
{
	if (l2sc->N202 == TIMER_ACTIVE)
		return;

	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
	l2sc->N202 = N202DEF;	
	l2sc->T202 = TIMER_ACTIVE;

	callout_reset(&l2sc->T202_timeout, T202DEF, 
			(void *)i4b_T202_timeout, l2sc);
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T202 stop
 *---------------------------------------------------------------------------*/
void
i4b_T202_stop(l2_softc_t *l2sc)
{
	CRIT_VAR;
	CRIT_BEG;
	if(l2sc->T202 != TIMER_IDLE)
	{
		callout_stop(&l2sc->T202_timeout);
		l2sc->T202 = TIMER_IDLE;
	}
	CRIT_END;
	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T203 timeout function
 *---------------------------------------------------------------------------*/
#if I4B_T203_ACTIVE
static void
i4b_T203_timeout(l2_softc_t *l2sc)
{
	NDBGL2(L2_T_ERR, "unit %d", l2sc->unit);
	i4b_next_l2state(l2sc, EV_T203EXP);
}
#endif

/*---------------------------------------------------------------------------*
 *	Q.921 timer T203 start
 *---------------------------------------------------------------------------*/
void
i4b_T203_start(l2_softc_t *l2sc)
{
#if I4B_T203_ACTIVE
	if (l2sc->T203 == TIMER_ACTIVE)
		return;
		
	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
	l2sc->T203 = TIMER_ACTIVE;

	callout_reset(&l2sc->T203_timeout, T203DEF, i4b_T203_timeout, l2sc);
#endif
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T203 stop
 *---------------------------------------------------------------------------*/
void
i4b_T203_stop(l2_softc_t *l2sc)
{
#if I4B_T203_ACTIVE
	CRIT_VAR;
	CRIT_BEG;
	if(l2sc->T203 != TIMER_IDLE)
	{
		callout_stop(&l2sc->T203_timeout);
		l2sc->T203 = TIMER_IDLE;
	}
	CRIT_END;
	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
#endif
}

/*---------------------------------------------------------------------------*
 *	Q.921 timer T203 restart
 *---------------------------------------------------------------------------*/
void
i4b_T203_restart(l2_softc_t *l2sc)
{
#if I4B_T203_ACTIVE
	CRIT_VAR;
	CRIT_BEG;

	if(l2sc->T203 != TIMER_IDLE)
	{
		callout_stop(&l2sc->T203_timeout);
	}
	else
	{
		l2sc->T203 = TIMER_ACTIVE;
	}

	callout_reset(&l2sc->T203_timeout, T203DEF, i4b_T203_timerout, l2sc);
	CRIT_END;
	NDBGL2(L2_T_MSG, "unit %d", l2sc->unit);
#endif
}

#endif /* NI4BQ921 > 0 */
