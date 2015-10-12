/*  Copyright (C) 2015 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <dnssec/binary.h>
#include <dnssec/crypto.h>
#include <dnssec/error.h>
#include <dnssec/key.h>
#include <dnssec/sign.h>
#include <libknot/descriptor.h>
#include <libknot/packet/wire.h>
#include <libknot/rdataset.h>
#include <libknot/rrset.h>
#include <libknot/rrtype/dnskey.h>
#include <libknot/rrtype/nsec.h>
#include <libknot/rrtype/rrsig.h>


#include "lib/defines.h"
#include "lib/dnssec/nsec.h"
#include "lib/dnssec/nsec3.h"
#include "lib/dnssec/signature.h"
#include "lib/dnssec.h"

#define DEBUG_MSG(fmt...) fprintf(stderr, fmt)

void kr_crypto_init(void)
{
	dnssec_crypto_init();
}

void kr_crypto_cleanup(void)
{
	dnssec_crypto_cleanup();
}

void kr_crypto_reinit(void)
{
	dnssec_crypto_reinit();
}

#define FLG_WILDCARD_EXPANSION 0x01 /**< Possibly generated by using wildcard expansion. */

/**
 * Check the RRSIG RR validity according to RFC4035 5.3.1 .
 * @param flags     The flags are going to be set according to validation result.
 * @param covered   RRSet to be checked.
 * @param rrsigs    RRSet containing the signatures.
 * @param sig_pos   Specifies the signature within the RRSIG RRSet.
 * @param keys      Associated DNSKEY RRSet.
 * @param key_pos   Specifies the key within the DNSKEY RRSet,
 * @param key       Parsed key (converted for direct usage).
 * @param zone_name The name of the zone cut.
 * @param timestamp Validation time.
 */
static int validate_rrsig_rr(int *flags, const knot_rrset_t *covered,
                             const knot_rrset_t *rrsigs, size_t sig_pos,
                             const knot_rrset_t *keys, size_t key_pos, const dnssec_key_t *key,
                             const knot_dname_t *zone_name, uint32_t timestamp)
{
	if (!flags || !covered || !rrsigs || !keys || !key || !zone_name) {
		return kr_error(EINVAL);
	}
	/* bullet 1 (presume same compression for the owner) */
	if ((covered->rclass != rrsigs->rclass) || !knot_dname_is_equal(covered->owner, rrsigs->owner)) {
		return kr_error(EINVAL);
	}
	/* bullet 2 */
	const knot_dname_t *signer_name = knot_rrsig_signer_name(&rrsigs->rrs, sig_pos);
	if (signer_name == NULL) {
		return kr_error(EINVAL);
	}
	if (knot_dname_cmp(signer_name, zone_name) != 0) {
		return kr_error(EINVAL);
	}
	/* bullet 3 */
	uint16_t tcovered = knot_rrsig_type_covered(&rrsigs->rrs, sig_pos);
	if (tcovered != covered->type) {
		return kr_error(EINVAL);
	}
	/* bullet 4 */
	{
		int rrsig_labels = knot_rrsig_labels(&rrsigs->rrs, sig_pos);
		int dname_labels = knot_dname_labels(covered->owner, NULL);
		if (knot_dname_is_wildcard(covered->owner)) {
			/* The asterisk does not count, RFC4034 3.1.3, paragraph 3. */
			--dname_labels;
		}
		if (rrsig_labels > dname_labels) {
			return kr_error(EINVAL);
		}
		if (rrsig_labels < dname_labels) {
			*flags |= FLG_WILDCARD_EXPANSION;
		}
	}
	/* bullet 5 */
	if (knot_rrsig_sig_expiration(&rrsigs->rrs, sig_pos) < timestamp) {
		return kr_error(EINVAL);
	}
	/* bullet 6 */
	if (knot_rrsig_sig_inception(&rrsigs->rrs, sig_pos) > timestamp) {
		return kr_error(EINVAL);
	}
	/* bullet 7 */
	if ((knot_dname_cmp(keys->owner, signer_name) != 0) ||
	    (knot_dnskey_alg(&keys->rrs, key_pos) != knot_rrsig_algorithm(&rrsigs->rrs, sig_pos)) ||
	    (dnssec_key_get_keytag(key) != knot_rrsig_key_tag(&rrsigs->rrs, sig_pos))) {
		return kr_error(EINVAL);
	}
	/* bullet 8 */
	/* Checked somewhere else. */
	/* bullet 9 and 10 */
	/* One of the requirements should be always fulfilled. */

	return kr_ok();
}

/**
 * Returns the number of labels that have been added by wildcard expansion.
 * @param expanded Expanded wildcard.
 * @param rrsigs   RRSet containing the signatures.
 * @param sig_pos  Specifies the signature within the RRSIG RRSet.
 * @return         Number of added labels, -1 on error.
 */
