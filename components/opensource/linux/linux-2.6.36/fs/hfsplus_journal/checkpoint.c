/*
 * linux/fs/checkpoint.c
 * 
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1999 Red Hat Software --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Checkpoint routines for the generic filesystem journaling code.  
 * Part of the ext2fs journaling system.  
 *
 * Checkpointing is the process of ensuring that a section of the log is
 * committed fully to disk, so that that portion of the log can be
 * reused.
 */

#include <linux/time.h>
#include <linux/fs.h>
#include "hfsplus_jbd.h"
#include <linux/errno.h>
#include <linux/slab.h>
#include "hfsplus_fs.h"

/*
 * Unlink a buffer from a transaction. 
 *
 * Called with j_list_lock held.
 */

static inline void __buffer_unlink(struct hfsplus_jbd_head *jh)
{
	hfsplus_transaction_t *transaction;

	transaction = jh->b_cp_transaction;
	jh->b_cp_transaction = NULL;

	jh->b_cpnext->b_cpprev = jh->b_cpprev;
	jh->b_cpprev->b_cpnext = jh->b_cpnext;
	if (transaction->t_checkpoint_list == jh)
		transaction->t_checkpoint_list = jh->b_cpnext;
	if (transaction->t_checkpoint_list == jh)
		transaction->t_checkpoint_list = NULL;
}

/*
 * Try to release a checkpointed buffer from its transaction.
 * Returns 1 if we released it.
 * Requires j_list_lock
 * Called under hfsplus_jbd_lock_bh_state(hfsplus_jh2bh(jh)), and drops it
 */
static int __try_to_free_cp_buf(struct hfsplus_jbd_head *jh)
{
	int ret = 0;
	struct buffer_head *bh = hfsplus_jh2bh(jh);

	if (jh->b_jlist == HFSPLUS_BJ_None && !buffer_locked(bh) && !buffer_dirty(bh)) {
		HFSPLUS_JBUFFER_TRACE(jh, "remove from checkpoint list");
		__hfsplus_jbd_remove_checkpoint(jh);
		hfsplus_jbd_unlock_bh_state(bh);
		hfsplus_jbd_remove_journal_head(bh);
		HFSPLUS_BUFFER_TRACE(bh, "release");
		__brelse(bh);
		ret = 1;
	} else {
		hfsplus_jbd_unlock_bh_state(bh);
	}
	return ret;
}

/*
 * __hfsplus__log_wait_for_space: wait until there is space in the journal.
 *
 * Called under j-state_lock *only*.  It will be unlocked if we have to wait
 * for a checkpoint to free up some space in the log.
 */
void __hfsplus__log_wait_for_space(hfsplus_jbd_t *journal)
{
	int nblocks;
	assert_spin_locked(&journal->j_state_lock);

	nblocks = hfsplus_jbd_space_needed(journal);
	while (__hfsplus__log_space_left(journal) < nblocks) {
		if (journal->j_flags & JFS_ABORT)
			return;
		spin_unlock(&journal->j_state_lock);
		down(&journal->j_checkpoint_sem);

		/*
		 * Test again, another process may have checkpointed while we
		 * were waiting for the checkpoint lock
		 */
		spin_lock(&journal->j_state_lock);
		nblocks = hfsplus_jbd_space_needed(journal);
		if (__hfsplus__log_space_left(journal) < nblocks) {
			spin_unlock(&journal->j_state_lock);
			hfsplus_jbd_log_do_checkpoint(journal);
			spin_lock(&journal->j_state_lock);
		}
		up(&journal->j_checkpoint_sem);
	}
}

/*
 * We were unable to perform hfsplus_jbd_trylock_bh_state() inside j_list_lock.
 * The caller must restart a list walk.  Wait for someone else to run
 * hfsplus_jbd_unlock_bh_state().
 */
static void jbd_sync_bh(hfsplus_jbd_t *journal, struct buffer_head *bh)
{
	get_bh(bh);
	spin_unlock(&journal->j_list_lock);
	hfsplus_jbd_lock_bh_state(bh);
	hfsplus_jbd_unlock_bh_state(bh);
	put_bh(bh);
}

/*
 * Clean up a transaction's checkpoint list.  
 *
 * We wait for any pending IO to complete and make sure any clean
 * buffers are removed from the transaction. 
 *
 * Return 1 if we performed any actions which might have destroyed the
 * checkpoint.  (hfsplus_jbd_remove_checkpoint() deletes the transaction when
 * the last checkpoint buffer is cleansed)
 *
 * Called with j_list_lock held.
 */
