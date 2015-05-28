#ifndef LIGHTNING_PKT_H
#define LIGHTNING_PKT_H
/* Simple (non-threadsafe!) wrapper for protobufs.
 *
 * This could be a simple set of macros, if the protobuf-c people hadn't
 * insisted on "prettifing" the names they generate into CamelCase.
 */
#include "lightning.pb-c.h"
#include <ccan/endian/endian.h>
#include <ccan/short_types/short_types.h>
#include <ccan/tal/tal.h>

/* A packet, ready to be de-protobuf'ed. */
struct pkt {
	le32 len;
	u8 data[];
};

/* Utility helper: dies if there's a problem. */
Pkt *pkt_from_file(const char *filename, Pkt__PktCase expect);

struct sha256;
struct bitcoin_compressed_pubkey;

/**
 * openchannel_pkt - create an openchannel message
 * @ctx: tal context to allocate off.
 * @seed: psuedo-random seed to shuffle inputs.
 * @revocation_hash: first hash value generated from seed.
 * @script_len, @script: the script which pays to us.
 * @commitment_fee: the fee to use for commitment tx.
 * @rel_locktime_seconds: relative seconds for commitment locktime.
 * @anchor: the anchor transaction details.
 */
struct pkt *openchannel_pkt(const tal_t *ctx,
			    u64 seed,
			    const struct sha256 *revocation_hash,
			    size_t script_len,
			    const void *script,
			    u64 commitment_fee,
			    u32 rel_locktime_seconds,
			    Anchor *anchor);

/**
 * open_anchor_sig_pkt - create an open_anchor_sig message
 * @ctx: tal context to allocate off.
 * @sigs: the der-encoded signatures (tal_count() gives len).
 * @num_sigs: the number of sigs.
 */
struct pkt *open_anchor_sig_pkt(const tal_t *ctx, u8 **sigs, size_t num_sigs);

/* Useful helper for allocating & populating a protobuf Sha256Hash */
Sha256Hash *sha256_to_proto(const tal_t *ctx, const struct sha256 *hash);
void proto_to_sha256(const Sha256Hash *pb, struct sha256 *hash);

BitcoinPubkey *pubkey_to_proto(const tal_t *ctx,
			       const struct bitcoin_compressed_pubkey *key);
#endif /* LIGHTNING_PKT_H */
