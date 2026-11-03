/*
 * quickstart.c: a complete libcmc model in one file, the one the guide
 * walks through (docs/guide/02-first-model.md).  A client writes a value
 * to a server.  The server makes the write durable, then acknowledges it.
 * The property: once the server has restarted after a crash, every write
 * the client saw acknowledged is still on its disk.
 *
 *   cc -std=c11 -Ilibcmc/include quickstart.c libcmc/build/libcmc.a -o quickstart
 *   ./quickstart --model quickstart --all-props --crashes 1
 *
 * Built with -DQUICKSTART_BUG, the server acknowledges a write before it
 * is durable, and one crash loses an acknowledged write.  make check-docs
 * builds both and checks what the guide says about them (test/docs_gate.sh).
 */
#include "cmc.h"

#include <string.h>

/* !region state */
enum
{
	SERVER = 0, CLIENT = 1		/* node numbers */
};
enum
{
	M_WRITE = 1, M_ACK			/* message kinds, body[0] */
};

/* Every node has this layout.  The first byte is durable. */
typedef struct QsState
{
	uint8_t		value;			/* durable: the server's stored value */
	uint8_t		restarted;		/* the server has come back from a crash */
	uint8_t		writer;			/* whom to acknowledge once the write is durable */
	uint8_t		pad;
} QsState;
/* !endregion state */

/* !region step */
static void
qs_send(CmcOut *out, int dst, uint8_t kind, uint8_t val)
{
	CmcOutput  *o = cmc_out_push(out, CMC_OUT_SEND);

	o->dst = (uint8_t) dst;
	o->len = 2;
	o->body[0] = kind;
	o->body[1] = val;
}

static void
qs_step(const void *cfg, int node, void *sv, const CmcEvent *ev, CmcOut *out)
{
	QsState    *s = sv;

	(void) cfg;
	if (node == CLIENT)
	{
		if (ev->kind == CMC_EV_CLIENT)
			qs_send(out, SERVER, M_WRITE, 1);
		else if (ev->kind == CMC_EV_RECV && ev->body[0] == M_ACK)
			cmc_out_push(out, CMC_OUT_REPLY)->arg = ev->body[1];
		return;
	}
	switch (ev->kind)
	{
		case CMC_EV_RECV:
			if (ev->body[0] == M_WRITE)
			{
				CmcOutput  *p = cmc_out_push(out, CMC_OUT_PERSIST);

				p->len = 1;
				p->body[0] = ev->body[1];
				s->writer = ev->src;
#ifdef QUICKSTART_BUG
				qs_send(out, ev->src, M_ACK, ev->body[1]);	/* not durable yet */
#endif
			}
			break;
		case CMC_EV_FLUSHED:
#ifndef QUICKSTART_BUG
			qs_send(out, s->writer, M_ACK, ev->body[0]);
#endif
			break;
		case CMC_EV_RESTART:
			s->restarted = 1;
			break;
		default:
			break;
	}
}

/* The engine calls this when a persist reaches disk: only it may change
 * the durable byte. */
static void
qs_apply_persist(const void *cfg, int node, void *durable, const uint8_t *body,
				 uint16_t len)
{
	(void) cfg;
	(void) node;
	(void) len;
	((QsState *) durable)->value = body[0];
}
/* !endregion step */

/* !region core */
static void
qs_cfg_default(void *cfg)
{
	(void) cfg;
}

static int
qs_cfg_set(void *cfg, const char *key, const char *val)
{
	(void) cfg;
	(void) key;
	(void) val;
	return -1;					/* this core has no settings */
}

static int
qs_nnodes(const void *cfg)
{
	(void) cfg;
	return 2;
}

static void
qs_init(const void *cfg, int node, void *sv)
{
	(void) cfg;
	(void) node;
	memset(sv, 0, sizeof(QsState));
}

static int
qs_clients(const void *cfg, int node)
{
	(void) cfg;
	return node == CLIENT;		/* only the client takes CLIENT events */
}

static void
qs_describe(const void *cfg, int node, const void *sv, FILE *f)
{
	const QsState *s = sv;

	(void) cfg;
	if (node == SERVER)
		fprintf(f, "server value=%u restarted=%u", s->value, s->restarted);
	else
		fprintf(f, "client");
}

static void
qs_describe_msg(const void *cfg, const uint8_t *body, FILE *f)
{
	(void) cfg;
	fprintf(f, "%s(%u)", body[0] == M_WRITE ? "WRITE" : "ACK", body[1]);
}

static const CmcCore qs_core = {
	.name = "quickstart",
	.state_size = sizeof(QsState),
	.durable_size = 1,
	.cfg_default = qs_cfg_default,
	.cfg_set = qs_cfg_set,
	.nnodes = qs_nnodes,
	.init = qs_init,
	.step = qs_step,
	.apply_persist = qs_apply_persist,
	.describe = qs_describe,
	.describe_msg = qs_describe_msg,
	.clients = qs_clients,
};
/* !endregion core */

/* !region props */
/* After a restart, every acknowledged write is still on the server's disk. */
static bool
no_lost_ack(const CmcWorld *w, CmcWhy *why)
{
	const QsState *s = cmc_node(w, SERVER);

	for (int i = 0; i < cmc_nreplies(w); i++)
	{
		int			who;
		uint32_t	v;

		cmc_reply(w, i, &who, &v);
		if (s->restarted && s->value != v)
		{
			cmc_why(why, "write %u was acknowledged, but the server restarted with value %u",
					v, s->value);
			return false;
		}
	}
	return true;
}

/* Some run gets a write acknowledged at all (else NoLostAck holds vacuously). */
static bool
acked(const CmcWorld *w, CmcWhy *why)
{
	(void) why;
	return cmc_nreplies(w) > 0;
}

static const CmcProp qs_props[] = {
	CMC_INVARIANT(NoLostAck, no_lost_ack),
	CMC_WITNESS_PROP(Acked, acked),
};

static const CmcModel qs_model = {
	.name = "quickstart",
	.core = &qs_core,
	.props = qs_props,
	.nprops = sizeof(qs_props) / sizeof(qs_props[0]),
};
/* !endregion props */

/* !region main */
int
main(int argc, char **argv)
{
	static const CmcModelEntry models[] = {
		{"quickstart", &qs_model},
	};

	return cmc_main(argc, argv, models, sizeof(models) / sizeof(models[0]));
}
/* !endregion main */