static int __cleanup_transaction(hfsplus_jbd_t *journal, hfsplus_transaction_t *transaction)
{
	struct hfsplus_jbd_head *jh, *next_jh, *last_jh;
	struct buffer_head *bh;
	int ret = 0;

	assert_spin_locked(&journal->j_list_lock);
	jh = transaction->t_checkpoint_list;
	if (!jh)
		return 0;

	last_jh = jh->b_cpprev;
	next_jh = jh;
	do {
		jh = next_jh;
		bh = hfsplus_jh2bh(jh);
		if (buffer_locked(bh)) {
			atomic_inc(&bh->b_count);
			spin_unlock(&journal->j_list_lock);
			wait_on_buffer(bh);
			/* the hfsplus_jbd_head may have gone by now */
			HFSPLUS_BUFFER_TRACE(bh, "brelse");
			__brelse(bh);
			goto out_return_1;
		}

		/*
		 * This is foul
		 */
		if (!hfsplus_jbd_trylock_bh_state(bh)) {
			jbd_sync_bh(journal, bh);
			goto out_return_1;
		}

		if (jh->b_transaction != NULL) {
			hfsplus_transaction_t *t = jh->b_transaction;
			hfsplus_jbd_tid_t tid = t->t_tid;

			spin_unlock(&journal->j_list_lock);
			hfsplus_jbd_unlock_bh_state(bh);
			hfsplus_log_start_commit(journal, tid);
			hfsplus_jbd_log_wait_commit(journal, tid);
			goto out_return_1;
		}

		/*
		 * AKPM: I think the buffer_hfsplus_jbddirty test is redundant - it
		 * shouldn't have NULL b_transaction?
		 */
		next_jh = jh->b_cpnext;
		if (!buffer_dirty(bh) && !buffer_hfsplus_jbddirty(bh)) {
			HFSPLUS_BUFFER_TRACE(bh, "remove from checkpoint");
			__hfsplus_jbd_remove_checkpoint(jh);
			hfsplus_jbd_unlock_bh_state(bh);
			hfsplus_jbd_remove_journal_head(bh);
			__brelse(bh);
			ret = 1;
		} else {
			hfsplus_jbd_unlock_bh_state(bh);
		}
	} while (jh != last_jh);

	return ret;
out_return_1:
	spin_lock(&journal->j_list_lock);
	return 1;
}

#define NR_BATCH	64

static void
__flush_batch(hfsplus_jbd_t *journal, struct buffer_head **bhs, int *batch_count)
{
	int i;

	spin_unlock(&journal->j_list_lock);
//	ll_rw_block(SWRITE, *batch_count, bhs);
	spin_lock(&journal->j_list_lock);
	for (i = 0; i < *batch_count; i++) {
		struct buffer_head *bh = bhs[i];
		clear_buffer_hfsplus_jbd_jwrite(bh);
		HFSPLUS_BUFFER_TRACE(bh, "brelse");
		__brelse(bh);
	}
	*batch_count = 0;
}

/*
 * Try to flush one buffer from the checkpoint list to disk.
 *
 * Return 1 if something happened which requires us to abort the current
 * scan of the checkpoint list.  
 *
 * Called with j_list_lock held.
 * Called under hfsplus_jbd_lock_bh_state(hfsplus_jh2bh(jh)), and drops it
 */
static int __flush_buffer(hfsplus_jbd_t *journal, struct hfsplus_jbd_head *jh,
			struct buffer_head **bhs, int *batch_count,
			int *drop_count)
{
	struct buffer_head *bh = hfsplus_jh2bh(jh);
	int ret = 0;

	if (buffer_dirty(bh) && !buffer_locked(bh) && jh->b_jlist == HFSPLUS_BJ_None) {
		HFSPLUS_J_ASSERT_JH(jh, jh->b_transaction == NULL);

		/*
		 * Important: we are about to write the buffer, and
		 * possibly block, while still holding the journal lock.
		 * We cannot afford to let the transaction logic start
		 * messing around with this buffer before we write it to
		 * disk, as that would break recoverability.  
		 */
		HFSPLUS_BUFFER_TRACE(bh, "queue");
		get_bh(bh);
		HFSPLUS_J_ASSERT_BH(bh, !buffer_hfsplus_jbd_jwrite(bh));
		set_buffer_hfsplus_jbd_jwrite(bh);
		bhs[*batch_count] = bh;
		hfsplus_jbd_unlock_bh_state(bh);
		(*batch_count)++;
		if (*batch_count == NR_BATCH) {
			__flush_batch(journal, bhs, batch_count);
			ret = 1;
		}
	} else {
		int last_buffer = 0;
		if (jh->b_cpnext == jh) {
			/* We may be about to drop the transaction.  Tell the
			 * caller that the lists have changed.
			 */
			last_buffer = 1;
		}
		if (__try_to_free_cp_buf(jh)) {
			(*drop_count)++;
			ret = last_buffer;
		}
	}
	return ret;
}

