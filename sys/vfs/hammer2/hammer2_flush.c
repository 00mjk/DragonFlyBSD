/*
 * Copyright (c) 2011-2015 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@dragonflybsd.org>
 * by Venkatesh Srinivas <vsrinivas@dragonflybsd.org>
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
 */
/*
 *			TRANSACTION AND FLUSH HANDLING
 *
 * Deceptively simple but actually fairly difficult to implement properly is
 * how I would describe it.
 *
 * Flushing generally occurs bottom-up but requires a top-down scan to
 * locate chains with MODIFIED and/or UPDATE bits set.  The ONFLUSH flag
 * tells how to recurse downward to find these chains.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/uuid.h>

#include "hammer2.h"

#define FLUSH_DEBUG 0

#define HAMMER2_FLUSH_DEPTH_LIMIT       10      /* stack recursion limit */


/*
 * Recursively flush the specified chain.  The chain is locked and
 * referenced by the caller and will remain so on return.  The chain
 * will remain referenced throughout but can temporarily lose its
 * lock during the recursion to avoid unnecessarily stalling user
 * processes.
 */
struct hammer2_flush_info {
	hammer2_chain_t *parent;
	int		depth;
	int		diddeferral;
	int		cache_index;
	int		flags;
	struct h2_flush_list flushq;
	hammer2_chain_t	*debug;
};

typedef struct hammer2_flush_info hammer2_flush_info_t;

static void hammer2_flush_core(hammer2_flush_info_t *info,
				hammer2_chain_t *chain, int flags);
static int hammer2_flush_recurse(hammer2_chain_t *child, void *data);

/*
 * Any per-pfs transaction initialization goes here.
 */
void
hammer2_trans_manage_init(hammer2_pfs_t *pmp)
{
}

/*
 * Transaction support for any modifying operation.  Transactions are used
 * in the pmp layer by the frontend and in the spmp layer by the backend.
 *
 * 0			- Normal transaction, interlocked against flush
 *			  transaction.
 *
 * TRANS_ISFLUSH	- Flush transaction, interlocked against normal
 *			  transaction.
 *
 * TRANS_BUFCACHE	- Buffer cache transaction, no interlock.
 *
 * Initializing a new transaction allocates a transaction ID.  Typically
 * passed a pmp (hmp passed as NULL), indicating a cluster transaction.  Can
 * be passed a NULL pmp and non-NULL hmp to indicate a transaction on a single
 * media target.  The latter mode is used by the recovery code.
 *
 * TWO TRANSACTION IDs can run concurrently, where one is a flush and the
 * other is a set of any number of concurrent filesystem operations.  We
 * can either have <running_fs_ops> + <waiting_flush> + <blocked_fs_ops>
 * or we can have <running_flush> + <concurrent_fs_ops>.
 *
 * During a flush, new fs_ops are only blocked until the fs_ops prior to
 * the flush complete.  The new fs_ops can then run concurrent with the flush.
 *
 * Buffer-cache transactions operate as fs_ops but never block.  A
 * buffer-cache flush will run either before or after the current pending
 * flush depending on its state.
 */
void
hammer2_trans_init(hammer2_pfs_t *pmp, uint32_t flags)
{
	uint32_t oflags;
	uint32_t nflags;
	int dowait;

	for (;;) {
		oflags = pmp->trans.flags;
		cpu_ccfence();
		dowait = 0;

		if (flags & HAMMER2_TRANS_ISFLUSH) {
			/*
			 * Requesting flush transaction.  Wait for all
			 * currently running transactions to finish.
			 */
			if (oflags & HAMMER2_TRANS_MASK) {
				nflags = oflags | HAMMER2_TRANS_FPENDING |
						  HAMMER2_TRANS_WAITING;
				dowait = 1;
			} else {
				nflags = (oflags | flags) + 1;
			}
		} else if (flags & HAMMER2_TRANS_BUFCACHE) {
			/*
			 * Requesting strategy transaction.  Generally
			 * allowed in all situations unless a flush
			 * is running without the preflush flag.
			 */
			if ((oflags & (HAMMER2_TRANS_ISFLUSH |
				       HAMMER2_TRANS_PREFLUSH)) ==
			    HAMMER2_TRANS_ISFLUSH) {
				nflags = oflags | HAMMER2_TRANS_WAITING;
				dowait = 1;
			} else {
				nflags = (oflags | flags) + 1;
			}
		} else {
			/*
			 * Requesting normal transaction.  Wait for any
			 * flush to finish before allowing.
			 */
			if (oflags & HAMMER2_TRANS_ISFLUSH) {
				nflags = oflags | HAMMER2_TRANS_WAITING;
				dowait = 1;
			} else {
				nflags = (oflags | flags) + 1;
			}
		}
		if (dowait)
			tsleep_interlock(&pmp->trans.sync_wait, 0);
		if (atomic_cmpset_int(&pmp->trans.flags, oflags, nflags)) {
			if (dowait == 0)
				break;
			tsleep(&pmp->trans.sync_wait, PINTERLOCKED,
			       "h2trans", hz);
		} else {
			cpu_pause();
		}
		/* retry */
	}
}

