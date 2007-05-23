/*
 * SYS/MSGPORT.H
 *
 *	Implements LWKT messages and ports.
 * 
 * $DragonFly: src/sys/sys/msgport.h,v 1.23 2007/05/23 02:09:41 dillon Exp $
 */

#ifndef _SYS_MSGPORT_H_
#define _SYS_MSGPORT_H_

#ifndef _SYS_QUEUE_H_
#include <sys/queue.h>		/* TAILQ_* macros */
#endif
#ifndef _SYS_STDINT_H_
#include <sys/stdint.h>
#endif

#ifdef _KERNEL

#ifndef _SYS_MALLOC_H_
#include <sys/malloc.h>
#endif

#endif

struct lwkt_msg;
struct lwkt_port;
struct thread;

typedef struct lwkt_msg		*lwkt_msg_t;
typedef struct lwkt_port	*lwkt_port_t;

typedef TAILQ_HEAD(lwkt_msg_queue, lwkt_msg) lwkt_msg_queue;

/*
 * LWKT command message operator type.  This type holds a message's
 * 'command'.  The command format is opaque to the LWKT messaging system,
 * meaning that it is specific to whatever convention the API chooses.
 * By convention lwkt_cmd_t is passed by value and is expected to
 * efficiently fit into a machine register.
 */
typedef union lwkt_cmd {
    int		cm_op;
    int		(*cm_func)(lwkt_msg_t msg);
} lwkt_cmd_t;

/*
 * The standard message and port structure for communications between
 * threads.  See kern/lwkt_msgport.c for documentation on how messages and
 * ports work.
 *
 * For the most part a message may only be manipulated by whomever currently
 * owns it, which generally means the originating port if the message has
 * not been sent yet or has been replied, and the target port if the message
 * has been sent and/or is undergoing processing.
 *
 * The one exception to this rule is an abort.  Aborts must be initiated
 * by the originator and may 'chase' the target (especially if a message
 * is being forwarded), potentially even 'chase' the message all the way
 * back to the originator if it races against the target replying the
 * message.  The ms_abort_port field is the only field that may be modified
 * by the originator or intermediate target (when the abort is chasing
 * a forwarding or reply op).  An abort may cause a reply to be delayed
 * until the abort catches up to it.
 *
 * Messages which support an abort will have MSGF_ABORTABLE set, indicating
 * that the ms_abort field has been initialized.  An abort will cause a
 * message to be requeued to the target port so the target sees the same
 * message twice:  once during initial processing of the message, and a
 * second time to process the abort request.  lwkt_getport() will detect
 * the requeued abort and will copy ms_abort into ms_cmd before returning
 * the requeued message the second time.  This makes target processing a 
 * whole lot less complex.
 *
 * NOTE! 64-bit-align this structure.
 */
typedef struct lwkt_msg {
    TAILQ_ENTRY(lwkt_msg) ms_node;	/* link node */
    lwkt_port_t ms_target_port;		/* current target or relay port */
    lwkt_port_t	ms_reply_port;		/* async replies returned here */
    lwkt_port_t ms_abort_port;		/* abort chasing port */
    lwkt_cmd_t	ms_cmd;			/* message command operator */
    lwkt_cmd_t	ms_abort;		/* message abort operator */
    int		ms_flags;		/* message flags */
    int		ms_error;		/* positive error code or 0 */
    union {
	void	*ms_resultp;		/* misc pointer data or result */
	int	ms_result;		/* standard 'int'eger result */
	long	ms_lresult;		/* long result */
	int	ms_fds[2];		/* two int bit results */
	__int32_t ms_result32;		/* 32 bit result */
	__int64_t ms_result64;		/* 64 bit result */
	__off_t	ms_offset;		/* off_t result */
    } u;
    int		ms_pad[2];		/* future use */
} lwkt_msg;

#define MSGF_DONE	0x0001		/* asynch message is complete */
#define MSGF_REPLY1	0x0002		/* asynch message has been returned */
#define MSGF_QUEUED	0x0004		/* message has been queued sanitychk */
#define MSGF_ASYNC	0x0008		/* sync/async hint */
#define MSGF_ABORTED	0x0010		/* indicate pending abort */
#define MSGF_PCATCH	0x0020		/* catch proc signal while waiting */
#define MSGF_REPLY2	0x0040		/* reply processed by rport cpu */
#define MSGF_ABORTABLE	0x0080		/* message supports abort */
#define MSGF_RETRIEVED	0x0100		/* message retrieved on target */

