/* IKEv2 Message ID tracking, for libreswan
 *
 * Copyright (C) 2019 Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include "lswlog.h"
#include "defs.h"
#include "state.h"
#include "demux.h"
#include "connections.h"
#include "ikev2_msgid.h"

/*
 * Logging utilities, can these share code?
 */

static void jam_v2_msgid(struct lswlog *buf,
			 struct ike_sa *ike, struct state *st,
			 const char *fmt, va_list ap)
{
	jam(buf, "Message ID: #%lu", ike->sa.st_serialno);
	if (st != NULL && IS_CHILD_SA(st)) {
		jam(buf, ".#%lu", st->st_serialno);
	}
	jam(buf, " ");
	jam_va_list(buf, fmt, ap);
	jam(buf, ";");
	const struct v2_msgid_windows *w = &ike->sa.st_v2_msgid_windows;
	jam(buf, " initiator.sent=%jd", w->initiator.sent);
	jam(buf, " initiator.recv=%jd", w->initiator.recv);
	jam(buf, " responder.sent=%jd", w->responder.sent);
	jam(buf, " responder.recv=%jd", w->responder.recv);
	if (st != NULL) {
		const struct v2_msgid_wip *wip = &st->st_v2_msgid_wip;
		jam(buf, " wip.initiator=%jd", wip->initiator);
		jam(buf, " wip.responder=%jd", wip->responder);
	}
}

void dbg_v2_msgid(struct ike_sa *ike, struct state *st,
		  const char *fmt, ...)
{
	LSWDBGP(DBG_BASE, buf) {
		va_list ap;
		va_start(ap, fmt);
		jam_v2_msgid(buf, ike, st, fmt, ap);
		va_end(ap);
	}
}

void fail_v2_msgid(const char *func, const char *file, unsigned long line,
		   struct ike_sa *ike, struct state *st,
		   const char *fmt, ...)
{
	LSWLOG_PEXPECT_SOURCE(func, file, line, buf) {
		va_list ap;
		va_start(ap, fmt);
		jam_v2_msgid(buf, ike, st, fmt, ap);
		va_end(ap);
	}
}

/*
 * Dump the MSGIDs along with any changes.
 *
 * Why not just dump the one that changed in the calling function?
 * Because MSGIDs have this strange habbit of mysteriously changing
 * between calls.
 */

static void jam_msgids(struct lswlog *buf, const char *what,
		       enum message_role message, intmax_t msgid,
		       struct ike_sa *ike, const struct v2_msgid_windows *old_windows,
		       struct state *wip, const struct v2_msgid_wip *old_wip)

{
	jam(buf, "Message ID: %s #%lu",
	    what, ike->sa.st_serialno);
	if (IS_CHILD_SA(wip)) {
		jam(buf, ".#%lu", wip->st_serialno);
	}

	switch (message) {
	case MESSAGE_REQUEST: jam(buf, " request %jd", msgid); break;
	case MESSAGE_RESPONSE: jam(buf, " response %jd", msgid); break;
	case NO_MESSAGE: break;
	default: bad_case(message);
	}

	jam(buf, "; ike:");

	jam(buf, " initiator.sent=%jd", old_windows->initiator.sent);
	if (old_windows->initiator.sent != ike->sa.st_v2_msgid_windows.initiator.sent) {
		jam(buf, "->%jd", ike->sa.st_v2_msgid_windows.initiator.sent);
	}
	jam(buf, " initiator.recv=%jd", old_windows->initiator.recv);
	if (old_windows->initiator.recv != ike->sa.st_v2_msgid_windows.initiator.recv) {
		jam(buf, "->%jd", ike->sa.st_v2_msgid_windows.initiator.recv);
	}

	jam(buf, " responder.sent=%jd", old_windows->responder.sent);
	if (old_windows->responder.sent != ike->sa.st_v2_msgid_windows.responder.sent) {
		jam(buf, "->%jd", ike->sa.st_v2_msgid_windows.responder.sent);
	}
	jam(buf, " responder.recv=%jd", old_windows->responder.recv);
	if (old_windows->responder.recv != ike->sa.st_v2_msgid_windows.responder.recv) {
		jam(buf, "->%jd", ike->sa.st_v2_msgid_windows.responder.recv);
	}

