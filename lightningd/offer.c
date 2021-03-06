#include <common/bolt12.h>
#include <common/bolt12_merkle.h>
#include <common/json_command.h>
#include <common/json_helpers.h>
#include <common/jsonrpc_errors.h>
#include <common/param.h>
#include <hsmd/hsmd_wiregen.h>
#include <lightningd/jsonrpc.h>
#include <lightningd/lightningd.h>
#include <wallet/wallet.h>
#include <wire/wire_sync.h>

static void json_populate_offer(struct json_stream *response,
				const struct sha256 *offer_id,
				const char *b12,
				const struct json_escape *label,
				enum offer_status status)
{
	json_add_sha256(response, "offer_id", offer_id);
	json_add_bool(response, "active", offer_status_active(status));
	json_add_bool(response, "single_use", offer_status_single(status));
	json_add_string(response, "bolt12", b12);
	json_add_bool(response, "used", status == OFFER_USED);
	if (label)
		json_add_escaped_string(response, "label", label);
}

static struct command_result *param_b12_offer(struct command *cmd,
					      const char *name,
					      const char *buffer,
					      const jsmntok_t *tok,
					      struct tlv_offer **offer)
{
	char *fail;
	*offer = offer_decode_nosig(cmd, buffer + tok->start,
				    tok->end - tok->start,
				    cmd->ld->our_features, chainparams, &fail);
	if (!*offer)
		return command_fail_badparam(cmd, name, buffer, tok, fail);
	if ((*offer)->signature)
		return command_fail_badparam(cmd, name, buffer, tok,
					     "must be unsigned offer");
	return NULL;
}

static void hsm_sign_b12_offer(struct lightningd *ld,
			       const struct sha256 *merkle,
			       struct bip340sig *sig)
{
	u8 *msg;

	msg = towire_hsmd_sign_bolt12(NULL, "offer", "signature", merkle, NULL);

	if (!wire_sync_write(ld->hsm_fd, take(msg)))
		fatal("Could not write to HSM: %s", strerror(errno));

	msg = wire_sync_read(tmpctx, ld->hsm_fd);
        if (!fromwire_hsmd_sign_bolt12_reply(msg, sig))
		fatal("HSM gave bad sign_offer_reply %s",
		      tal_hex(msg, msg));
}

static struct command_result *json_createoffer(struct command *cmd,
					       const char *buffer,
					       const jsmntok_t *obj UNNEEDED,
					       const jsmntok_t *params)
{
	struct json_stream *response;
	struct json_escape *label;
	struct tlv_offer *offer;
	struct sha256 merkle;
	const char *b12str;
	bool *single_use;
	enum offer_status status;

	if (!param(cmd, buffer, params,
		   p_req("bolt12", param_b12_offer, &offer),
		   p_opt("label", param_label, &label),
		   p_opt_def("single_use", param_bool, &single_use, false),
		   NULL))
		return command_param_failed();

	if (*single_use)
		status = OFFER_SINGLE_USE;
	else
		status = OFFER_MULTIPLE_USE;
 	merkle_tlv(offer->fields, &merkle);
	offer->signature = tal(offer, struct bip340sig);
	hsm_sign_b12_offer(cmd->ld, &merkle, offer->signature);
	b12str = offer_encode(cmd, offer);
	if (!wallet_offer_create(cmd->ld->wallet, &merkle, b12str, label,
				 status)) {
		return command_fail(cmd,
				    OFFER_ALREADY_EXISTS,
				    "Duplicate offer");
	}

	response = json_stream_success(cmd);
	json_populate_offer(response, &merkle, b12str, label, status);
	return command_success(cmd, response);
}

static const struct json_command createoffer_command = {
	"createoffer",
	"payment",
	json_createoffer,
	"Create and sign an offer {bolt12} with and optional {label}."
};
AUTODATA(json_command, &createoffer_command);

static struct command_result *json_listoffers(struct command *cmd,
					       const char *buffer,
					       const jsmntok_t *obj UNNEEDED,
					       const jsmntok_t *params)
{
	struct sha256 *offer_id;
	struct json_stream *response;
	struct wallet *wallet = cmd->ld->wallet;
	const char *b12;
	const struct json_escape *label;
	bool *active_only;
	enum offer_status status;

	if (!param(cmd, buffer, params,
		   p_opt("offer_id", param_sha256, &offer_id),
		   p_opt_def("active_only", param_bool, &active_only, false),
		   NULL))
		return command_param_failed();

	response = json_stream_success(cmd);
	json_array_start(response, "offers");
	if (offer_id) {
		b12 = wallet_offer_find(tmpctx, wallet, offer_id, &label,
					&status);
		if (b12 && offer_status_active(status) >= *active_only) {
			json_object_start(response, NULL);
			json_populate_offer(response,
					    offer_id, b12, label, status);
			json_object_end(response);
		}
	} else {
		struct db_stmt *stmt;
		struct sha256 id;

		for (stmt = wallet_offer_id_first(cmd->ld->wallet, &id);
		     stmt;
		     stmt = wallet_offer_id_next(cmd->ld->wallet, stmt, &id)) {
			b12 = wallet_offer_find(tmpctx, wallet, &id,
						&label, &status);
			if (offer_status_active(status) >= *active_only) {
				json_object_start(response, NULL);
				json_populate_offer(response,
						    &id, b12, label, status);
				json_object_end(response);
			}
		}
	}
	json_array_end(response);
	return command_success(cmd, response);
}

