#include <string.h>
#include <sys/time.h>

#include <common/sockaddr.h>
#include "lib/context.h"
#include "lib/rplan.h"

int kr_context_init(struct kr_context *ctx, mm_ctx_t *mm)
{
	memset(ctx, 0, sizeof(struct kr_context));

	ctx->pool = mm;

	kr_rplan_init(&ctx->rplan, mm);
	kr_delegmap_init(&ctx->dp_map, mm);

	ctx->cache = kr_cache_open("/tmp/kresolved", 0, mm);

	return 0;
}

int kr_context_reset(struct kr_context *ctx)
{
	ctx->state = 0;
	ctx->resolved_qry = NULL;
	ctx->current_ns = NULL;
	ctx->query = NULL;
	kr_rplan_clear(&ctx->rplan);

	return 0;
}

int kr_context_deinit(struct kr_context *ctx)
{
	kr_delegmap_deinit(&ctx->dp_map);
	kr_cache_close(ctx->cache);

	return -1;
}

int kr_result_init(struct kr_context *ctx, struct kr_result *result)
{
	memset(result, 0, sizeof(struct kr_result));

	/* Initialize answer packet. */

	knot_pkt_t *ans = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, ctx->pool);
	if (ans == NULL) {
		return -1;
	}

	struct kr_query *qry = kr_rplan_next(&ctx->rplan);
	if (qry == NULL) {
		knot_pkt_free(&ans);
		return -1;
	}

	knot_pkt_put_question(ans, qry->sname, qry->sclass, qry->stype);
	knot_wire_set_rcode(ans->wire, KNOT_RCODE_SERVFAIL);
	knot_wire_set_qr(ans->wire);

	result->ans = ans;

	/* Start cache transaction. */
	result->txn = kr_cache_txn_begin(ctx->cache, NULL, ctx->pool);
	if (result->txn == NULL) {
		knot_pkt_free(&ans);
		return -1;
	}

	return 0;
}

int kr_result_deinit(struct kr_result *result)
{
	knot_pkt_free(&result->ans);
	kr_cache_txn_commit(result->txn);

	return 0;
}
