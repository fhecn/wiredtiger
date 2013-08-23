/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_row_modify --
 *	Row-store insert, update and delete.
 */
int
__wt_row_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_remove)
{
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *ins_head, **ins_headp, *t;
	WT_ITEM *key, *value;
	WT_PAGE *page;
	WT_UPDATE *old_upd, *upd, **upd_entry, *upd_obsolete;
	size_t ins_size, upd_size;
	uint32_t ins_slot;
	u_int skipdepth;
	int i, logged;

	key = &cbt->iface.key;
	value = is_remove ? NULL : &cbt->iface.value;

	page = cbt->page;

	ins = NULL;
	upd = NULL;
	logged = 0;

	/*
	 * Modify: allocate an update array as necessary, build a WT_UPDATE
	 * structure, and call a serialized function to insert the WT_UPDATE
	 * structure.
	 *
	 * Insert: allocate an insert array as necessary, build a WT_INSERT
	 * and WT_UPDATE structure pair, and call a serialized function to
	 * insert the WT_INSERT structure.
	 */
	if (cbt->compare == 0) {
		if (cbt->ins == NULL) {
			/* Allocate an update array as necessary. */
			if (page->u.row.upd == NULL) {
				WT_ERR(__wt_calloc_def(
				    session, page->entries, &upd));
				if (WT_ATOMIC_CAS(page->u.row.upd, NULL, upd))
					__wt_cache_page_inmem_incr(session,
					    page, page->entries *
					    sizeof(WT_UPDATE *));
				else
					__wt_free(session, upd);
				upd = NULL;
			}

			/* Set the WT_UPDATE array reference. */
			upd_entry = &page->u.row.upd[cbt->slot];
		} else
			upd_entry = &cbt->ins->upd;

		/* Make sure the update can proceed. */
		WT_ERR(__wt_txn_update_check(session, old_upd = *upd_entry));

		/* Allocate the WT_UPDATE structure and transaction ID. */
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		WT_ERR(__wt_txn_modify(session, &upd->txnid));
		logged = 1;

		/* Serialize the update. */
		WT_ERR(__wt_update_serial(session, page, cbt->write_gen,
		    upd_entry, old_upd, &upd, upd_size, &upd_obsolete));

		/* Discard any obsolete WT_UPDATE structures. */
		if (upd_obsolete != NULL)
			__wt_update_obsolete_free(session, page, upd_obsolete);
	} else {
		/*
		 * Allocate the insert array as necessary.
		 *
		 * We allocate an additional insert array slot for insert keys
		 * sorting less than any key on the page.  The test to select
		 * that slot is baroque: if the search returned the first page
		 * slot, we didn't end up processing an insert list, and the
		 * comparison value indicates the search key was smaller than
		 * the returned slot, then we're using the smallest-key insert
		 * slot.  That's hard, so we set a flag.
		 */
		ins_slot = F_ISSET(
		    cbt, WT_CBT_SEARCH_SMALLEST) ? page->entries : cbt->slot;

		if (page->u.row.ins == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->entries + 1, &ins));
			if (WT_ATOMIC_CAS(page->u.row.ins, NULL, ins))
				__wt_cache_page_inmem_incr(session,
				    page, (page->entries + 1) *
				    sizeof(WT_INSERT_HEAD *));
			else
				__wt_free(session, ins);
			ins = NULL;
		}
		ins_headp = &page->u.row.ins[ins_slot];

		/* Allocate the WT_INSERT_HEAD structure as necessary. */
		if ((ins_head = *ins_headp) == NULL) {
			WT_ERR(__wt_calloc_def(session, 1, &t));
			if (WT_ATOMIC_CAS(*ins_headp, NULL, t)) {
				__wt_cache_page_inmem_incr(session,
				    page, sizeof(WT_INSERT_HEAD));

				/*
				 * If allocating a new insert list head, we have
				 * to initialize the cursor's insert list stack
				 * and insert head reference as well, search
				 * couldn't have.
				 */
				for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
					cbt->ins_stack[i] = &t->head[i];
					cbt->next_stack[i] = NULL;
				}
				cbt->ins_head = t;
			} else {
				/*
				 * I'm not returning restart here, even though
				 * the update will fail (the cursor's insert
				 * stack is by definition wrong because it was
				 * never set).   The reason is because it won't
				 * close the race, it only makes it less likely
				 * (and maybe simplifies the serialization
				 * function check).  Let the serialization code
				 * own the problem.
				 */
				__wt_free(session, t);
			}

			ins_head = *ins_headp;
		}

		/* Choose a skiplist depth for this insert. */
		skipdepth = __wt_skip_choose_depth();

		/*
		 * Allocate a WT_INSERT/WT_UPDATE pair and transaction ID, and
		 * update the cursor to reference it.
		 */
		WT_ERR(__wt_row_insert_alloc(
		    session, key, skipdepth, &ins, &ins_size));
		WT_ERR(__wt_update_alloc(session, value, &upd, &upd_size));
		WT_ERR(__wt_txn_modify(session, &upd->txnid));
		logged = 1;
		ins->upd = upd;
		ins_size += upd_size;
		cbt->ins = ins;

		/* Insert the WT_INSERT structure. */
		WT_ERR(__wt_insert_serial(session, page, cbt->write_gen,
		    ins_head, cbt->ins_stack, cbt->next_stack,
		    &ins, ins_size, skipdepth));
	}

	if (0) {
err:		/*
		 * Remove the update from the current transaction, so we don't
		 * try to modify it on rollback.
		 */
		if (logged)
			__wt_txn_unmodify(session);
		__wt_free(session, ins);
		__wt_free(session, upd);
	}

	return (ret);
}