static const struct json_command listoffers_command = {
	"listoffers",
	"payment",
	json_listoffers,
	"If {offer_id} is set, show that."
	" Otherwise, if {showdisabled} is true, list all, otherwise just non-disabled ones."
};
AUTODATA(json_command, &listoffers_command);

static struct command_result *json_disableoffer(struct command *cmd,
						const char *buffer,
						const jsmntok_t *obj UNNEEDED,
						const jsmntok_t *params)
{
	struct json_stream *response;
	struct sha256 *offer_id;
	struct wallet *wallet = cmd->ld->wallet;
	const char *b12;
	const struct json_escape *label;
	enum offer_status status;

	if (!param(cmd, buffer, params,
		   p_req("offer_id", param_sha256, &offer_id),
		   NULL))
		return command_param_failed();

	b12 = wallet_offer_find(tmpctx, wallet, offer_id, &label, &status);
	if (!b12)
		return command_fail(cmd, LIGHTNINGD, "Unknown offer");

	if (!offer_status_active(status))
		return command_fail(cmd, OFFER_ALREADY_DISABLED,
				    "offer is not active");
	status = wallet_offer_disable(wallet, offer_id, status);

	response = json_stream_success(cmd);
	json_populate_offer(response, offer_id, b12, label, status);
	return command_success(cmd, response);
}

static const struct json_command disableoffer_command = {
	"disableoffer",
	"payment",
	json_disableoffer,
	"Disable offer {offer_id}",
};
AUTODATA(json_command, &disableoffer_command);

/* We do some sanity checks now, since we're looking up prev payment anyway,
 * but our main purpose is to fill in invreq->payer_info tweak. */
static struct command_result *prev_payment(struct command *cmd,
					   const char *label,
					   struct tlv_invoice_request *invreq)
{
	const struct wallet_payment **payments;
	bool prev_paid = false;

	assert(!invreq->payer_info);
	payments = wallet_payment_list(cmd, cmd->ld->wallet, NULL);

	for (size_t i = 0; i < tal_count(payments); i++) {
		const struct tlv_invoice *inv;
		char *fail;

		/* FIXME: Restrict db queries instead */
		if (!payments[i]->label || !streq(label, payments[i]->label))
			continue;

		if (!payments[i]->invstring)
			continue;

		inv = invoice_decode(tmpctx, payments[i]->invstring,
				     strlen(payments[i]->invstring),
				     NULL, chainparams, &fail);
		if (!inv)
			continue;

		/* They can reuse labels across different offers. */
		if (!sha256_eq(inv->offer_id, invreq->offer_id))
			continue;

		/* Be paranoid, in case someone inserts their own
		 * clashing label! */
		if (!inv->recurrence_counter)
			continue;

		/* BOLT-offers #12:
		 * - if the offer contained `recurrence_base` with
		 *   `start_any_period` non-zero:
		 *   - MUST include `recurrence_start`
		 *   - MUST set `period_offset` to the period the sender wants
		 *     for the initial request
		 *   - MUST set `period_offset` to the same value on all
		 *     following requests.
		 */
		if (invreq->recurrence_start) {
			if (!inv->recurrence_start)
				return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
						    "unexpected"
						    " recurrence_start");
			if (*inv->recurrence_start != *invreq->recurrence_start)
				return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
						    "recurrence_start was"
						    " previously %u",
						    *inv->recurrence_start);
		} else {
			if (inv->recurrence_start)
				return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
						    "missing"
						    " recurrence_start");
		}

		if (*inv->recurrence_counter == *invreq->recurrence_counter-1) {
			if (payments[i]->status == PAYMENT_COMPLETE)
				prev_paid = true;
		}

		if (inv->payer_info)
			invreq->payer_info
				= tal_dup_talarr(invreq, u8, inv->payer_info);
	}

	if (!invreq->payer_info)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "No previous payment attempted for this"
				    " label and offer");

	if (!prev_paid)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "previous invoice has not been paid");

	return NULL;
}