static int wildcard_radix_len_diff(const knot_dname_t *expanded,
                                   const knot_rrset_t *rrsigs, size_t sig_pos)
{
	if (!expanded || !rrsigs) {
		return -1;
	}

	return knot_dname_labels(expanded, NULL) - knot_rrsig_labels(&rrsigs->rrs, sig_pos);
}

int kr_rrset_validate(const knot_pkt_t *pkt, knot_section_t section_id,
                      const knot_rrset_t *covered, const knot_rrset_t *keys,
                      const knot_dname_t *zone_name, uint32_t timestamp,
                      bool has_nsec3)
{
	if (!pkt || !covered || !keys || !zone_name) {
		return kr_error(EINVAL);
	}

	for (unsigned i = 0; i < keys->rrs.rr_count; ++i) {
		int ret = kr_rrset_validate_with_key(pkt, section_id, covered, keys, i, NULL, zone_name, timestamp, has_nsec3);
		if (ret == 0) {
			return ret;
		}
	}

	return kr_error(ENOENT);
}

int kr_rrset_validate_with_key(const knot_pkt_t *pkt, knot_section_t section_id,
                               const knot_rrset_t *covered, const knot_rrset_t *keys,
                               size_t key_pos, const struct dseckey *key,
                               const knot_dname_t *zone_name, uint32_t timestamp,
                               bool has_nsec3)
{
	int ret;
	int val_flgs;
	struct dseckey *created_key = NULL;
	int trim_labels;
	if (key == NULL) {
		const knot_rdata_t *krr = knot_rdataset_at(&keys->rrs, key_pos);
		ret = kr_dnssec_key_from_rdata(&created_key, keys->owner,
			                       knot_rdata_data(krr), knot_rdata_rdlen(krr));
		if (ret != 0) {
			return ret;
		}
		key = created_key;
	}

	ret = kr_error(ENOENT);
	const knot_pktsection_t *sec = knot_pkt_section(pkt, section_id);
	for (unsigned i = 0; i < sec->count; ++i) {
		/* Try every RRSIG. */
		const knot_rrset_t *rrsig = knot_pkt_rr(sec, i);
		if (rrsig->type != KNOT_RRTYPE_RRSIG) {
			continue;
		}
		for (uint16_t j = 0; j < rrsig->rrs.rr_count; ++j) {
			val_flgs = 0;
			trim_labels = 0;
			if (validate_rrsig_rr(&val_flgs, covered, rrsig, j,
			                      keys, key_pos, (dnssec_key_t *) key,
			                      zone_name, timestamp) != 0) {
				continue;
			}
			if (val_flgs & FLG_WILDCARD_EXPANSION) {
				trim_labels = wildcard_radix_len_diff(covered->owner, rrsig, j);
				if (trim_labels < 0) {
					break;
				}
			}
			if (kr_check_signature(rrsig, j, (dnssec_key_t *) key, covered, trim_labels) != 0) {
				continue;
			}
			if (val_flgs & FLG_WILDCARD_EXPANSION) {
				if (!has_nsec3) {
					ret = kr_nsec_wildcard_answer_response_check(pkt, KNOT_AUTHORITY, covered->owner);
				} else {
					ret = kr_nsec3_wildcard_answer_response_check(pkt, KNOT_AUTHORITY, covered->owner, trim_labels - 1);
				}
				if (ret != 0) {
					continue;
				}
			}
			ret = kr_ok();
			break;
		}
		if (ret == kr_ok()) {
			break;
		}
	}

	kr_dnssec_key_free(&created_key);
	return ret;
}

int kr_dnskeys_trusted(const knot_pkt_t *pkt, knot_section_t section_id, const knot_rrset_t *keys,
                       const knot_rrset_t *ta, const knot_dname_t *zone_name, uint32_t timestamp,
                       bool has_nsec3)
{
	if (!pkt || !keys || !ta) {
		return kr_error(EINVAL);
	}

	/* RFC4035 5.2, bullet 1
	 * The supplied DS record has been authenticated.
	 * It has been validated or is part of a configured trust anchor.
	 */
	for (uint16_t i = 0; i < keys->rrs.rr_count; ++i) {
		/* RFC4035 5.3.1, bullet 8 */ /* ZSK */
		const knot_rdata_t *krr = knot_rdataset_at(&keys->rrs, i);
		const uint8_t *key_data = knot_rdata_data(krr);
		if (!kr_dnssec_key_zsk(key_data) || kr_dnssec_key_revoked(key_data)) {
			continue;
		}
		
		struct dseckey *key;
		if (kr_dnssec_key_from_rdata(&key, keys->owner, key_data, knot_rdata_rdlen(krr)) != 0) {
			continue;
		}
		if (kr_authenticate_referral(ta, (dnssec_key_t *) key) != 0) {
			kr_dnssec_key_free(&key);
			continue;
		}
		if (kr_rrset_validate_with_key(pkt, section_id, keys, keys, i, key, zone_name, timestamp, has_nsec3) != 0) {
			kr_dnssec_key_free(&key);
			continue;
		}
		kr_dnssec_key_free(&key);
		return kr_ok();
	}
	/* No useable key found */
	return kr_error(ENOENT);
}