/*
 * __wt_row_insert_alloc --
 *	Row-store insert: allocate a WT_INSERT structure and fill it in.
 */
int
__wt_row_insert_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *key, u_int skipdepth, WT_INSERT **insp, size_t *ins_sizep)
{
	WT_INSERT *ins;
	size_t ins_size;

	/*
	 * Allocate the WT_INSERT structure, next pointers for the skip list,
	 * and room for the key.  Then copy the key into place.
	 */
	ins_size = sizeof(WT_INSERT) +
	    skipdepth * sizeof(WT_INSERT *) + key->size;
	WT_RET(__wt_calloc(session, 1, ins_size, &ins));

	ins->u.key.offset = WT_STORE_SIZE(ins_size - key->size);
	WT_INSERT_KEY_SIZE(ins) = key->size;
	memcpy(WT_INSERT_KEY(ins), key->data, key->size);

	*insp = ins;
	if (ins_sizep != NULL)
		*ins_sizep = ins_size;
	return (0);
}

/*
 * __wt_insert_serial_func --
 *	Server function to add an WT_INSERT entry to the page.
 */
int
__wt_insert_serial_func(WT_SESSION_IMPL *session, void *args)
{
	WT_INSERT *new_ins, ***ins_stack, **next_stack;
	WT_INSERT_HEAD *ins_head;
	WT_PAGE *page;
	uint32_t write_gen;
	u_int i, skipdepth;

	__wt_insert_unpack(args, &page, &write_gen,
	    &ins_head, &ins_stack, &next_stack, &new_ins, &skipdepth);

	/*
	 * Largely ignore the page's write-generation, just confirm it hasn't
	 * wrapped.
	 */
	WT_RET(__wt_page_write_gen_wrapped_check(page));

	/*
	 * Confirm we are still in the expected position, and no item has been
	 * added where our insert belongs.  Take extra care at the beginning
	 * and end of the list (at each level): retry if we race there.
	 */
	for (i = 0; i < skipdepth; i++) {
		if (ins_stack[i] == NULL ||
		    *ins_stack[i] != next_stack[i])
			return (WT_RESTART);
		if (next_stack[i] == NULL &&
		    ins_head->tail[i] != NULL &&
		    ins_stack[i] != &ins_head->tail[i]->next[i])
			return (WT_RESTART);
	}

	/*
	 * Publish: First, point the new WT_INSERT item's skiplist references
	 * to the next elements in the insert list, then flush memory.  Second,
	 * update the skiplist elements that reference the new WT_INSERT item,
	 * this ensures the list is never inconsistent.
	 */
	for (i = 0; i < skipdepth; i++)
		new_ins->next[i] = *ins_stack[i];
	WT_WRITE_BARRIER();
	for (i = 0; i < skipdepth; i++) {
		if (ins_head->tail[i] == NULL ||
		    ins_stack[i] == &ins_head->tail[i]->next[i])
			ins_head->tail[i] = new_ins;
		*ins_stack[i] = new_ins;
	}

	__wt_insert_new_ins_taken(args);

	__wt_page_and_tree_modify_set(session, page);
	return (0);
}

/*
 * __wt_update_alloc --
 *	Allocate a WT_UPDATE structure and associated value and fill it in.
 */