/*
 * Start a sub-transaction, there is no 'subdone' function.  This will
 * issue a new modify_tid (mtid) for the current transaction, which is a
 * CLC (cluster level change) id and not a per-node id.
 *
 * This function must be called for each XOP when multiple XOPs are run in
 * sequence within a transaction.
 *
 * Callers typically update the inode with the transaction mtid manually
 * to enforce sequencing.
 */
hammer2_tid_t
hammer2_trans_sub(hammer2_pfs_t *pmp)
{
	hammer2_tid_t mtid;

	mtid = atomic_fetchadd_64(&pmp->modify_tid, 1);

	return (mtid);
}

/*
 * Clears the PREFLUSH stage, called during a flush transaction after all
 * logical buffer I/O has completed.
 */
void
hammer2_trans_clear_preflush(hammer2_pfs_t *pmp)
{
	atomic_clear_int(&pmp->trans.flags, HAMMER2_TRANS_PREFLUSH);
}

void
hammer2_trans_done(hammer2_pfs_t *pmp)
{
	uint32_t oflags;
	uint32_t nflags;

	for (;;) {
		oflags = pmp->trans.flags;
		cpu_ccfence();
		KKASSERT(oflags & HAMMER2_TRANS_MASK);
		if ((oflags & HAMMER2_TRANS_MASK) == 1) {
			/*
			 * This was the last transaction
			 */
			nflags = (oflags - 1) & ~(HAMMER2_TRANS_ISFLUSH |
						  HAMMER2_TRANS_BUFCACHE |
						  HAMMER2_TRANS_PREFLUSH |
						  HAMMER2_TRANS_FPENDING |
						  HAMMER2_TRANS_WAITING);
		} else {
			/*
			 * Still transactions pending
			 */
			nflags = oflags - 1;
		}
		if (atomic_cmpset_int(&pmp->trans.flags, oflags, nflags)) {
			if ((nflags & HAMMER2_TRANS_MASK) == 0 &&
			    (oflags & HAMMER2_TRANS_WAITING)) {
				wakeup(&pmp->trans.sync_wait);
			}
			break;
		} else {
			cpu_pause();
		}
		/* retry */
	}
}

/*
 * Obtain new, unique inode number (not serialized by caller).
 */
hammer2_tid_t
hammer2_trans_newinum(hammer2_pfs_t *pmp)
{
	hammer2_tid_t tid;

	tid = atomic_fetchadd_64(&pmp->inode_tid, 1);

	return tid;
}

/*
 * Assert that a strategy call is ok here.  Strategy calls are legal
 *
 * (1) In a normal transaction.
 * (2) In a flush transaction only if PREFLUSH is also set.
 */
void
hammer2_trans_assert_strategy(hammer2_pfs_t *pmp)
{
	KKASSERT((pmp->trans.flags & HAMMER2_TRANS_ISFLUSH) == 0 ||
		 (pmp->trans.flags & HAMMER2_TRANS_PREFLUSH));
}


/*
 * Chains undergoing destruction are removed from the in-memory topology.
 * To avoid getting lost these chains are placed on the delayed flush
 * queue which will properly dispose of them.
 *
 * We do this instead of issuing an immediate flush in order to give
 * recursive deletions (rm -rf, etc) a chance to remove more of the
 * hierarchy, potentially allowing an enormous amount of write I/O to
 * be avoided.
 */
void
hammer2_delayed_flush(hammer2_chain_t *chain)
{
	if ((chain->flags & HAMMER2_CHAIN_DELAYED) == 0) {
		hammer2_spin_ex(&chain->hmp->list_spin);
		if ((chain->flags & (HAMMER2_CHAIN_DELAYED |
				     HAMMER2_CHAIN_DEFERRED)) == 0) {
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_DELAYED |
						      HAMMER2_CHAIN_DEFERRED);
			TAILQ_INSERT_TAIL(&chain->hmp->flushq,
					  chain, flush_node);
			hammer2_chain_ref(chain);
		}
		hammer2_spin_unex(&chain->hmp->list_spin);
	}
}

/*
 * Flush the chain and all modified sub-chains through the specified
 * synchronization point, propagating blockref updates back up.  As
 * part of this propagation, mirror_tid and inode/data usage statistics
 * propagates back upward.
 *
 * modify_tid (clc - cluster level change) is not propagated.
 *
 * update_tid (clc) is used for validation and is not propagated by this
 * function.
 *
 * This routine can be called from several places but the most important
 * is from VFS_SYNC (frontend) via hammer2_inode_xop_flush (backend).
 *
 * chain is locked on call and will remain locked on return.  The chain's
 * UPDATE flag indicates that its parent's block table (which is not yet
 * part of the flush) should be updated.  The chain may be replaced by
 * the call if it was modified.
 */
