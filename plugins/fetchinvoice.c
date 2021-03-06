#include <bitcoin/chainparams.h>
#include <ccan/array_size/array_size.h>
#include <ccan/json_out/json_out.h>
#include <ccan/mem/mem.h>
#include <ccan/str/hex/hex.h>
#include <ccan/tal/str/str.h>
#include <ccan/time/time.h>
#include <ccan/utf8/utf8.h>
#include <common/blindedpath.h>
#include <common/bolt11.h>
#include <common/bolt12.h>
#include <common/bolt12_merkle.h>
#include <common/dijkstra.h>
#include <common/gossmap.h>
#include <common/json_stream.h>
#include <common/memleak.h>
#include <common/overflows.h>
#include <common/route.h>
#include <common/type_to_string.h>
#include <common/utils.h>
#include <errno.h>
#include <inttypes.h>
#include <plugins/libplugin.h>
#include <secp256k1_schnorrsig.h>

static struct gossmap *global_gossmap;
static struct node_id local_id;
static LIST_HEAD(sent_list);

struct sent {
	/* We're in sent_invreqs, awaiting reply. */
	struct list_node list;
	/* The blinding factor used by reply. */
	struct pubkey reply_blinding;
	/* The command which sent us. */
	struct command *cmd;
	/* The offer we are trying to get an invoice for. */
	struct tlv_offer *offer;
	/* The invreq we sent. */
	struct tlv_invoice_request *invreq;
};

static struct sent *find_sent(const struct pubkey *blinding)
{
	struct sent *i;

	list_for_each(&sent_list, i, list) {
		if (pubkey_eq(&i->reply_blinding, blinding))
			return i;
	}
	return NULL;
}

static const char *field_diff_(const tal_t *a, const tal_t *b,
			       const char *fieldname)
{
	/* One is set and the other isn't? */
	if ((a == NULL) != (b == NULL))
		return fieldname;
	if (!memeq(a, tal_bytelen(a), b, tal_bytelen(b)))
		return fieldname;
	return NULL;
}