/*
 * Perform an actual checkpoint.  We don't write out only enough to
 * satisfy the current blocked requests: rather we submit a reasonably
 * sized chunk of the outstanding data to disk at once for
 * efficiency.  __hfsplus__log_wait_for_space() will retry if we didn't free enough.
 * 
 * However, we _do_ take into account the amount requested so that once
 * the IO has been queued, we can return as soon as enough of it has
 * completed to disk.  
 *
 * The journal should be locked before calling this function.
 */
int hfsplus_jbd_log_do_checkpoint(hfsplus_jbd_t *journal)
{
	int result;
	int batch_count = 0;
	struct buffer_head *bhs[NR_BATCH];

	dprint(DBG_JCHKPT, "Start checkpoint\n");

	/* 
	 * First thing: if there are any transactions in the log which
	 * don't need checkpointing, just eliminate them from the
	 * journal straight away.  
	 */
	result = cleanup_hfsplus_jbd_tail(journal);
	dprint(DBG_JCHKPT, "cleanup_hfsplus_jbd_tail returned %d\n", result);
	if (result <= 0)
		return result;

	/*
	 * OK, we need to start writing disk blocks.  Try to free up a
	 * quarter of the log in a single checkpoint if we can.
	 */
	/*
	 * AKPM: check this code.  I had a feeling a while back that it
	 * degenerates into a busy loop at unmount time.
	 */
	spin_lock(&journal->j_list_lock);
	while (journal->j_checkpoint_transactions) {
		hfsplus_transaction_t *transaction;
		struct hfsplus_jbd_head *jh, *last_jh, *next_jh;
		int drop_count = 0;
		int cleanup_ret, retry = 0;
		hfsplus_jbd_tid_t this_tid;

		transaction = journal->j_checkpoint_transactions;
		this_tid = transaction->t_tid;
		jh = transaction->t_checkpoint_list;
		last_jh = jh->b_cpprev;
		next_jh = jh;
		do {
			struct buffer_head *bh;

			jh = next_jh;
			next_jh = jh->b_cpnext;
			bh = hfsplus_jh2bh(jh);
			if (!hfsplus_jbd_trylock_bh_state(bh)) {
				jbd_sync_bh(journal, bh);
				spin_lock(&journal->j_list_lock);
				retry = 1;
				break;
			}
			retry = __flush_buffer(journal, jh, bhs, &batch_count, &drop_count);
			if (cond_resched_lock(&journal->j_list_lock)) {
				retry = 1;
				break;
			}
		} while (jh != last_jh && !retry);

		if (batch_count) {
			__flush_batch(journal, bhs, &batch_count);
			retry = 1;
		}

		/*
		 * If someone cleaned up this transaction while we slept, we're
		 * done
		 */
		if (journal->j_checkpoint_transactions != transaction)
			break;
		if (retry)
			continue;
		/*
		 * Maybe it's a new transaction, but it fell at the same
		 * address
		 */
		if (transaction->t_tid != this_tid)
			continue;
		/*
		 * We have walked the whole transaction list without
		 * finding anything to write to disk.  We had better be
		 * able to make some progress or we are in trouble. 
		 */
		cleanup_ret = __cleanup_transaction(journal, transaction);
		HFSPLUS_J_ASSERT(drop_count != 0 || cleanup_ret != 0);
		if (journal->j_checkpoint_transactions != transaction)
			break;
	}
	spin_unlock(&journal->j_list_lock);
	result = cleanup_hfsplus_jbd_tail(journal);
	if (result < 0)
		return result;

	return 0;
}

/*
 * Check the list of checkpoint transactions for the journal to see if
 * we have already got rid of any since the last update of the log tail
 * in the journal superblock.  If so, we can instantly roll the
 * superblock forward to remove those transactions from the log.
 * 
 * Return <0 on error, 0 on success, 1 if there was nothing to clean up.
 * 
 * Called with the journal lock held.
 *
 * This is the only part of the journaling code which really needs to be
 * aware of transaction aborts.  Checkpointing involves writing to the
 * main filesystem area rather than to the journal, so it can proceed
 * even in abort state, but we must not update the journal superblock if
 * we have an abort error outstanding.
 */