bool kr_dnssec_key_zsk(const uint8_t *dnskey_rdata)
{
	return wire_read_u16(dnskey_rdata) & 0x0100;
}

bool kr_dnssec_key_ksk(const uint8_t *dnskey_rdata)
{
	return wire_read_u16(dnskey_rdata) & 0x0001;
}

/** Return true if the DNSKEY is revoked. */
bool kr_dnssec_key_revoked(const uint8_t *dnskey_rdata)
{
	return wire_read_u16(dnskey_rdata) & 0x0080;
}

int kr_dnssec_key_tag(uint16_t rrtype, const uint8_t *rdata, size_t rdlen)
{
	if (!rdata || rdlen == 0 || (rrtype != KNOT_RRTYPE_DS && rrtype != KNOT_RRTYPE_DNSKEY)) {
		return kr_error(EINVAL);
	}
	if (rrtype == KNOT_RRTYPE_DS) {
		return wire_read_u16(rdata);
	} else if (rrtype == KNOT_RRTYPE_DNSKEY) {
		struct dseckey *key = NULL;
		int ret = kr_dnssec_key_from_rdata(&key, NULL, rdata, rdlen);
		if (ret != 0) {
			return ret;
		}
		uint16_t keytag = dnssec_key_get_keytag((dnssec_key_t *)key);
		kr_dnssec_key_free(&key);
		return keytag;
	} else {
		return kr_error(EINVAL);
	}
}

int kr_dnssec_key_match(const uint8_t *key_a_rdata, size_t key_a_rdlen,
                        const uint8_t *key_b_rdata, size_t key_b_rdlen)
{
	dnssec_key_t *key_a = NULL, *key_b = NULL;
	int ret = kr_dnssec_key_from_rdata((struct dseckey **)&key_a, NULL, key_a_rdata, key_a_rdlen);
	if (ret != 0) {
		return ret;
	}
	ret = kr_dnssec_key_from_rdata((struct dseckey **)&key_b, NULL, key_b_rdata, key_b_rdlen);
	if (ret != 0) {
		dnssec_key_free(key_a);
		return ret;
	}
	/* If the algorithm and the public key match, we can be sure
	 * that they are the same key. */
	ret = kr_error(ENOENT);
	dnssec_binary_t pk_a, pk_b;
	if (dnssec_key_get_algorithm(key_a) == dnssec_key_get_algorithm(key_b) &&
	    dnssec_key_get_pubkey(key_a, &pk_a) == DNSSEC_EOK &&
	    dnssec_key_get_pubkey(key_b, &pk_b) == DNSSEC_EOK) {
		if (pk_a.size == pk_b.size && memcmp(pk_a.data, pk_b.data, pk_a.size) == 0) {
			ret = 0;
		}
	}
	dnssec_key_free(key_a);
	dnssec_key_free(key_b);
	return ret;
}

int kr_dnssec_key_from_rdata(struct dseckey **key, const knot_dname_t *kown, const uint8_t *rdata, size_t rdlen)
{
	if (!key || !rdata || rdlen == 0) {
		return kr_error(EINVAL);
	}

	dnssec_key_t *new_key = NULL;
	const dnssec_binary_t binary_key = {
		.size = rdlen,
		.data = (uint8_t *)rdata
	};

	int ret = dnssec_key_new(&new_key);
	if (ret != DNSSEC_EOK) {
		return kr_error(ENOMEM);
	}
	ret = dnssec_key_set_rdata(new_key, &binary_key);
	if (ret != DNSSEC_EOK) {
		dnssec_key_free(new_key);
		return kr_error(ENOMEM);
	}
	if (kown) {
		ret = dnssec_key_set_dname(new_key, kown);
		if (ret != DNSSEC_EOK) {
			dnssec_key_free(new_key);
			return kr_error(ENOMEM);
		}
	}

	*key = (struct dseckey *) new_key;
	return kr_ok();
}

void kr_dnssec_key_free(struct dseckey **key)
{
	assert(key);

	dnssec_key_free((dnssec_key_t *) *key);
	*key = NULL;
}