int
__wt_update_alloc(WT_SESSION_IMPL *session,
    WT_ITEM *value, WT_UPDATE **updp, size_t *sizep)
{
	WT_UPDATE *upd;
	size_t size;

	/*
	 * Allocate the WT_UPDATE structure and room for the value, then copy
	 * the value into place.
	 */
	size = value == NULL ? 0 : value->size;
	WT_RET(__wt_calloc(session, 1, sizeof(WT_UPDATE) + size, &upd));
	if (value == NULL)
		WT_UPDATE_DELETED_SET(upd);
	else {
		upd->size = WT_STORE_SIZE(size);
		memcpy(WT_UPDATE_DATA(upd), value->data, size);
	}

	*updp = upd;
	*sizep = sizeof(WT_UPDATE) + size;
	return (0);
}

/*
 * __wt_update_obsolete_check --
 *	Check for obsolete updates.
 */
WT_UPDATE *
__wt_update_obsolete_check(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_UPDATE *next;

	/*
	 * This function identifies obsolete updates, and truncates them from
	 * the rest of the chain; because this routine is called from inside
	 * a serialization function, the caller has responsibility for actually
	 * freeing the memory.
	 *
	 * Walk the list of updates, looking for obsolete updates.  If we find
	 * an update no session will ever move past, we can discard any updates
	 * that appear after it.
	 */
	for (; upd != NULL; upd = upd->next)
		if (__wt_txn_visible_all(session, upd->txnid)) {
			/*
			 * We cannot discard this WT_UPDATE structure, we can
			 * only discard WT_UPDATE structures subsequent to it,
			 * other threads of control will terminate their walk
			 * in this element.  Save a reference to the list we
			 * will discard, and terminate the list.
			 */
			if ((next = upd->next) == NULL)
				return (NULL);
			if (!WT_ATOMIC_CAS(upd->next, next, NULL))
				return (NULL);

			return (next);
		}
	return (NULL);
}

/*
 * __wt_update_obsolete_free --
 *	Free an obsolete update list.
 */
void
__wt_update_obsolete_free(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *upd)
{
	WT_UPDATE *next;
	size_t size;

	/* Free a WT_UPDATE list. */
	for (size = 0; upd != NULL; upd = next) {
		/* Deleted items have a dummy size: don't include that. */
		size += sizeof(WT_UPDATE) +
		    (WT_UPDATE_DELETED_ISSET(upd) ? 0 : upd->size);

		next = upd->next;
		__wt_free(session, upd);
	}
	if (size != 0)
		__wt_cache_page_inmem_decr(session, page, size);
}

/*
 * __wt_page_obsolete --
 *	Discard all obsolete updates on a row-store leaf page.
 */
void
__wt_row_leaf_obsolete(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	/* For entries before the first on-page record... */
	WT_SKIP_FOREACH(ins, WT_ROW_INSERT_SMALLEST(page))
		if ((upd =
		    __wt_update_obsolete_check(session, ins->upd)) != NULL)
			__wt_update_obsolete_free(session, page, upd);

	/* For each entry on the page... */
	WT_ROW_FOREACH(page, rip, i) {
		if ((upd = __wt_update_obsolete_check(
		    session, WT_ROW_UPDATE(page, rip))) != NULL)
			__wt_update_obsolete_free(session, page, upd);

		WT_SKIP_FOREACH(ins, WT_ROW_INSERT(page, rip))
			if ((upd = __wt_update_obsolete_check(
			    session, ins->upd)) != NULL)
				__wt_update_obsolete_free(session, page, upd);
	}
}

/*
 * __wt_update_serial_func --
 *	Server function to add an WT_UPDATE entry in the page array.
 */
int
__wt_update_serial_func(WT_SESSION_IMPL *session, void *args)
{
	WT_PAGE *page;
	WT_UPDATE *old_upd, *upd, **upd_entry, **upd_obsolete;
	uint32_t write_gen;

	__wt_update_unpack(args, &page, &write_gen,
	    &upd_entry, &old_upd, &upd, &upd_obsolete);

	/*
	 * Ignore the page's write-generation (other than the special case of
	 * it wrapping).  If we're still in the expected position, we're good
	 * to go and no update has been added where ours belongs.  If a new
	 * update has been added, check if our update is still permitted.
	 */
	WT_RET(__wt_page_write_gen_wrapped_check(page));
	if (old_upd != *upd_entry)
		WT_RET(__wt_txn_update_check(session, *upd_entry));

	upd->next = *upd_entry;
	/*
	 * Publish: there must be a barrier to ensure the new entry's next
	 * pointer is set before we update the linked list.
	 */
	WT_PUBLISH(*upd_entry, upd);
	__wt_update_upd_taken(args);

	/* Discard obsolete WT_UPDATE structures. */
	*upd_obsolete = __wt_update_obsolete_check(session, upd->next);

	__wt_page_and_tree_modify_set(session, page);
	return (0);
}