void
hammer2_flush(hammer2_chain_t *chain, int flags)
{
	hammer2_chain_t *scan;
	hammer2_flush_info_t info;
	hammer2_dev_t *hmp;
	int loops;

	/*
	 * Execute the recursive flush and handle deferrals.
	 *
	 * Chains can be ridiculously long (thousands deep), so to
	 * avoid blowing out the kernel stack the recursive flush has a
	 * depth limit.  Elements at the limit are placed on a list
	 * for re-execution after the stack has been popped.
	 */
	bzero(&info, sizeof(info));
	TAILQ_INIT(&info.flushq);
	info.cache_index = -1;
	info.flags = flags & ~HAMMER2_FLUSH_TOP;

	/*
	 * Calculate parent (can be NULL), if not NULL the flush core
	 * expects the parent to be referenced so it can easily lock/unlock
	 * it without it getting ripped up.
	 */
	if ((info.parent = chain->parent) != NULL)
		hammer2_chain_ref(info.parent);

	/*
	 * Extra ref needed because flush_core expects it when replacing
	 * chain.
	 */
	hammer2_chain_ref(chain);
	hmp = chain->hmp;
	loops = 0;

	for (;;) {
		/*
		 * Move hmp->flushq to info.flushq if non-empty so it can
		 * be processed.
		 */
		if (TAILQ_FIRST(&hmp->flushq) != NULL) {
			hammer2_spin_ex(&chain->hmp->list_spin);
			TAILQ_CONCAT(&info.flushq, &hmp->flushq, flush_node);
			hammer2_spin_unex(&chain->hmp->list_spin);
		}

		/*
		 * Unwind deep recursions which had been deferred.  This
		 * can leave the FLUSH_* bits set for these chains, which
		 * will be handled when we [re]flush chain after the unwind.
		 */
		while ((scan = TAILQ_FIRST(&info.flushq)) != NULL) {
			KKASSERT(scan->flags & HAMMER2_CHAIN_DEFERRED);
			TAILQ_REMOVE(&info.flushq, scan, flush_node);
			atomic_clear_int(&scan->flags, HAMMER2_CHAIN_DEFERRED |
						       HAMMER2_CHAIN_DELAYED);

			/*
			 * Now that we've popped back up we can do a secondary
			 * recursion on the deferred elements.
			 *
			 * NOTE: hammer2_flush() may replace scan.
			 */
			if (hammer2_debug & 0x0040)
				kprintf("deferred flush %p\n", scan);
			hammer2_chain_lock(scan, HAMMER2_RESOLVE_MAYBE);
			hammer2_flush(scan, flags & ~HAMMER2_FLUSH_TOP);
			hammer2_chain_unlock(scan);
			hammer2_chain_drop(scan);	/* ref from deferral */
		}

		/*
		 * [re]flush chain.
		 */
		info.diddeferral = 0;
		hammer2_flush_core(&info, chain, flags);

		/*
		 * Only loop if deep recursions have been deferred.
		 */
		if (TAILQ_EMPTY(&info.flushq))
			break;

		if (++loops % 1000 == 0) {
			kprintf("hammer2_flush: excessive loops on %p\n",
				chain);
			if (hammer2_debug & 0x100000)
				Debugger("hell4");
		}
	}
	hammer2_chain_drop(chain);
	if (info.parent)
		hammer2_chain_drop(info.parent);
}

/*
 * This is the core of the chain flushing code.  The chain is locked by the
 * caller and must also have an extra ref on it by the caller, and remains
 * locked and will have an extra ref on return.  Upon return, the caller can
 * test the UPDATE bit on the child to determine if the parent needs updating.
 *
 * (1) Determine if this node is a candidate for the flush, return if it is
 *     not.  fchain and vchain are always candidates for the flush.
 *
 * (2) If we recurse too deep the chain is entered onto the deferral list and
 *     the current flush stack is aborted until after the deferral list is
 *     run.
 *
 * (3) Recursively flush live children (rbtree).  This can create deferrals.
 *     A successful flush clears the MODIFIED and UPDATE bits on the children
 *     and typically causes the parent to be marked MODIFIED as the children
 *     update the parent's block table.  A parent might already be marked
 *     MODIFIED due to a deletion (whos blocktable update in the parent is
 *     handled by the frontend), or if the parent itself is modified by the
 *     frontend for other reasons.
 *
 * (4) Permanently disconnected sub-trees are cleaned up by the front-end.
 *     Deleted-but-open inodes can still be individually flushed via the
 *     filesystem syncer.
 *
 * (5) Note that an unmodified child may still need the block table in its
 *     parent updated (e.g. rename/move).  The child will have UPDATE set
 *     in this case.
 *
 *			WARNING ON BREF MODIFY_TID/MIRROR_TID
 *
 * blockref.modify_tid is consistent only within a PFS, and will not be
 * consistent during synchronization.  mirror_tid is consistent across the
 * block device regardless of the PFS.
 */