int cleanup_hfsplus_jbd_tail(hfsplus_jbd_t *journal)
{
	hfsplus_transaction_t * transaction;
	hfsplus_jbd_tid_t		first_tid;
	unsigned long	blocknr, freed;

	/* OK, work out the oldest transaction remaining in the log, and
	 * the log block it starts at. 
	 * 
	 * If the log is now empty, we need to work out which is the
	 * next transaction ID we will write, and where it will
	 * start. */

	spin_lock(&journal->j_state_lock);
	spin_lock(&journal->j_list_lock);
	transaction = journal->j_checkpoint_transactions;
	if (transaction) {
		first_tid = transaction->t_tid;
		blocknr = transaction->t_log_start;
	} else if ((transaction = journal->j_committing_transaction) != NULL) {
		first_tid = transaction->t_tid;
		blocknr = transaction->t_log_start;
	} else if ((transaction = journal->j_running_transaction) != NULL) {
		first_tid = transaction->t_tid;
		blocknr = journal->j_head;
	} else {
		first_tid = journal->j_transaction_sequence;
		blocknr = journal->j_head;
	}
	spin_unlock(&journal->j_list_lock);
	HFSPLUS_J_ASSERT(blocknr != 0);

	/* If the oldest pinned transaction is at the tail of the log
           already then there's not much we can do right now. */
	if (journal->j_tail_sequence == first_tid) {
		spin_unlock(&journal->j_state_lock);
		return 1;
	}

	/* OK, update the superblock to recover the freed space.
	 * Physical blocks come first: have we wrapped beyond the end of
	 * the log?  */
	freed = blocknr - journal->j_tail;
	if (blocknr < journal->j_tail)
		freed = freed + journal->j_last - journal->j_first;

#ifdef HFSPLUS_JOURNAL_MAC_COMPATIBLE
	hfsplus_journal_header_start_update(journal, freed);
#endif

	dprint(DBG_JCHKPT,
		  "Cleaning journal tail from %d to %d (offset %lu), "
		  "freeing %lu\n",
		  journal->j_tail_sequence, first_tid, blocknr, freed);

	journal->j_free += freed;
	journal->j_tail_sequence = first_tid;
	journal->j_tail = blocknr;
	spin_unlock(&journal->j_state_lock);
	if (!(journal->j_flags & JFS_ABORT))
		hfsplus_jbd_update_superblock(journal, 1);
	return 0;
}


/* Checkpoint list management */

/*
 * hfsplus_jbd_clean_checkpoint_list
 *
 * Find all the written-back checkpoint buffers in the journal and release them.
 *
 * Called with the journal locked.
 * Called with j_list_lock held.
 * Returns number of bufers reaped (for debug)
 */

int __hfsplus_jbd_clean_checkpoint_list(hfsplus_jbd_t *journal)
{
	hfsplus_transaction_t *transaction, *last_transaction, *next_transaction;
	int ret = 0;

	transaction = journal->j_checkpoint_transactions;
	if (transaction == 0)
		goto out;

	last_transaction = transaction->t_cpprev;
	next_transaction = transaction;
	do {
		struct hfsplus_jbd_head *jh;

		transaction = next_transaction;
		next_transaction = transaction->t_cpnext;
		jh = transaction->t_checkpoint_list;
		if (jh) {
			struct hfsplus_jbd_head *last_jh = jh->b_cpprev;
			struct hfsplus_jbd_head *next_jh = jh;

			do {
				jh = next_jh;
				next_jh = jh->b_cpnext;
				/* Use trylock because of the ranknig */
				if (hfsplus_jbd_trylock_bh_state(hfsplus_jh2bh(jh)))
					ret += __try_to_free_cp_buf(jh);
				/*
				 * This function only frees up some memory
				 * if possible so we dont have an obligation
				 * to finish processing. Bail out if preemption
				 * requested:
				 */
				if (need_resched())
					goto out;
			} while (jh != last_jh);
		}
	} while (transaction != last_transaction);
out:
	return ret;
}

/* 
 * hfsplus_jbd_remove_checkpoint: called after a buffer has been committed
 * to disk (either by being write-back flushed to disk, or being
 * committed to the log).
 *
 * We cannot safely clean a transaction out of the log until all of the
 * buffer updates committed in that transaction have safely been stored
 * elsewhere on disk.  To achieve this, all of the buffers in a
 * transaction need to be maintained on the transaction's checkpoint
 * list until they have been rewritten, at which point this function is
 * called to remove the buffer from the existing transaction's
 * checkpoint list.  
 *
 * This function is called with the journal locked.
 * This function is called with j_list_lock held.
 */