#define MSG_CMD_CDEV	0x00010000
#define MSG_CMD_VFS	0x00020000
#define MSG_CMD_SYSCALL	0x00030000
#define MSG_SUBCMD_MASK	0x0000FFFF

#ifdef _KERNEL
MALLOC_DECLARE(M_LWKTMSG);
#endif

/*
 * Notes on port processing requirements:
 *
 * mp_putport():
 *	- may return synchronous error code (error != EASYNC) directly and
 *	  does not need to check or set MSGF_DONE if so, or set ms_target_port
 *	- for asynch procesing should clear MSGF_DONE and set ms_target_port
 *	  to port prior to initiation of the command.
 *
 * mp_waitport():
 *	- if the passed msg is NULL we wait for, remove, and return the
 *	  next pending message on the port.
 *	- if the passed msg is non-NULL we wait for that particular message,
 *	  which typically involves waiting until MSGF_DONE is set then
 *	  pulling the message off the port if MSGF_QUEUED is set and
 *	  returning it.  If MSGF_PCATCH is set in the message we allow
 *	  a signal to interrupt and abort the message.
 *
 * mp_replyport():
 *	- reply a message (executed on the originating port to return a
 *	  message to it).  This can be rather involved if abort is to be
 *	  supported, see lwkt_default_replyport().  Generally speaking
 *	  one sets MSGF_DONE.  If MSGF_ASYNC is set the message is queued
 *	  to the port, else the port's thread is scheduled.
 *
 * mp_abortport():
 *	- abort a message.  The mp_abortport function for the message's
 *	  ms_target_port is called.  ms_target_port is basically where
 *	  the message was sent to or last forwarded to.  Aborting a message
 *	  can be rather involved.  Note that the lwkt_getmsg() code ensures
 *	  that a message is returned non-abort before it is returned again
 *	  with its ms_cmd set to ms_abort, even if the abort occurs before
 *	  the initial retrieval of the message.  The setting of ms_cmd to
 *	  ms_abort is NOT handled by mp_abortport().  mp_abortport() is
 *	  basically responsible for requeueing the message to the target
 *	  port and setting the MSGF_ABORTED flag.
 *
 */
typedef struct lwkt_port {
    lwkt_msg_queue	mp_msgq;
    int			mp_flags;
    int			mp_unused01;
    struct thread	*mp_td;
    int			(*mp_putport)(lwkt_port_t, lwkt_msg_t);
    void *		(*mp_waitport)(lwkt_port_t, lwkt_msg_t);
    void		(*mp_replyport)(lwkt_port_t, lwkt_msg_t);
    void		(*mp_abortport)(lwkt_port_t, lwkt_msg_t);
} lwkt_port;

#define MSGPORTF_WAITING	0x0001

/*
 * These functions are good for userland as well as the kernel.  The 
 * messaging function support for userland is provided by the kernel's
 * kern/lwkt_msgport.c.  The port functions are provided by userland.
 */
void lwkt_initport(lwkt_port_t, struct thread *);
void lwkt_initport_null_rport(lwkt_port_t, struct thread *);
void lwkt_sendmsg(lwkt_port_t, lwkt_msg_t);
int lwkt_domsg(lwkt_port_t, lwkt_msg_t);
int lwkt_forwardmsg(lwkt_port_t, lwkt_msg_t);
void lwkt_abortmsg(lwkt_msg_t);
void *lwkt_getport(lwkt_port_t);

int lwkt_default_putport(lwkt_port_t port, lwkt_msg_t msg);
void *lwkt_default_waitport(lwkt_port_t port, lwkt_msg_t msg);
void lwkt_default_replyport(lwkt_port_t port, lwkt_msg_t msg);
void lwkt_default_abortport(lwkt_port_t port, lwkt_msg_t msg);
void lwkt_null_replyport(lwkt_port_t port, lwkt_msg_t msg);

#endif