static void
hammer2_flush_core(hammer2_flush_info_t *info, hammer2_chain_t *chain,
		   int flags)
{
	hammer2_chain_t *parent;
	hammer2_dev_t *hmp;
	int diddeferral;

	/*
	 * (1) Optimize downward recursion to locate nodes needing action.
	 *     Nothing to do if none of these flags are set.
	 */
	if ((chain->flags & HAMMER2_CHAIN_FLUSH_MASK) == 0) {
		if (hammer2_debug & 0x200) {
			if (info->debug == NULL)
				info->debug = chain;
		} else {
			return;
		}
	}

	hmp = chain->hmp;
	diddeferral = info->diddeferral;
	parent = info->parent;		/* can be NULL */

	/*
	 * Downward search recursion
	 */
	if (chain->flags & (HAMMER2_CHAIN_DEFERRED | HAMMER2_CHAIN_DELAYED)) {
		/*
		 * Already deferred.
		 */
		++info->diddeferral;
	} else if ((chain->flags & HAMMER2_CHAIN_PFSBOUNDARY) &&
		   (flags & HAMMER2_FLUSH_ALL) == 0 &&
		   (flags & HAMMER2_FLUSH_TOP) == 0) {
		/*
		 * We do not recurse through PFSROOTs.  PFSROOT flushes are
		 * handled by the related pmp's (whether mounted or not,
		 * including during recovery).
		 *
		 * But we must still process the PFSROOT chains for block
		 * table updates in their parent (which IS part of our flush).
		 *
		 * Note that the volume root, vchain, does not set this flag.
		 * Note the logic here requires that this test be done before
		 * the depth-limit test, else it might become the top on a
		 * flushq iteration.
		 */
		;
	} else if (info->depth == HAMMER2_FLUSH_DEPTH_LIMIT) {
		/*
		 * Recursion depth reached.
		 */
		KKASSERT((chain->flags & HAMMER2_CHAIN_DELAYED) == 0);
		hammer2_chain_ref(chain);
		TAILQ_INSERT_TAIL(&info->flushq, chain, flush_node);
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_DEFERRED);
		++info->diddeferral;
	} else if (chain->flags & HAMMER2_CHAIN_ONFLUSH) {
		/*
		 * Downward recursion search (actual flush occurs bottom-up).
		 * pre-clear ONFLUSH.  It can get set again due to races,
		 * which we want so the scan finds us again in the next flush.
		 */
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_ONFLUSH);
		info->parent = chain;
		hammer2_spin_ex(&chain->core.spin);
		RB_SCAN(hammer2_chain_tree, &chain->core.rbtree,
			NULL, hammer2_flush_recurse, info);
		hammer2_spin_unex(&chain->core.spin);
		info->parent = parent;
		if (info->diddeferral)
			hammer2_chain_setflush(chain);
	}

	/*
	 * Now we are in the bottom-up part of the recursion.
	 *
	 * Do not update chain if lower layers were deferred.
	 */
	if (info->diddeferral)
		goto done;

	/*
	 * Propagate the DESTROY flag downwards.  This dummies up the flush
	 * code and tries to invalidate related buffer cache buffers to
	 * avoid the disk write.
	 */
	if (parent && (parent->flags & HAMMER2_CHAIN_DESTROY))
		atomic_set_int(&chain->flags, HAMMER2_CHAIN_DESTROY);

	/*
	 * Chain was already modified or has become modified, flush it out.
	 */