#define field_diff(a, b, fieldname) \
	field_diff_(a->fieldname, b->fieldname, #fieldname)

/* Returns true if b is a with something appended. */
static bool description_is_appended(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	if (tal_bytelen(b) < tal_bytelen(a))
		return false;
	return memeq(a, tal_bytelen(a), b, tal_bytelen(a));
}

/* Hack to suppress warnings when we finish a different command */
static void discard_result(struct command_result *ret)
{
}

static struct command_result *recv_onion_message(struct command *cmd,
						 const char *buf,
						 const jsmntok_t *params)
{
	const jsmntok_t *om, *invtok, *errtok, *blindingtok;
	const u8 *invbin;
	size_t len;
	struct tlv_invoice *inv;
	struct sent *sent;
	struct sha256 merkle, sighash;
	struct json_stream *out;
	const char *badfield;
	struct pubkey blinding;
	u64 *expected_amount;

	plugin_log(cmd->plugin, LOG_INFORM, "Received onion message: %.*s",
		   json_tok_full_len(params),
		   json_tok_full(buf, params));

	om = json_get_member(buf, params, "onion_message");
	blindingtok = json_get_member(buf, om, "blinding_in");
	if (!blindingtok || !json_to_pubkey(buf, blindingtok, &blinding))
		return command_hook_success(cmd);

	sent = find_sent(&blinding);
	if (!sent) {
		plugin_log(cmd->plugin, LOG_DBG,
			   "No match for onion %.*s",
			   json_tok_full_len(om),
			   json_tok_full(buf, om));
		return command_hook_success(cmd);
	}

	/* From here on, we know it's genuine, so we will fail the
	 * fetchinvoice command if the invoice is invalid */
	errtok = json_get_member(buf, om, "invoice_error");
	if (errtok) {
		const u8 *data = json_tok_bin_from_hex(cmd, buf, errtok);
		size_t dlen = tal_bytelen(data);
		struct tlv_invoice_error *err = tlv_invoice_error_new(cmd);
		struct json_out *details = json_out_new(cmd);

		plugin_log(cmd->plugin, LOG_DBG, "errtok = %.*s",
			   json_tok_full_len(errtok),
			   json_tok_full(buf, errtok));
		json_out_start(details, NULL, '{');
		if (!fromwire_invoice_error(&data, &dlen, err)) {
			plugin_log(cmd->plugin, LOG_DBG,
				   "Invalid invoice_error %.*s",
				   json_tok_full_len(errtok),
				   json_tok_full(buf, errtok));
			json_out_addstr(details, "invoice_error_hex",
					tal_strndup(tmpctx,
						    buf + errtok->start,
						    errtok->end - errtok->start));
		} else {
			char *failstr;

			/* FIXME: with a bit more generate-wire.py support,
			 * we could have fieldnames and even types. */
			if (err->erroneous_field)
				json_out_add(details, "erroneous_field", false,
					     "%"PRIu64, *err->erroneous_field);
			if (err->suggested_value)
				json_out_addstr(details, "suggested_value",
						tal_hex(tmpctx,
							err->suggested_value));
			/* If they don't include this, it'll be empty */
			failstr = tal_strndup(tmpctx,
					      err->error,
					      tal_bytelen(err->error));
			json_out_addstr(details, "error", failstr);
		}
		json_out_end(details, '}');
		discard_result(command_done_err(sent->cmd,
						OFFER_BAD_INVREQ_REPLY,
						"Remote node sent failure message",
						details));
		return command_hook_success(cmd);
	}

	invtok = json_get_member(buf, om, "invoice");
	if (!invtok) {
		plugin_log(cmd->plugin, LOG_UNUSUAL,
			   "Neither invoice nor invoice_request_failed in reply %.*s",
			   json_tok_full_len(om),
			   json_tok_full(buf, om));
		discard_result(command_fail(sent->cmd,
					    OFFER_BAD_INVREQ_REPLY,
					    "Neither invoice nor invoice_request_failed in reply %.*s",
					    json_tok_full_len(om),
					    json_tok_full(buf, om)));
		return command_hook_success(cmd);
	}

	invbin = json_tok_bin_from_hex(cmd, buf, invtok);
	len = tal_bytelen(invbin);
	inv = tlv_invoice_new(cmd);
 	if (!fromwire_invoice(&invbin, &len, inv)) {
		badfield = "invoice";
		goto badinv;
	}

	/* BOLT-offers #12:
	 * - MUST reject the invoice unless `node_id` is equal to the offer.
	 */
	if (!pubkey32_eq(sent->offer->node_id, inv->node_id)) {
		badfield = "node_id";
		goto badinv;
	}

	/* BOLT-offers #12:
	 *   - MUST reject the invoice if `signature` is not a valid signature
	 *      using `node_id` as described in [Signature Calculation]
	 */
	merkle_tlv(inv->fields, &merkle);
	sighash_from_merkle("invoice", "signature", &merkle, &sighash);

	if (!inv->signature
	    || secp256k1_schnorrsig_verify(secp256k1_ctx, inv->signature->u8,
					   sighash.u.u8, &inv->node_id->pubkey) != 1) {
		badfield = "signature";
		goto badinv;
	}

	/* BOLT-offers #12:
	 * - MUST reject the invoice if `msat` is not present.
	 */
	if (!inv->amount) {
		badfield = "amount";
		goto badinv;
	}

	/* BOLT-offers #12:
	 * - MUST reject the invoice unless `offer_id` is equal to the id of the
	 *   offer.
	 */
	if ((badfield = field_diff(sent->invreq, inv, offer_id)))
		goto badinv;

	/* BOLT-offers #12:
	 * - if the invoice is a reply to an `invoice_request`:
	 *...
	 *   - MUST reject the invoice unless the following fields are equal or
	 *     unset exactly as they are in the `invoice_request:`
	 *     - `quantity`
	 *     - `recurrence_counter`
	 *     - `recurrence_start`
	 *     - `payer_key`
	 *     - `payer_info`
	 */
	if ((badfield = field_diff(sent->invreq, inv, quantity)))
		goto badinv;
	if ((badfield = field_diff(sent->invreq, inv, recurrence_counter)))
		goto badinv;
	if ((badfield = field_diff(sent->invreq, inv, recurrence_start)))
		goto badinv;
	if ((badfield = field_diff(sent->invreq, inv, payer_key)))
		goto badinv;
	if ((badfield = field_diff(sent->invreq, inv, payer_info)))
		goto badinv;

	/* Get the amount we expected. */
	if (sent->offer->amount && !sent->offer->currency) {
		expected_amount = tal(tmpctx, u64);

		*expected_amount = *sent->offer->amount;
		if (sent->invreq->quantity) {
			/* We should never have sent this! */
			if (mul_overflows_u64(*expected_amount,
					      *sent->invreq->quantity)) {
				badfield = "quantity overflow";
				goto badinv;
			}
			*expected_amount *= *sent->invreq->quantity;
		}
	} else
		expected_amount = NULL;

	/* BOLT-offers #12:
	 * - if the offer contained `recurrence`:
	 *   - MUST reject the invoice if `recurrence_basetime` is not set.
	 */
	if (sent->invreq->recurrence_counter && !inv->recurrence_basetime) {
		badfield = "recurrence_basetime";
		goto badinv;
	}

	/* BOLT-offers #12:
	 * - SHOULD confirm authorization if the `description` does not exactly
	 *   match the `offer`
	 *   - MAY highlight if `description` has simply had a change appended.
	 */
	/* We highlight these changes to the caller, for them to handle */
	out = jsonrpc_stream_success(sent->cmd);
	json_add_string(out, "invoice", invoice_encode(tmpctx, inv));
	json_object_start(out, "changes");
	if (field_diff(sent->offer, inv, description)) {
		/* Did they simply append? */
		if (description_is_appended(sent->offer->description,
					    inv->description)) {
			size_t off = tal_bytelen(sent->offer->description);
			json_add_stringn(out, "description_appended",
					 inv->description + off,
					 tal_bytelen(inv->description) - off);
		} else if (!inv->description)
			json_add_stringn(out, "description_removed",
					 sent->offer->description,
					 tal_bytelen(sent->offer->description));
		else
			json_add_stringn(out, "description",
					 inv->description,
					 tal_bytelen(inv->description));
	}

	/* BOLT-offers #12:
	 * - SHOULD confirm authorization if `vendor` does not exactly
	 *   match the `offer`
	 */
	if (field_diff(sent->offer, inv, vendor)) {
		if (!inv->vendor)
			json_add_stringn(out, "vendor_removed",
					 sent->offer->vendor,
					 tal_bytelen(sent->offer->vendor));
		else
			json_add_stringn(out, "vendor",
					 inv->vendor,
					 tal_bytelen(inv->vendor));
	}
	/* BOLT-offers #12:
	 *   - SHOULD confirm authorization if `msat` is not within the amount
	 *     range authorized.
	 */
	/* We always tell them this unless it's trivial to calc and
	 * exactly as expected. */
	if (!expected_amount || *inv->amount != *expected_amount)
		json_add_amount_msat_only(out, "msat",
					  amount_msat(*inv->amount));
	json_object_end(out);

	/* We tell them about next period at this point, if any. */
	if (sent->offer->recurrence) {
		u64 next_counter, next_period_idx;
		u64 paywindow_start, paywindow_end;

		next_counter = *sent->invreq->recurrence_counter + 1;
		if (sent->invreq->recurrence_start)
			next_period_idx = *sent->invreq->recurrence_start
				+ next_counter;
		else
			next_period_idx = next_counter;

		/* If this was the last, don't tell them about a next! */
		if (!sent->offer->recurrence_limit
		    || next_period_idx <= *sent->offer->recurrence_limit) {
			json_object_start(out, "next_period");
			json_add_u64(out, "counter", next_counter);
			json_add_u64(out, "starttime",
				     offer_period_start(*inv->recurrence_basetime,
							next_period_idx,
							sent->offer->recurrence));
			json_add_u64(out, "endtime",
				     offer_period_start(*inv->recurrence_basetime,
							next_period_idx + 1,
							sent->offer->recurrence) - 1);

			offer_period_paywindow(sent->offer->recurrence,
					       sent->offer->recurrence_paywindow,
					       sent->offer->recurrence_base,
					       *inv->recurrence_basetime,
					       next_period_idx,
					       &paywindow_start, &paywindow_end);
			json_add_u64(out, "paywindow_start", paywindow_start);
			json_add_u64(out, "paywindow_end", paywindow_end);
			json_object_end(out);
		}
	}

	discard_result(command_finished(sent->cmd, out));
	return command_hook_success(cmd);

badinv:
	plugin_log(cmd->plugin, LOG_DBG, "Failed invoice due to %s", badfield);
	discard_result(command_fail(sent->cmd,
				    OFFER_BAD_INVREQ_REPLY,
				    "Incorrect %s field in %.*s",
				    badfield,
				    json_tok_full_len(invtok),
				    json_tok_full(buf, invtok)));
	return command_hook_success(cmd);
}

static void destroy_sent(struct sent *sent)
{
	list_del(&sent->list);
}

static struct command_result *sendonionmsg_done(struct command *cmd,
						const char *buf UNUSED,
						const jsmntok_t *result UNUSED,
						struct sent *sent)
{
	/* FIXME: timeout! */
	sent->cmd = cmd;
	list_add_tail(&sent_list, &sent->list);
	tal_add_destructor(sent, destroy_sent);
	return command_still_pending(cmd);
}

static void init_gossmap(struct plugin *plugin)
{
	global_gossmap
		= notleak_with_children(gossmap_load(NULL,
						     GOSSIP_STORE_FILENAME));
	if (!global_gossmap)
		plugin_err(plugin, "Could not load gossmap %s: %s",
			   GOSSIP_STORE_FILENAME, strerror(errno));
}

static struct gossmap *get_gossmap(struct plugin *plugin)
{
	if (!global_gossmap)
		init_gossmap(plugin);
	else
		gossmap_refresh(global_gossmap);
	return global_gossmap;
}

static struct command_result *param_offer(struct command *cmd,
					  const char *name,
					  const char *buffer,
					  const jsmntok_t *tok,
					  struct tlv_offer **offer)
{
	char *fail;

	/* BOLT-offers #12:
	 * - if `features` contains unknown _odd_ bits that are non-zero:
	 *  - MUST ignore the bit.
	 * - if `features` contains unknown _even_ bits that are non-zero:
	 *  - MUST NOT respond to the offer.
	 *  - SHOULD indicate the unknown bit to the user.
	 */
	/* BOLT-offers #12:
	 *   - MUST NOT set or imply any `chain_hash` not set or implied by
	 *     the offer.
	 */
	*offer = offer_decode(cmd, buffer + tok->start, tok->end - tok->start,
			      plugin_feature_set(cmd->plugin), chainparams,
			      &fail);
	if (!*offer)
		return command_fail_badparam(cmd, name, buffer, tok,
					     tal_fmt(cmd,
						     "Unparsable offer: %s",
						     fail));

	/* BOLT-offers #12:
	 *
	 *  - if `node_id`, `description` or `signature` is not set:
	 *    - MUST NOT respond to the offer.
	 */
	/* Note: offer_decode checks `signature` */
	if (!(*offer)->node_id)
		return command_fail_badparam(cmd, name, buffer, tok,
					     "Offer does not contain a node_id");

	if (!(*offer)->description)
		return command_fail_badparam(cmd, name, buffer, tok,
					     "Offer does not contain a description");
	return NULL;
}

static bool can_carry_onionmsg(const struct gossmap *map,
			       const struct gossmap_chan *c,
			       int dir,
			       struct amount_msat amount UNUSED,
			       void *arg UNUSED)
{
	const struct gossmap_node *n;
	/* Don't use it if either side says it's disabled */
	if (!c->half[dir].enabled || !c->half[!dir].enabled)
		return false;

	/* Check features of recipient */
	n = gossmap_nth_node(map, c, !dir);
	return n && gossmap_node_get_feature(map, n, OPT_ONION_MESSAGES) != -1;
}

/* make_blindedpath only needs pubkeys */
static const struct pubkey *route_backwards(const tal_t *ctx,
					    const struct gossmap *gossmap,
					    struct route **r)
{
	struct pubkey *rarr;

	rarr = tal_arr(ctx, struct pubkey, tal_count(r));
	for (size_t i = 0; i < tal_count(r); i++) {
		const struct gossmap_node *dst;
		struct node_id id;

		dst = gossmap_nth_node(gossmap, r[i]->c, r[i]->dir);
		gossmap_node_get_id(gossmap, dst, &id);
		/* We're going backwards */
		if (!pubkey_from_node_id(&rarr[tal_count(rarr) - 1 - i], &id))
			abort();
	}

	return rarr;
}

static struct command_result *send_message(struct command *cmd,
					   struct sent *sent,
					   const char *msgfield,
					   const u8 *msgval)
{
	const struct dijkstra *dij;
	const struct gossmap_node *dst, *src;
	struct route **r;
	struct gossmap *gossmap = get_gossmap(cmd->plugin);
	const struct pubkey *backwards;
	struct onionmsg_path **path;
	struct pubkey blinding;
	struct out_req *req;
	struct node_id dstid;

	/* FIXME: Use blinded path if avail. */
	gossmap_guess_node_id(gossmap, sent->offer->node_id, &dstid);
	dst = gossmap_find_node(gossmap, &dstid);
	if (!dst)
		return command_fail(cmd, LIGHTNINGD,
				    "Unknown destination %s",
				    type_to_string(tmpctx, struct node_id,
						   &dstid));

	/* If we don't exist in gossip, routing can't happen. */
	src = gossmap_find_node(gossmap, &local_id);
	if (!src)
		return command_fail(cmd, PAY_ROUTE_NOT_FOUND,
				    "We don't have any channels");

	dij = dijkstra(tmpctx, gossmap, dst, AMOUNT_MSAT(0), 0,
		       can_carry_onionmsg, route_score_shorter, NULL);

	r = route_from_dijkstra(tmpctx, gossmap, dij, src);
	if (!r)
		/* FIXME: We need to retry kind of like keysend here... */
		return command_fail(cmd, OFFER_ROUTE_NOT_FOUND,
				    "Can't find route");

	/* Ok, now make reply for onion_message */
	backwards = route_backwards(tmpctx, gossmap, r);
	path = make_blindedpath(tmpctx, backwards, &blinding,
				&sent->reply_blinding);

	req = jsonrpc_request_start(cmd->plugin, cmd, "sendonionmessage",
				    &sendonionmsg_done,
				    &forward_error,
				    sent);
	json_array_start(req->js, "hops");
	for (size_t i = 0; i < tal_count(r); i++) {
		struct node_id id;

		json_object_start(req->js, NULL);
		gossmap_node_get_id(gossmap,
				    gossmap_nth_node(gossmap, r[i]->c, !r[i]->dir),
				    &id);
		json_add_node_id(req->js, "id", &id);
		if (i == tal_count(r) - 1)
			json_add_hex_talarr(req->js, msgfield, msgval);
		json_object_end(req->js);
	}
	json_array_end(req->js);

	json_object_start(req->js, "reply_path");
	json_add_pubkey(req->js, "blinding", &blinding);
	json_array_start(req->js, "path");
	for (size_t i = 0; i < tal_count(path); i++) {
		json_object_start(req->js, NULL);
		json_add_pubkey(req->js, "id", &path[i]->node_id);
		if (path[i]->enctlv)
			json_add_hex_talarr(req->js, "enctlv", path[i]->enctlv);
		json_object_end(req->js);
	}
	json_array_end(req->js);
	json_object_end(req->js);
	return send_outreq(cmd->plugin, req);
}

static struct command_result *invreq_done(struct command *cmd,
					  const char *buf,
					  const jsmntok_t *result,
					  struct tlv_offer *offer)
{
	const jsmntok_t *t;
	struct sent *sent;
	char *fail;
	u8 *rawinvreq;

	/* We need to remember both offer and invreq to check reply. */
	sent = tal(cmd, struct sent);
	sent->offer = tal_steal(sent, offer);

	/* Get invoice request */
	t = json_get_member(buf, result, "bolt12");
	if (!t)
		return command_fail(cmd, LIGHTNINGD,
				    "Missing bolt12 %.*s",
				    json_tok_full_len(result),
				    json_tok_full(buf, result));

	plugin_log(cmd->plugin, LOG_DBG,
		   "invoice_request: %.*s",
		   json_tok_full_len(t),
		   json_tok_full(buf, t));

	sent->invreq = invrequest_decode(sent,
					 buf + t->start,
					 t->end - t->start,
					 plugin_feature_set(cmd->plugin),
					 chainparams,
					 &fail);
	if (!sent->invreq)
		return command_fail(cmd, LIGHTNINGD,
				    "Invalid invoice_request %.*s: %s",
				    json_tok_full_len(t),
				    json_tok_full(buf, t),
				    fail);

	rawinvreq = tal_arr(tmpctx, u8, 0);
	towire_invoice_request(&rawinvreq, sent->invreq);
	return send_message(cmd, sent, "invoice_request", rawinvreq);
}

/* Fetches an invoice for this offer, and makes sure it corresponds. */
static struct command_result *json_fetchinvoice(struct command *cmd,
						const char *buffer,
						const jsmntok_t *params)
{
	struct tlv_offer *offer;
	struct amount_msat *msat;
	const char *rec_label;
	struct out_req *req;
	struct tlv_invoice_request *invreq;

	invreq = tlv_invoice_request_new(cmd);

	if (!param(cmd, buffer, params,
		   p_req("offer", param_offer, &offer),
		   p_opt("msatoshi", param_msat, &msat),
		   p_opt("quantity", param_u64, &invreq->quantity),
		   p_opt("recurrence_counter", param_number,
			 &invreq->recurrence_counter),
		   p_opt("recurrence_start", param_number,
			 &invreq->recurrence_start),
		   p_opt("recurrence_label", param_string, &rec_label),
		   NULL))
		return command_param_failed();

	/* BOLT-offers #12:
	 *  - MUST set `offer_id` to the merkle root of the offer as described
	 *    in [Signature Calculation](#signature-calculation).
	 */
	invreq->offer_id = tal(invreq, struct sha256);
	merkle_tlv(offer->fields, invreq->offer_id);

	/* Check if they are trying to send us money. */
	if (offer->send_invoice)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Offer wants an invoice, not invoice_request");

	/* BOLT-offers #12:
	 * - SHOULD not respond to an offer if the current time is after
	 *   `absolute_expiry`.
	 */
	if (offer->absolute_expiry
	    && time_now().ts.tv_sec > *offer->absolute_expiry)
		return command_fail(cmd, OFFER_EXPIRED, "Offer expired");

	/* BOLT-offers #12:
	 * - if the offer did not specify `amount`:
	 *   - MUST specify `amount`.`msat` in multiples of the minimum
	 *     lightning-payable unit (e.g. milli-satoshis for bitcoin) for the
	 *     first `chains` entry.
	 * - otherwise:
	 *   - MUST NOT set `amount`
	 */
	if (offer->amount) {
		if (msat)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "msatoshi parameter unnecessary");
	} else {
		if (!msat)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "msatoshi parameter required");
		invreq->amount = tal_dup(invreq, u64,
					 &msat->millisatoshis); /* Raw: tu64 */
	}

	/* BOLT-offers #12:
	 *   - if the offer had a `quantity_min` or `quantity_max` field:
	 *     - MUST set `quantity`
	 *     - MUST set it within that (inclusive) range.
	 *   - otherwise:
	 *     - MUST NOT set `quantity`
	 */
	if (offer->quantity_min || offer->quantity_max) {
		if (!invreq->quantity)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "quantity parameter required");
		if (offer->quantity_min
		    && *invreq->quantity < *offer->quantity_min)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "quantity must be >= %"PRIu64,
					    *offer->quantity_min);
		if (offer->quantity_max
		    && *invreq->quantity > *offer->quantity_max)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "quantity must be <= %"PRIu64,
					    *offer->quantity_max);
	} else {
		if (invreq->quantity)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "quantity parameter unnecessary");
	}

	/* BOLT-offers #12:
	 * - if the offer contained `recurrence`:
	 */
	if (offer->recurrence) {
		/* BOLT-offers #12:
		 *    - for the initial request:
		 *...
		 *      - MUST set `recurrence_counter` `counter` to 0.
		 */
		/* BOLT-offers #12:
		 *    - for any successive requests:
		 *...
		 *      - MUST set `recurrence_counter` `counter` to one greater
		 *        than the highest-paid invoice.
		 */
		if (!invreq->recurrence_counter)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "needs recurrence_counter");

		/* BOLT-offers #12:
		 *    - if the offer contained `recurrence_base` with
		 *      `start_any_period` non-zero:
		 *      - MUST include `recurrence_start`
		 *...
		 *    - otherwise:
		 *      - MUST NOT include `recurrence_start`
		 */
		if (offer->recurrence_base
		    && offer->recurrence_base->start_any_period) {
			if (!invreq->recurrence_start)
				return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
						    "needs recurrence_start");
		} else {
			if (invreq->recurrence_start)
				return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
						    "unnecessary recurrence_start");
		}

		/* recurrence_label uniquely identifies this series of
		 * payments. */
		if (!rec_label)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "needs recurrence_label");

		/* FIXME! */
		/* BOLT-offers #12:
		 * - SHOULD NOT send an `invoice_request` for a period which has
		 *   already passed.
		 */
		/* If there's no recurrence_base, we need the initial payment
		 * for this... */
	} else {
		/* BOLT-offers #12:
		 * - otherwise:
		 *   - MUST NOT set `recurrence_counter`.
		 *...
		 *   - MUST NOT set `recurrence_start`
		 */
		if (invreq->recurrence_counter)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "unnecessary recurrence_counter");
		if (invreq->recurrence_start)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "unnecessary recurrence_start");
	}

	/* BOLT-offers #12:
	 *
	 * - if the chain for the invoice is not solely bitcoin:
	 *   - MUST specify `chains` the offer is valid for.
	 * - otherwise:
	 *   - the bitcoin chain is implied as the first and only entry.
	 */
	if (!streq(chainparams->network_name, "bitcoin")) {
		invreq->chains = tal_arr(invreq, struct bitcoin_blkid, 1);
		invreq->chains[0] = chainparams->genesis_blockhash;
	}

	invreq->features
		= plugin_feature_set(cmd->plugin)->bits[BOLT11_FEATURE];

	/* Make the invoice request (fills in payer_key and payer_info) */
	req = jsonrpc_request_start(cmd->plugin, cmd, "createinvoicerequest",
				    &invreq_done,
				    &forward_error,
				    offer);
	json_add_string(req->js, "bolt12", invrequest_encode(tmpctx, invreq));
	if (rec_label)
		json_add_string(req->js, "recurrence_label", rec_label);
	return send_outreq(cmd->plugin, req);
}

static const struct plugin_command commands[] = { {
	"fetchinvoice",
	"payment",
	"Request remote node for an invoice for this {offer}, with {amount}, {quanitity}, {recurrence_counter}, {recurrence_start} and {recurrence_label} iff required.",
	NULL,
	json_fetchinvoice,
	}
};

static void init(struct plugin *p, const char *buf UNUSED,
		 const jsmntok_t *config UNUSED)
{
	rpc_scan(p, "getinfo",
		 take(json_out_obj(NULL, NULL, NULL)),
		 "{id:%}", JSON_SCAN(json_to_node_id, &local_id));
}

static const struct plugin_hook hooks[] = {
	{
		"onion_message_blinded",
		recv_onion_message
	},
};

int main(int argc, char *argv[])
{
	setup_locale();
	plugin_main(argv, init, PLUGIN_RESTARTABLE, true, NULL,
		    commands, ARRAY_SIZE(commands),
		    /* No notifications */
	            NULL, 0,
		    hooks, ARRAY_SIZE(hooks),
		    /* No options */
		    NULL);
}