	if (IS_CHILD_SA(wip)) {
		jam(buf, "; child:");
	}
	jam(buf, " wip.initiator=%jd", old_wip->initiator);
	if (old_wip->initiator != wip->st_v2_msgid_wip.initiator) {
		jam(buf, "->%jd", wip->st_v2_msgid_wip.initiator);
	}
	jam(buf, " wip.responder=%jd", old_wip->responder);
	if (old_wip->responder != wip->st_v2_msgid_wip.responder) {
		jam(buf, "->%jd", wip->st_v2_msgid_wip.responder);
	}
}

/*
 * Maintain or reset Message IDs.
 *
 * When resetting, need to fudge things up sufficient to fool
 * ikev2_update_msgid_counters(() into thinking that this is a shiny
 * new init request.
 */

static const struct v2_msgid_windows empty_v2_msgid_windows = {
	.initiator = {
		.sent = -1,
		.recv = -1,
	},
	.responder = {
		.sent = -1,
		.recv = -1,
	},
};

static const struct v2_msgid_wip empty_v2_msgid_wip = {
	.initiator = -1,
	.responder = -1,
};

void v2_msgid_init_ike(struct ike_sa *ike)
{
	struct v2_msgid_windows old_windows = ike->sa.st_v2_msgid_windows;
	ike->sa.st_v2_msgid_windows = empty_v2_msgid_windows;
	struct v2_msgid_wip old_wip = ike->sa.st_v2_msgid_wip;
	ike->sa.st_v2_msgid_wip = empty_v2_msgid_wip;
	if (DBGP(DBG_BASE)) {
		LSWLOG_DEBUG(buf) {
			/* pretend there's a sender */
			jam_msgids(buf, "init_ike", NO_MESSAGE, -1,
				   ike, &old_windows,
				   &ike->sa, &old_wip);
		}
	}
}

void v2_msgid_init_child(struct ike_sa *ike, struct child_sa *child)
{
	child->sa.st_v2_msgid_windows = empty_v2_msgid_windows;
	struct v2_msgid_wip old_child = child->sa.st_v2_msgid_wip;
	child->sa.st_v2_msgid_wip = empty_v2_msgid_wip;
	if (DBGP(DBG_BASE)) {
		LSWLOG_DEBUG(buf) {
			/* pretend there's a sender */
			jam_msgids(buf, "init_child", NO_MESSAGE, -1,
				   ike, &ike->sa.st_v2_msgid_windows, /* unchanged */
				   &child->sa, &old_child);
		}
	}
}

void v2_msgid_start_responder(struct ike_sa *ike, struct state *responder,
			      const struct msg_digest *md)
{
	enum message_role role = v2_msg_role(md);
	if (!pexpect(role == MESSAGE_REQUEST)) {
		return;
	}
	/* extend msgid */
	intmax_t msgid = md->hdr.isa_msgid;
	const struct v2_msgid_wip wip = responder->st_v2_msgid_wip;

	if (DBGP(DBG_BASE) &&
	    responder->st_v2_msgid_wip.responder != -1) {
		FAIL_V2_MSGID(ike, responder,
			      "responder->st_v2_msgid_wip.responder=%jd == -1",
			      responder->st_v2_msgid_wip.responder);
	}
	responder->st_v2_msgid_wip.responder = msgid;
	if (DBGP(DBG_BASE)) {
		LSWLOG_DEBUG(buf) {
			jam_msgids(buf, "start-responder", role, msgid,
				   ike, &ike->sa.st_v2_msgid_windows,
				   responder, &wip);
		}
	}
}

/*
 * XXX: This is to hack around the broken code that switches from the
 * IKE SA to the CHILD SA before sending the reply.  Instead, because
 * the CHILD SA can fail, the IKE SA should be the one processing the
 * message?
 */