again:
	if ((hammer2_debug & 0x200) &&
	    info->debug &&
	    (chain->flags & (HAMMER2_CHAIN_MODIFIED | HAMMER2_CHAIN_UPDATE))) {
		hammer2_chain_t *scan = chain;

		kprintf("DISCONNECTED FLUSH %p->%p\n", info->debug, chain);
		while (scan) {
			kprintf("    chain %p [%08x] bref=%016jx:%02x\n",
				scan, scan->flags,
				scan->bref.key, scan->bref.type);
			if (scan == info->debug)
				break;
			scan = scan->parent;
		}
	}

	if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
		/*
		 * Dispose of the modified bit.
		 *
		 * UPDATE should already be set.
		 * bref.mirror_tid should already be set.
		 */
		KKASSERT((chain->flags & HAMMER2_CHAIN_UPDATE) ||
			 chain == &hmp->vchain);
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_MODIFIED);

		/*
		 * Manage threads waiting for excessive dirty memory to
		 * be retired.
		 */
		if (chain->pmp)
			hammer2_pfs_memory_wakeup(chain->pmp);

		if ((chain->flags & HAMMER2_CHAIN_UPDATE) ||
		    chain == &hmp->vchain ||
		    chain == &hmp->fchain) {
			/*
			 * Drop the ref from the MODIFIED bit we cleared,
			 * net -1 ref.
			 */
			hammer2_chain_drop(chain);
		} else {
			/*
			 * Drop the ref from the MODIFIED bit we cleared and
			 * set a ref for the UPDATE bit we are setting.  Net
			 * 0 refs.
			 */
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
		}

		/*
		 * Issue the flush.  This is indirect via the DIO.
		 *
		 * NOTE: A DELETED node that reaches this point must be
		 *	 flushed for synchronization point consistency.
		 *
		 * NOTE: Even though MODIFIED was already set, the related DIO
		 *	 might not be dirty due to a system buffer cache
		 *	 flush and must be set dirty if we are going to make
		 *	 further modifications to the buffer.  Chains with
		 *	 embedded data don't need this.
		 */
		if (hammer2_debug & 0x1000) {
			kprintf("Flush %p.%d %016jx/%d data=%016jx",
				chain, chain->bref.type,
				(uintmax_t)chain->bref.key,
				chain->bref.keybits,
				(uintmax_t)chain->bref.data_off);
		}
		if (hammer2_debug & 0x2000) {
			Debugger("Flush hell");
		}

		/*
		 * Update chain CRCs for flush.
		 *
		 * NOTE: Volume headers are NOT flushed here as they require
		 *	 special processing.
		 */
		switch(chain->bref.type) {
		case HAMMER2_BREF_TYPE_FREEMAP:
			/*
			 * Update the volume header's freemap_tid to the
			 * freemap's flushing mirror_tid.
			 *
			 * (note: embedded data, do not call setdirty)
			 */
			KKASSERT(hmp->vchain.flags & HAMMER2_CHAIN_MODIFIED);
			KKASSERT(chain == &hmp->fchain);
			hmp->voldata.freemap_tid = chain->bref.mirror_tid;
			if (hammer2_debug & 0x8000) {
				/* debug only, avoid syslogd loop */
				kprintf("sync freemap mirror_tid %08jx\n",
					(intmax_t)chain->bref.mirror_tid);
			}

			/*
			 * The freemap can be flushed independently of the
			 * main topology, but for the case where it is
			 * flushed in the same transaction, and flushed
			 * before vchain (a case we want to allow for
			 * performance reasons), make sure modifications
			 * made during the flush under vchain use a new
			 * transaction id.
			 *
			 * Otherwise the mount recovery code will get confused.
			 */
			++hmp->voldata.mirror_tid;
			break;
		case HAMMER2_BREF_TYPE_VOLUME:
			/*
			 * The free block table is flushed by
			 * hammer2_vfs_sync() before it flushes vchain.
			 * We must still hold fchain locked while copying
			 * voldata to volsync, however.
			 *
			 * (note: embedded data, do not call setdirty)
			 */
			hammer2_chain_lock(&hmp->fchain,
					   HAMMER2_RESOLVE_ALWAYS);
			hammer2_voldata_lock(hmp);
			if (hammer2_debug & 0x8000) {
				/* debug only, avoid syslogd loop */
				kprintf("sync volume  mirror_tid %08jx\n",
					(intmax_t)chain->bref.mirror_tid);
			}

			/*
			 * Update the volume header's mirror_tid to the
			 * main topology's flushing mirror_tid.  It is
			 * possible that voldata.mirror_tid is already
			 * beyond bref.mirror_tid due to the bump we made
			 * above in BREF_TYPE_FREEMAP.
			 */
			if (hmp->voldata.mirror_tid < chain->bref.mirror_tid) {
				hmp->voldata.mirror_tid =
					chain->bref.mirror_tid;
			}

			/*
			 * The volume header is flushed manually by the
			 * syncer, not here.  All we do here is adjust the
			 * crc's.
			 */
			KKASSERT(chain->data != NULL);
			KKASSERT(chain->dio == NULL);

			hmp->voldata.icrc_sects[HAMMER2_VOL_ICRC_SECT1]=
				hammer2_icrc32(
					(char *)&hmp->voldata +
					 HAMMER2_VOLUME_ICRC1_OFF,
					HAMMER2_VOLUME_ICRC1_SIZE);
			hmp->voldata.icrc_sects[HAMMER2_VOL_ICRC_SECT0]=
				hammer2_icrc32(
					(char *)&hmp->voldata +
					 HAMMER2_VOLUME_ICRC0_OFF,
					HAMMER2_VOLUME_ICRC0_SIZE);
			hmp->voldata.icrc_volheader =
				hammer2_icrc32(
					(char *)&hmp->voldata +
					 HAMMER2_VOLUME_ICRCVH_OFF,
					HAMMER2_VOLUME_ICRCVH_SIZE);

			if (hammer2_debug & 0x8000) {
				/* debug only, avoid syslogd loop */
				kprintf("syncvolhdr %016jx %016jx\n",
					hmp->voldata.mirror_tid,
					hmp->vchain.bref.mirror_tid);
			}
			hmp->volsync = hmp->voldata;
			atomic_set_int(&chain->flags, HAMMER2_CHAIN_VOLUMESYNC);
			hammer2_voldata_unlock(hmp);
			hammer2_chain_unlock(&hmp->fchain);
			break;
		case HAMMER2_BREF_TYPE_DATA:
			/*
			 * Data elements have already been flushed via the
			 * logical file buffer cache.  Their hash was set in
			 * the bref by the vop_write code.  Do not re-dirty.
			 *
			 * Make sure any device buffer(s) have been flushed
			 * out here (there aren't usually any to flush) XXX.
			 */
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
			/*
			 * Buffer I/O will be cleaned up when the volume is
			 * flushed (but the kernel is free to flush it before
			 * then, as well).
			 */
			KKASSERT((chain->flags & HAMMER2_CHAIN_EMBEDDED) == 0);
			hammer2_chain_setcheck(chain, chain->data);
			break;
		case HAMMER2_BREF_TYPE_INODE:
			/*
			 * NOTE: We must call io_setdirty() to make any late
			 *	 changes to the inode data, the system might
			 *	 have already flushed the buffer.
			 */
			if (chain->data->ipdata.meta.op_flags &
			    HAMMER2_OPFLAG_PFSROOT) {
				/*
				 * non-NULL pmp if mounted as a PFS.  We must
				 * sync fields cached in the pmp? XXX
				 */
				hammer2_inode_data_t *ipdata;

				hammer2_io_setdirty(chain->dio);
				ipdata = &chain->data->ipdata;
				if (chain->pmp) {
					ipdata->meta.pfs_inum =
						chain->pmp->inode_tid;
				}
			} else {
				/* can't be mounted as a PFS */
			}

			KKASSERT((chain->flags & HAMMER2_CHAIN_EMBEDDED) == 0);
			hammer2_chain_setcheck(chain, chain->data);
			break;
		default:
			KKASSERT(chain->flags & HAMMER2_CHAIN_EMBEDDED);
			panic("hammer2_flush_core: unsupported "
			      "embedded bref %d",
			      chain->bref.type);
			/* NOT REACHED */
		}

		/*
		 * If the chain was destroyed try to avoid unnecessary I/O.
		 * (this only really works if the DIO system buffer is the
		 * same size as chain->bytes).
		 */
		if ((chain->flags & HAMMER2_CHAIN_DESTROY) && chain->dio) {
			hammer2_io_setinval(chain->dio, chain->bytes);
		}
	}

	/*
	 * If UPDATE is set the parent block table may need to be updated.
	 *
	 * NOTE: UPDATE may be set on vchain or fchain in which case
	 *	 parent could be NULL.  It's easiest to allow the case
	 *	 and test for NULL.  parent can also wind up being NULL
	 *	 due to a deletion so we need to handle the case anyway.
	 *
	 * If no parent exists we can just clear the UPDATE bit.  If the
	 * chain gets reattached later on the bit will simply get set
	 * again.
	 */
	if ((chain->flags & HAMMER2_CHAIN_UPDATE) && parent == NULL) {
		atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
		hammer2_chain_drop(chain);
	}

	/*
	 * The chain may need its blockrefs updated in the parent.  This
	 * requires some fancy footwork.
	 */
	if (chain->flags & HAMMER2_CHAIN_UPDATE) {
		hammer2_blockref_t *base;
		int count;

		/*
		 * Both parent and chain must be locked.  This requires
		 * temporarily unlocking the chain.  We have to deal with
		 * the case where the chain might be reparented or modified
		 * while it was unlocked.
		 */
		hammer2_chain_unlock(chain);
		hammer2_chain_lock(parent, HAMMER2_RESOLVE_ALWAYS);
		hammer2_chain_lock(chain, HAMMER2_RESOLVE_MAYBE);
		if (chain->parent != parent) {
			kprintf("PARENT MISMATCH ch=%p p=%p/%p\n",
				chain, chain->parent, parent);
			hammer2_chain_unlock(parent);
			goto done;
		}

		/*
		 * Check race condition.  If someone got in and modified
		 * it again while it was unlocked, we have to loop up.
		 */
		if (chain->flags & HAMMER2_CHAIN_MODIFIED) {
			hammer2_chain_unlock(parent);
			kprintf("hammer2_flush: chain %p flush-mod race\n",
				chain);
			goto again;
		}

		/*
		 * Clear UPDATE flag, mark parent modified, update its
		 * modify_tid if necessary, and adjust the parent blockmap.
		 */
		if (chain->flags & HAMMER2_CHAIN_UPDATE) {
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_UPDATE);
			hammer2_chain_drop(chain);
		}

		/*
		 * (optional code)
		 *
		 * Avoid actually modifying and updating the parent if it
		 * was flagged for destruction.  This can greatly reduce
		 * disk I/O in large tree removals because the
		 * hammer2_io_setinval() call in the upward recursion
		 * (see MODIFIED code above) can only handle a few cases.
		 */
		if (parent->flags & HAMMER2_CHAIN_DESTROY) {
			if (parent->bref.modify_tid < chain->bref.modify_tid) {
				parent->bref.modify_tid =
					chain->bref.modify_tid;
			}
			atomic_clear_int(&chain->flags, HAMMER2_CHAIN_BMAPPED |
							HAMMER2_CHAIN_BMAPUPD);
			hammer2_chain_unlock(parent);
			goto skipupdate;
		}

		/*
		 * We are updating the parent's blockmap, the parent must
		 * be set modified.
		 */
		hammer2_chain_modify(parent, 0, 0, 0);
		if (parent->bref.modify_tid < chain->bref.modify_tid)
			parent->bref.modify_tid = chain->bref.modify_tid;

		/*
		 * Calculate blockmap pointer
		 */
		switch(parent->bref.type) {
		case HAMMER2_BREF_TYPE_INODE:
			/*
			 * Access the inode's block array.  However, there is
			 * no block array if the inode is flagged DIRECTDATA.
			 */
			if (parent->data &&
			    (parent->data->ipdata.meta.op_flags &
			     HAMMER2_OPFLAG_DIRECTDATA) == 0) {
				base = &parent->data->
					ipdata.u.blockset.blockref[0];
			} else {
				base = NULL;
			}
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
		case HAMMER2_BREF_TYPE_FREEMAP_NODE:
			if (parent->data)
				base = &parent->data->npdata[0];
			else
				base = NULL;
			count = parent->bytes / sizeof(hammer2_blockref_t);
			break;
		case HAMMER2_BREF_TYPE_VOLUME:
			base = &chain->hmp->voldata.sroot_blockset.blockref[0];
			count = HAMMER2_SET_COUNT;
			break;
		case HAMMER2_BREF_TYPE_FREEMAP:
			base = &parent->data->npdata[0];
			count = HAMMER2_SET_COUNT;
			break;
		default:
			base = NULL;
			count = 0;
			panic("hammer2_flush_core: "
			      "unrecognized blockref type: %d",
			      parent->bref.type);
		}

		/*
		 * Blocktable updates
		 *
		 * We synchronize pending statistics at this time.  Delta
		 * adjustments designated for the current and upper level
		 * are synchronized.
		 */
		if (base && (chain->flags & HAMMER2_CHAIN_BMAPUPD)) {
			if (chain->flags & HAMMER2_CHAIN_BMAPPED) {
				hammer2_spin_ex(&parent->core.spin);
				hammer2_base_delete(parent, base, count,
						    &info->cache_index, chain);
				hammer2_spin_unex(&parent->core.spin);
				/* base_delete clears both bits */
			} else {
				atomic_clear_int(&chain->flags,
						 HAMMER2_CHAIN_BMAPUPD);
			}
		}
		if (base && (chain->flags & HAMMER2_CHAIN_BMAPPED) == 0) {
			hammer2_spin_ex(&parent->core.spin);
			hammer2_base_insert(parent, base, count,
					    &info->cache_index, chain);
			hammer2_spin_unex(&parent->core.spin);
			/* base_insert sets BMAPPED */
		}
		hammer2_chain_unlock(parent);
	}
