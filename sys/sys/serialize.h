/*
 * Provides a fast serialization facility that will serialize across blocking
 * conditions.  This facility is very similar to a lock but much faster for
 * the common case.  It utilizes the atomic_intr_*() functions to acquire
 * and release the serializer and token functions to block.
 *
 * This API is designed to be used whenever low level serialization is
 * required.  Unlike tokens this serialization is not safe from deadlocks
 * nor is it recursive, and care must be taken when using it. 
 *
 * $DragonFly: src/sys/sys/serialize.h,v 1.8 2008/05/05 12:35:03 sephe Exp $
 */

#ifndef _SYS_SERIALIZE_H_
#define _SYS_SERIALIZE_H_

#ifndef _MACHINE_STDINT_H_
#include <machine/stdint.h>
#endif

struct thread;

struct lwkt_serialize {
    __atomic_intr_t	interlock;
    struct thread	*last_td;
    unsigned int	sleep_cnt;
    unsigned int	tryfail_cnt;
    unsigned int	enter_cnt;
    unsigned int	try_cnt;
};

/*
 * Note that last_td is only maintained when INVARIANTS is turned on,
 * so this check is only useful as part of a [K]KASSERT.
 */
#define ASSERT_SERIALIZED(ss)		KKASSERT((ss)->last_td == curthread)
#define ASSERT_NOT_SERIALIZED(ss)	KKASSERT((ss)->last_td != curthread)

typedef struct lwkt_serialize *lwkt_serialize_t;

void lwkt_serialize_init(lwkt_serialize_t);
void lwkt_serialize_enter(lwkt_serialize_t);
#ifdef SMP
void lwkt_serialize_adaptive_enter(lwkt_serialize_t);
#endif
int lwkt_serialize_try(lwkt_serialize_t);
void lwkt_serialize_exit(lwkt_serialize_t);
void lwkt_serialize_handler_disable(lwkt_serialize_t);
void lwkt_serialize_handler_enable(lwkt_serialize_t);
void lwkt_serialize_handler_call(lwkt_serialize_t, void (*)(void *, void *), void *, void *);
int lwkt_serialize_handler_try(lwkt_serialize_t, void (*)(void *, void *), void *, void *);

#endif