void v2_msgid_switch_responder(struct ike_sa *ike, struct child_sa *child,
			       const struct msg_digest *md)
{
	enum message_role role = v2_msg_role(md);
	if (!pexpect(role == MESSAGE_REQUEST)) {
		return;
	}
	intmax_t msgid = md->hdr.isa_msgid;
	/* out with the old */
	{
		const struct v2_msgid_wip wip = ike->sa.st_v2_msgid_wip;
		if (DBGP(DBG_BASE) &&
		    ike->sa.st_v2_msgid_wip.responder != msgid) {
			FAIL_V2_MSGID(ike, &child->sa,
				      "ike->sa.st_v2_msgid_wip.responder=%jd == msgid=%jd",
				      ike->sa.st_v2_msgid_wip.responder, msgid);
		}
		ike->sa.st_v2_msgid_wip.responder = -1;
		if (DBGP(DBG_BASE)) {
			LSWLOG_DEBUG(buf) {
				jam_msgids(buf, "switch-from", role, msgid,
					   ike, &ike->sa.st_v2_msgid_windows,
					   &ike->sa, &wip);
			}
		}
	}
	/* in with the new */
	{
		const struct v2_msgid_wip wip = child->sa.st_v2_msgid_wip;
		if (DBGP(DBG_BASE) &&
		    child->sa.st_v2_msgid_wip.responder != -1) {
			FAIL_V2_MSGID(ike, &child->sa,
				      "child->sa.st_v2_msgid_wip.responder=%jd == -1",
				      child->sa.st_v2_msgid_wip.responder);
		}
		child->sa.st_v2_msgid_wip.responder = msgid;
		if (DBGP(DBG_BASE)) {
			LSWLOG_DEBUG(buf) {
				jam_msgids(buf, "switch-to", role, msgid,
					   ike, &ike->sa.st_v2_msgid_windows,
					   &child->sa, &wip);
			}
		}
	}
}

void v2_msgid_switch_initiator(struct ike_sa *ike, struct child_sa *child,
			       const struct msg_digest *md)
{
	enum message_role role = v2_msg_role(md);
	if (!pexpect(role == MESSAGE_RESPONSE)) {
		return;
	}
	intmax_t msgid = md->hdr.isa_msgid;
	/* out with the old */
	{
		const struct v2_msgid_wip wip = ike->sa.st_v2_msgid_wip;
		if (DBGP(DBG_BASE) &&
		    ike->sa.st_v2_msgid_wip.initiator != msgid) {
			FAIL_V2_MSGID(ike, &child->sa,
				      "ike->sa.st_v2_msgid_wip.initiator=%jd == msgid=%jd",
				      ike->sa.st_v2_msgid_wip.initiator, msgid);
		}
		ike->sa.st_v2_msgid_wip.initiator = -1;
		if (DBGP(DBG_BASE)) {
			LSWLOG_DEBUG(buf) {
				jam_msgids(buf, "switch-from", role, msgid,
					   ike, &ike->sa.st_v2_msgid_windows,
					   &ike->sa, &wip);
			}
		}
	}
	/* in with the new */
	{
		const struct v2_msgid_wip wip = child->sa.st_v2_msgid_wip;
		if (DBGP(DBG_BASE) &&
		    child->sa.st_v2_msgid_wip.initiator != -1) {
			FAIL_V2_MSGID(ike, &child->sa,
				      "child->sa.st_v2_msgid_wip.initiator=%jd == -1",
				      child->sa.st_v2_msgid_wip.initiator);
		}
		child->sa.st_v2_msgid_wip.initiator = msgid;
		if (DBGP(DBG_BASE)) {
			LSWLOG_DEBUG(buf) {
				jam_msgids(buf, "switch-to", role, msgid,
					   ike, &ike->sa.st_v2_msgid_windows,
					   &child->sa, &wip);
			}
		}
	}
}