skipupdate:
	;

	/*
	 * Final cleanup after flush
	 */
done:
	KKASSERT(chain->refs > 0);
	if (hammer2_debug & 0x200) {
		if (info->debug == chain)
			info->debug = NULL;
	}
}

/*
 * Flush recursion helper, called from flush_core, calls flush_core.
 *
 * Flushes the children of the caller's chain (info->parent), restricted
 * by sync_tid.  Set info->domodify if the child's blockref must propagate
 * back up to the parent.
 *
 * Ripouts can move child from rbtree to dbtree or dbq but the caller's
 * flush scan order prevents any chains from being lost.  A child can be
 * executes more than once.
 *
 * WARNING! If we do not call hammer2_flush_core() we must update
 *	    bref.mirror_tid ourselves to indicate that the flush has
 *	    processed the child.
 *
 * WARNING! parent->core spinlock is held on entry and return.
 */
static int
hammer2_flush_recurse(hammer2_chain_t *child, void *data)
{
	hammer2_flush_info_t *info = data;
	hammer2_chain_t *parent = info->parent;

	/*
	 * (child can never be fchain or vchain so a special check isn't
	 *  needed).
	 *
	 * We must ref the child before unlocking the spinlock.
	 *
	 * The caller has added a ref to the parent so we can temporarily
	 * unlock it in order to lock the child.
	 */
	hammer2_chain_ref(child);
	hammer2_spin_unex(&parent->core.spin);

	hammer2_chain_unlock(parent);
	hammer2_chain_lock(child, HAMMER2_RESOLVE_MAYBE);

	/*
	 * Recurse and collect deferral data.  We're in the media flush,
	 * this can cross PFS boundaries.
	 */
	if (child->flags & HAMMER2_CHAIN_FLUSH_MASK) {
		++info->depth;
		hammer2_flush_core(info, child, info->flags);
		--info->depth;
	} else if (hammer2_debug & 0x200) {
		if (info->debug == NULL)
			info->debug = child;
		++info->depth;
		hammer2_flush_core(info, child, info->flags);
		--info->depth;
		if (info->debug == child)
			info->debug = NULL;
	}

	/*
	 * Relock to continue the loop
	 */
	hammer2_chain_unlock(child);
	hammer2_chain_lock(parent, HAMMER2_RESOLVE_MAYBE);
	hammer2_chain_drop(child);
	KKASSERT(info->parent == parent);
	hammer2_spin_ex(&parent->core.spin);

	return (0);
}