void __hfsplus_jbd_remove_checkpoint(struct hfsplus_jbd_head *jh)
{
	hfsplus_transaction_t *transaction;
	hfsplus_jbd_t *journal;

	HFSPLUS_JBUFFER_TRACE(jh, "entry");

	if ((transaction = jh->b_cp_transaction) == NULL) {
		HFSPLUS_JBUFFER_TRACE(jh, "not on transaction");
		goto out;
	}
	journal = transaction->t_journal;

	__buffer_unlink(jh);

	if (transaction->t_checkpoint_list != NULL)
		goto out;
	HFSPLUS_JBUFFER_TRACE(jh, "transaction has no more buffers");

	/*
	 * There is one special case to worry about: if we have just pulled the
	 * buffer off a committing transaction's forget list, then even if the
	 * checkpoint list is empty, the transaction obviously cannot be
	 * dropped!
	 *
	 * The locking here around j_committing_transaction is a bit sleazy.
	 * See the comment at the end of hfsplus_jbd_commit_transaction().
	 */
	if (transaction == journal->j_committing_transaction) {
		HFSPLUS_JBUFFER_TRACE(jh, "belongs to committing transaction");
		goto out;
	}

	/* OK, that was the last buffer for the transaction: we can now
	   safely remove this transaction from the log */

	__hfsplus_jbd_drop_transaction(journal, transaction);

	/* Just in case anybody was waiting for more transactions to be
           checkpointed... */
	wake_up(&journal->j_wait_logspace);
out:
	HFSPLUS_JBUFFER_TRACE(jh, "exit");
}

/*
 * hfsplus_jbd_insert_checkpoint: put a committed buffer onto a checkpoint
 * list so that we know when it is safe to clean the transaction out of
 * the log.
 *
 * Called with the journal locked.
 * Called with j_list_lock held.
 */
void __hfsplus_jbd_insert_checkpoint(struct hfsplus_jbd_head *jh, 
			       hfsplus_transaction_t *transaction)
{
	HFSPLUS_JBUFFER_TRACE(jh, "entry");
	HFSPLUS_J_ASSERT_JH(jh, buffer_dirty(hfsplus_jh2bh(jh)) || buffer_hfsplus_jbddirty(hfsplus_jh2bh(jh)));
	HFSPLUS_J_ASSERT_JH(jh, jh->b_cp_transaction == NULL);

	jh->b_cp_transaction = transaction;

	if (!transaction->t_checkpoint_list) {
		jh->b_cpnext = jh->b_cpprev = jh;
	} else {
		jh->b_cpnext = transaction->t_checkpoint_list;
		jh->b_cpprev = transaction->t_checkpoint_list->b_cpprev;
		jh->b_cpprev->b_cpnext = jh;
		jh->b_cpnext->b_cpprev = jh;
	}
	transaction->t_checkpoint_list = jh;
}

/*
 * We've finished with this transaction structure: adios...
 * 
 * The transaction must have no links except for the checkpoint by this
 * point.
 *
 * Called with the journal locked.
 * Called with j_list_lock held.
 */

void __hfsplus_jbd_drop_transaction(hfsplus_jbd_t *journal, hfsplus_transaction_t *transaction)
{
	assert_spin_locked(&journal->j_list_lock);
	if (transaction->t_cpnext) {
		transaction->t_cpnext->t_cpprev = transaction->t_cpprev;
		transaction->t_cpprev->t_cpnext = transaction->t_cpnext;
		if (journal->j_checkpoint_transactions == transaction)
			journal->j_checkpoint_transactions =
				transaction->t_cpnext;
		if (journal->j_checkpoint_transactions == transaction)
			journal->j_checkpoint_transactions = NULL;
	}

	HFSPLUS_J_ASSERT(transaction->t_state == HFSPLUS_T_FINISHED);
	HFSPLUS_J_ASSERT(transaction->t_buffers == NULL);
	HFSPLUS_J_ASSERT(transaction->t_sync_datalist == NULL);
	HFSPLUS_J_ASSERT(transaction->t_forget == NULL);
	HFSPLUS_J_ASSERT(transaction->t_iobuf_list == NULL);
	HFSPLUS_J_ASSERT(transaction->t_shadow_list == NULL);
	HFSPLUS_J_ASSERT(transaction->t_log_list == NULL);
	HFSPLUS_J_ASSERT(transaction->t_checkpoint_list == NULL);
	HFSPLUS_J_ASSERT(transaction->t_updates == 0);
	HFSPLUS_J_ASSERT(journal->j_committing_transaction != transaction);
	HFSPLUS_J_ASSERT(journal->j_running_transaction != transaction);

	dprint(DBG_JCHKPT, "Dropping transaction %d, all done\n", transaction->t_tid);
	kfree(transaction);
}