void v2_msgid_cancel_responder(struct ike_sa *ike, struct state *responder,
			       const struct msg_digest *md)
{
	enum message_role role = v2_msg_role(md);
	if (!pexpect(role == MESSAGE_REQUEST)) {
		return;
	}
	/* extend msgid */
	intmax_t msgid = md->hdr.isa_msgid;
	const struct v2_msgid_wip wip = responder->st_v2_msgid_wip;

	/*
	 * If an encrypted message is corrupt things bail before
	 * start_responder() but then STF_IGNORE tries to clear it.
	 */
	if (DBGP(DBG_BASE) &&
	    responder->st_v2_msgid_wip.responder != msgid) {
		FAIL_V2_MSGID(ike, responder,
			      "responder->st_v2_msgid_wip.responder=%jd == msgid=%jd",
			      responder->st_v2_msgid_wip.responder, msgid);
	}
	responder->st_v2_msgid_wip.responder = -1;
	if (DBGP(DBG_BASE)) {
		LSWLOG_DEBUG(buf) {
			jam_msgids(buf, "cancel-responder", role, msgid,
				   ike, &ike->sa.st_v2_msgid_windows,
				   responder, &wip);
		}
	}
}

void v2_msgid_update_recv(struct ike_sa *ike, struct state *receiver,
			  struct msg_digest *md)
{
	/* extend msgid */
	intmax_t msgid = md->hdr.isa_msgid;
	const struct v2_msgid_windows old = ike->sa.st_v2_msgid_windows;
	struct v2_msgid_windows *new = &ike->sa.st_v2_msgid_windows;
	const struct v2_msgid_wip old_receiver = receiver->st_v2_msgid_wip;

	enum message_role receiving = v2_msg_role(md);

	switch (receiving) {
	case MESSAGE_REQUEST:
		/*
		 * Processing request finished.  Scrub it as wip.
		 *
		 * XXX: should this done in update_sent() since it is
		 * when sending the response that things really
		 * finish?
		 */
		if (DBGP(DBG_BASE) &&
		    receiver->st_v2_msgid_wip.responder != msgid) {
			FAIL_V2_MSGID(ike, receiver,
				      "wip.responder=%jd == msgid=%jd",
				      receiver->st_v2_msgid_wip.responder, msgid);
		}
		receiver->st_v2_msgid_wip.responder = -1;
		/* last request we received */
		new->responder.recv = msgid;
		/* extend st_msgid_lastrecv */
		if (ike->sa.st_msgid_lastrecv != new->responder.recv) {
			dbg("Message ID: XXX: IKE #%lu receiver #%lu: ike.lastrecv "PRI_MSGID" != ike.responder.recv %jd",
			    ike->sa.st_serialno, receiver->st_serialno,
			    ike->sa.st_msgid_lastrecv,
			    new->responder.recv);
		}
		break;
	case MESSAGE_RESPONSE:
		/* last response we received */
		new->initiator.recv = msgid;
		/* extend st_msgid_lastack */
		if (DBGP(DBG_BASE) && ike->sa.st_msgid_lastack != new->initiator.recv) {
			FAIL_V2_MSGID(ike, receiver,
				      "ike.lastack="PRI_MSGID" == ike.initiator.recv=%jd",
				      ike->sa.st_msgid_lastack,
				      new->initiator.recv);
		}
		/*
		 * Since the response has been successfully processed,
		 * clear WIP.INITIATOR.  This way duplicate
		 * responses get discarded as there is no receiving
		 * state.
		 *
		 * XXX: Unfortunately the record 'n' send code throws
		 * a spanner in the works.  It calls update_send()
		 * before update_recv() breaking the assumption that
		 * WIP.INITIATOR is the old MSGID.
		 */
		if (old_receiver.initiator > msgid) {
			/*
			 * Hack around record 'n' send calling
			 * update_sent() (setting WIP.INITIATOR to
			 * the next request) midway through
			 * processing.
			 *
			 * Getting rid of record 'n' send will fix
			 * this hack.
			 */
			dbg("Message ID: XXX: IKE #%lu receiver #%lu: receiver.wip.initiator %jd != receiver.msgid %jd (record 'n' called update_sent() before update_recv()?)",
			    ike->sa.st_serialno, receiver->st_serialno,
			    old_receiver.initiator, msgid);
		} else {
			if (DBGP(DBG_BASE) && old_receiver.initiator != msgid) {
				FAIL_V2_MSGID(ike, receiver,
					      "receiver.wip.initiator=%jd == receiver.msgid=%jd",
					      old_receiver.initiator, msgid);
			}
			receiver->st_v2_msgid_wip.initiator = -1;
		}
		/* this is what matters */
		pexpect(receiver->st_v2_msgid_wip.initiator != msgid);
		break;
	case NO_MESSAGE:
		dbg("Message ID: IKE #%lu skipping update_recv as MD is fake",
		    ike->sa.st_serialno);
		return;
	default:
		bad_case(receiving);
	}