/*
 * flush helper (direct)
 *
 * Quickly flushes any dirty chains for a device.  This will update our
 * concept of the volume root but does NOT flush the actual volume root
 * and does not flush dirty device buffers.
 *
 * This function is primarily used by the bulkfree code to allow it to
 * create a snapshot for the pass.  It doesn't care about any pending
 * work (dirty vnodes, dirty inodes, dirty logical buffers) for which blocks
 * have not yet been allocated.
 */
void
hammer2_flush_quick(hammer2_dev_t *hmp)
{
	hammer2_chain_t *chain;

	hammer2_trans_init(hmp->spmp, HAMMER2_TRANS_ISFLUSH);

	hammer2_chain_ref(&hmp->vchain);
	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	if (hmp->vchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		chain = &hmp->vchain;
		hammer2_flush(chain, HAMMER2_FLUSH_TOP |
				     HAMMER2_FLUSH_ALL);
		KKASSERT(chain == &hmp->vchain);
	}
	hammer2_chain_unlock(&hmp->vchain);
	hammer2_chain_drop(&hmp->vchain);

	hammer2_trans_done(hmp->spmp);  /* spmp trans */
}

/*
 * flush helper (backend threaded)
 *
 * Flushes core chains, issues disk sync, flushes volume roots.
 *
 * Primarily called from vfs_sync().
 */