static struct command_result *param_b12_invreq(struct command *cmd,
					       const char *name,
					       const char *buffer,
					       const jsmntok_t *tok,
					       struct tlv_invoice_request **invreq)
{
	char *fail;

	*invreq = invrequest_decode(cmd, buffer + tok->start,
				    tok->end - tok->start,
				    cmd->ld->our_features, chainparams, &fail);
	if (!*invreq)
		return command_fail_badparam(cmd, name, buffer, tok, fail);
	if ((*invreq)->payer_info)
		return command_fail_badparam(cmd, name, buffer, tok,
					     "must not have payer_info");
	if ((*invreq)->payer_key)
		return command_fail_badparam(cmd, name, buffer, tok,
					     "must not have payer_key");
	return NULL;
}

static struct command_result *json_createinvoicerequest(struct command *cmd,
							const char *buffer,
							const jsmntok_t *obj,
							const jsmntok_t *params)
{
	struct tlv_invoice_request *invreq;
	const char *label;
	struct sha256 tweakhash;
	struct json_stream *response;
	secp256k1_pubkey tweaked;

	if (!param(cmd, buffer, params,
		   p_req("bolt12", param_b12_invreq, &invreq),
		   p_opt("recurrence_label", param_escaped_string, &label),
		   NULL))
		return command_param_failed();

	if (invreq->recurrence_counter) {
		if (!label)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "Need payment label for recurring payments");

		if (*invreq->recurrence_counter != 0) {
			struct command_result *err
				= prev_payment(cmd, label, invreq);
			if (err)
				return err;
		}
	}

	if (!invreq->payer_info) {
		/* BOLT-offers #12:
		 * `payer_info` might typically contain information about the
		 * derivation of the `payer_key`.  This should not leak any
		 * information (such as using a simple BIP-32 derivation
		 * path); a valid system might be for a node to maintain a
		 * base payer key, and encode a 128-bit tweak here.  The
		 * payer_key would be derived by tweaking the base key with
		 * SHA256(payer_base_pubkey || tweak).
		 */
		invreq->payer_info = tal_arr(invreq, u8, 16);
		randombytes_buf(invreq->payer_info,
				tal_bytelen(invreq->payer_info));
	}

	payer_key_tweak(&cmd->ld->bolt12_base,
			invreq->payer_info, tal_bytelen(invreq->payer_info),
			&tweakhash);

	/* Tweaking gives a not-x-only pubkey, must then convert. */
	if (secp256k1_xonly_pubkey_tweak_add(secp256k1_ctx,
					     &tweaked,
					     &cmd->ld->bolt12_base.pubkey,
					     tweakhash.u.u8) != 1) {
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Invalid tweak");
	}
	invreq->payer_key = tal(invreq, struct pubkey32);
	if (secp256k1_xonly_pubkey_from_pubkey(secp256k1_ctx,
					       &invreq->payer_key->pubkey,
					       NULL, &tweaked) != 1) {
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Invalid tweaked key");
	}

	/* BOLT-offers #12:
	 * - if the offer contained `recurrence`:
	 *...
	 *   - MUST set `recurrence_signature` `sig` as detailed in
	 *    [Signature Calculation](#signature-calculation) using the
	 *    `payer_key`.
	 */
	if (invreq->recurrence_counter) {
		struct sha256 merkle;
		u8 *msg;

		/* This populates the ->fields from our entries */
		invreq->fields = tlv_make_fields(invreq, invoice_request);
		merkle_tlv(invreq->fields, &merkle);

		msg = towire_hsmd_sign_bolt12(NULL,
					      "invoice_request",
					      "recurrence_signature",
					      &merkle, invreq->payer_info);
		if (!wire_sync_write(cmd->ld->hsm_fd, take(msg)))
			fatal("Could not write to HSM: %s", strerror(errno));

		msg = wire_sync_read(tmpctx, cmd->ld->hsm_fd);
		invreq->recurrence_signature = tal(invreq, struct bip340sig);
		if (!fromwire_hsmd_sign_bolt12_reply(msg,
						     invreq->recurrence_signature))
			fatal("HSM gave bad sign_offer_reply %s",
			      tal_hex(msg, msg));

		/* FIXME: Validate signature! */
	}

	response = json_stream_success(cmd);
	json_add_string(response, "bolt12", invrequest_encode(tmpctx, invreq));
	if (label)
		json_add_escaped_string(response, "recurrence_label",
					take(json_escape(NULL, label)));
	return command_success(cmd, response);
}

static const struct json_command createinvreq_command = {
	"createinvoicerequest",
	"payment",
	json_createinvoicerequest,
	"Create and sign an invoice_request {bolt12}, with {recurrence_label} if recurring, filling in payer_info and payer_key."
};
AUTODATA(json_command, &createinvreq_command);