	if (DBGP(DBG_BASE)) {
		LSWLOG_DEBUG(buf) {
			jam_msgids(buf, "recv", receiving, msgid,
				   ike, &old, receiver, &old_receiver);
		}
	}
}

void v2_msgid_update_sent(struct ike_sa *ike, struct state *sender,
			  struct msg_digest *md, enum message_role sending)
{
	struct v2_msgid_windows old = ike->sa.st_v2_msgid_windows;
	struct v2_msgid_windows *new = &ike->sa.st_v2_msgid_windows;
	struct v2_msgid_wip old_sender = sender->st_v2_msgid_wip;
	intmax_t msgid;
	switch (sending) {
	case MESSAGE_REQUEST:
		/*
		 * pluto is initiating a new exchange.
		 *
		 * Use the next Message ID (which should be what was
		 * used by the code emitting the message request)
		 */
		msgid = new->initiator.sent + 1;
		sender->st_v2_msgid_wip.initiator = new->initiator.sent = msgid;
		/* extend st_msgid */
		if (DBGP(DBG_BASE) && sender->st_msgid != sender->st_v2_msgid_wip.initiator) {
			FAIL_V2_MSGID(ike, sender,
				      "sender.msgid="PRI_MSGID" == sender.wip.initiator=%jd",
				      sender->st_msgid,
				      sender->st_v2_msgid_wip.initiator);
		}
		/* extend st_msgid_nextuse */
		if (DBGP(DBG_BASE) && ike->sa.st_msgid_nextuse != new->initiator.sent + 1) {
			FAIL_V2_MSGID(ike, sender,
				      "ike.nextuse="PRI_MSGID" == ike.initiator.sent=%jd+1",
				      ike->sa.st_msgid_nextuse,
				      new->initiator.sent);
		}
#if 0
		/*
		 * XXX: The record 'n' send code calls update_send()
		 * before update_recv() breaking WIP.INITIATOR's
		 * expected sequence OLD-MSGID -> -1 -> NEW-MSGID.
		 */
		if (DBGP(DBG_BASE) && old_sender.initiator != -1) {
			FAIL_V2_MSGID(ike, sender,
				      "sender.wip.initiator=%jd == -1",
				      old_sender.initiator);
		}
#else
		if (old_sender.initiator != -1) {
			dbg("Message ID: XXX: IKE #%lu sender #%lu: expecting sender.wip.initiator %jd == -1 (record 'n' send out-of-order?)",
			    ike->sa.st_serialno, sender->st_serialno,
			    old_sender.initiator);
		}
#endif
		break;
	case MESSAGE_RESPONSE:
		/*
		 * pluto is responding to MD.
		 *
		 * Since this is a response, the MD's Message ID
		 * trumps what ever is in responder.sent.  This way,
		 * when messages are lost, the counter jumps forward
		 * to the most recent received.
		 */
		passert(md != NULL);
		pexpect(v2_msg_role(md) == MESSAGE_REQUEST);
		/* extend isa_msgid */
		msgid = md->hdr.isa_msgid;
		new->responder.sent = msgid;
		/* extend st_msgid_lastreplied */
		if (DBGP(DBG_BASE) && ike->sa.st_msgid_lastreplied != new->responder.sent) {
			FAIL_V2_MSGID(ike, sender,
				      "ike.lastreplied="PRI_MSGID" == ike.responder.sent=%jd",
				      ike->sa.st_msgid_lastreplied,
				      new->responder.sent);
		}
		break;
	case NO_MESSAGE:
		dbg("Message ID: IKE #%lu sender #%lu: skipping update_send as nothing to send",
		    ike->sa.st_serialno, sender->st_serialno);
		return;
	default:
		bad_case(sending);
	}