void
hammer2_inode_xop_flush(hammer2_xop_t *arg, int clindex)
{
	hammer2_xop_flush_t *xop = &arg->xop_flush;
	hammer2_chain_t *chain;
	hammer2_chain_t *parent;
	hammer2_dev_t *hmp;
	int error = 0;
	int total_error = 0;
	int j;

	/*
	 * Flush core chains
	 */
	chain = hammer2_inode_chain(xop->head.ip1, clindex,
				    HAMMER2_RESOLVE_ALWAYS);
	if (chain) {
		hmp = chain->hmp;
		if (chain->flags & HAMMER2_CHAIN_FLUSH_MASK) {
			hammer2_flush(chain, HAMMER2_FLUSH_TOP);
			parent = chain->parent;
			KKASSERT(chain->pmp != parent->pmp);
			hammer2_chain_setflush(parent);
		}
		hammer2_chain_unlock(chain);
		hammer2_chain_drop(chain);
		chain = NULL;
	} else {
		hmp = NULL;
	}

	/*
	 * Flush volume roots.  Avoid replication, we only want to
	 * flush each hammer2_dev (hmp) once.
	 */
	for (j = clindex - 1; j >= 0; --j) {
		if ((chain = xop->head.ip1->cluster.array[j].chain) != NULL) {
			if (chain->hmp == hmp) {
				chain = NULL;	/* safety */
				goto skip;
			}
		}
	}
	chain = NULL;	/* safety */

	/*
	 * spmp transaction.  The super-root is never directly mounted so
	 * there shouldn't be any vnodes, let alone any dirty vnodes
	 * associated with it, so we shouldn't have to mess around with any
	 * vnode flushes here.
	 */
	hammer2_trans_init(hmp->spmp, HAMMER2_TRANS_ISFLUSH);

	/*
	 * Media mounts have two 'roots', vchain for the topology
	 * and fchain for the free block table.  Flush both.
	 *
	 * Note that the topology and free block table are handled
	 * independently, so the free block table can wind up being
	 * ahead of the topology.  We depend on the bulk free scan
	 * code to deal with any loose ends.
	 */
	hammer2_chain_ref(&hmp->vchain);
	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	hammer2_chain_ref(&hmp->fchain);
	hammer2_chain_lock(&hmp->fchain, HAMMER2_RESOLVE_ALWAYS);
	if (hmp->fchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		/*
		 * This will also modify vchain as a side effect,
		 * mark vchain as modified now.
		 */
		hammer2_voldata_modify(hmp);
		chain = &hmp->fchain;
		hammer2_flush(chain, HAMMER2_FLUSH_TOP);
		KKASSERT(chain == &hmp->fchain);
	}
	hammer2_chain_unlock(&hmp->fchain);
	hammer2_chain_unlock(&hmp->vchain);
	hammer2_chain_drop(&hmp->fchain);
	/* vchain dropped down below */

	hammer2_chain_lock(&hmp->vchain, HAMMER2_RESOLVE_ALWAYS);
	if (hmp->vchain.flags & HAMMER2_CHAIN_FLUSH_MASK) {
		chain = &hmp->vchain;
		hammer2_flush(chain, HAMMER2_FLUSH_TOP);
		KKASSERT(chain == &hmp->vchain);
	}
	hammer2_chain_unlock(&hmp->vchain);
	hammer2_chain_drop(&hmp->vchain);

	error = 0;

	/*
	 * We can't safely flush the volume header until we have
	 * flushed any device buffers which have built up.
	 *
	 * XXX this isn't being incremental
	 */
	vn_lock(hmp->devvp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(hmp->devvp, MNT_WAIT, 0);
	vn_unlock(hmp->devvp);

	/*
	 * The flush code sets CHAIN_VOLUMESYNC to indicate that the
	 * volume header needs synchronization via hmp->volsync.
	 *
	 * XXX synchronize the flag & data with only this flush XXX
	 */
	if (error == 0 &&
	    (hmp->vchain.flags & HAMMER2_CHAIN_VOLUMESYNC)) {
		struct buf *bp;

		/*
		 * Synchronize the disk before flushing the volume
		 * header.
		 */
		bp = getpbuf(NULL);
		bp->b_bio1.bio_offset = 0;
		bp->b_bufsize = 0;
		bp->b_bcount = 0;
		bp->b_cmd = BUF_CMD_FLUSH;
		bp->b_bio1.bio_done = biodone_sync;
		bp->b_bio1.bio_flags |= BIO_SYNC;
		vn_strategy(hmp->devvp, &bp->b_bio1);
		biowait(&bp->b_bio1, "h2vol");
		relpbuf(bp, NULL);

		/*
		 * Then we can safely flush the version of the
		 * volume header synchronized by the flush code.
		 */
		j = hmp->volhdrno + 1;
		if (j >= HAMMER2_NUM_VOLHDRS)
			j = 0;
		if (j * HAMMER2_ZONE_BYTES64 + HAMMER2_SEGSIZE >
		    hmp->volsync.volu_size) {
			j = 0;
		}
		if (hammer2_debug & 0x8000) {
			/* debug only, avoid syslogd loop */
			kprintf("sync volhdr %d %jd\n",
				j, (intmax_t)hmp->volsync.volu_size);
		}
		bp = getblk(hmp->devvp, j * HAMMER2_ZONE_BYTES64,
			    HAMMER2_PBUFSIZE, 0, 0);
		atomic_clear_int(&hmp->vchain.flags,
				 HAMMER2_CHAIN_VOLUMESYNC);
		bcopy(&hmp->volsync, bp->b_data, HAMMER2_PBUFSIZE);
		bawrite(bp);
		hmp->volhdrno = j;
	}
	if (error)
		total_error = error;

	hammer2_trans_done(hmp->spmp);  /* spmp trans */
skip:
	error = hammer2_xop_feed(&xop->head, NULL, clindex, total_error);
}