	if (DBGP(DBG_BASE)) {
		LSWLOG_DEBUG(buf) {
			jam_msgids(buf, "sent", sending, msgid,
				   ike, &old, sender, &old_sender);
		}
	}
}

#if 0
static void schedule_next_send(struct ike_sa *ike)
{
	msgid_t unack = (ike->sa.st_v2_msgid_windows.initiator.sent -
			 ike->sa.st_v2_msgid_windows.initiator.recv);
	while (unack < ike->sa.st_connection->ike_window) {
		if (ike->sa.send_next_ix == NULL) {
			break;
		}
		/* get next from list */
		so_serial_t child_so = ike->sa.send_next_ix->st_serialno;
		{
			struct initiate_list *p = ike->sa.send_next_ix;
			ike->sa.send_next_ix = p->next;
			pfree(p);
		}
		struct state *child = state_with_serialno(child_so);
		if (child == NULL) {
			dbg("can't send for #%lu using parent #%lu as it disappeared",
			    child_so, ike->sa.st_serialno);
			continue;
		}
		dbg("Message ID: scheduling CHILD SA #%lu send using IKE SA #%lu next message id="PRI_MSGID", unack="PRI_MSGID,
		    child->st_serialno, ike->sa.st_serialno,
		    ike->sa.st_v2_msgid_windows.initiator.sent + 1,
		    unack);
		event_force(EVENT_v2_SEND_NEXT_IKE, child);
		unack++;
	}
}
#endif

void schedule_next_send(struct state *st)
{
	struct initiate_list *p;
	struct state *cst = NULL;
	int i = 1;

	if (st->send_next_ix != NULL) {
		p = st->send_next_ix;
		cst = state_with_serialno(p->st_serialno);
		if (cst != NULL) {
			event_force(EVENT_v2_SEND_NEXT_IKE, cst);
			dbg("Message ID: #%lu send next using parent #%lu next message id=%u, waiting to send %d",
			    cst->st_serialno, st->st_serialno,
			    st->st_msgid_nextuse, i);
		}
		st->send_next_ix = st->send_next_ix->next;
		pfree(p);
	}
}

stf_status add_st_to_ike_sa_send_list(struct state *st, struct ike_sa *ike)
{
	msgid_t unack = ike->sa.st_msgid_nextuse - ike->sa.st_msgid_lastack - 1;
	intmax_t unack2 = ike->sa.st_v2_msgid_windows.initiator.sent - ike->sa.st_v2_msgid_windows.initiator.recv;
	if (unack != unack2) {
		dbg("Message ID: XXX: IKE #%lu expecting unack "PRI_MSGID", got %jd for #%lu",
		    ike->sa.st_serialno, unack, unack2,
		    st->st_serialno);
	}
	stf_status e = STF_OK;
	const char *what;

	if (unack < st->st_connection->ike_window) {
		what  =  "send new exchange now";
	} else  {
		e = STF_SUSPEND;
		what = "wait sending, add to send next list";
		delete_event(st);
		event_schedule_s(EVENT_SA_REPLACE, MAXIMUM_RESPONDER_WAIT, st);
		loglog(RC_LOG_SERIOUS, "message id deadlock? %s using parent #%lu unacknowledged %u next message id=%u ike exchange window %u",
			what, ike->sa.st_serialno, unack,
			ike->sa.st_msgid_nextuse,
			ike->sa.st_connection->ike_window);

		struct initiate_list **pp = &ike->sa.send_next_ix;
		while (*pp != NULL)
			pp = &(*pp)->next;
		*pp = alloc_thing(struct initiate_list, "struct initiate_list");
		**pp = (struct initiate_list) {
			.st_serialno = st->st_serialno,
			.next = NULL };

	}
	dbg("Message ID: #%lu %s using parent #%lu unacknowledged %u next message id=%u ike exchange window %u",
	    st->st_serialno,
	    what, ike->sa.st_serialno, unack,
	    ike->sa.st_msgid_nextuse,
	    ike->sa.st_connection->ike_window);
	return e;
}
