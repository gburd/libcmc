/*
 * hc_glue.c: everything of the HC calibration core that is not the pure
 * step/action code: configuration parsing, Init, trace descriptions and
 * the CmcCore vtable.  hc_core.c holds Next and the action effects and
 * calls nothing outside itself but mem* (the purity gate checks this).
 */
#include "cmc.h"
#include "hc_core.h"

#include <stdlib.h>
#include <string.h>

#define BIT(n)	(1u << ((n) - 1))

/* ---- vtable ---------------------------------------------------------- */

static void
hc_cfg_default(void *cfgv)
{
	HcCfg	   *c = cfgv;

	c->n = 3;
	c->maxe = 3;
	c->writes = 1;
	c->cp = true;
	c->code = true;
	c->confirm = false;
	c->zerf = false;
}

static int
parse_bool(const char *v, bool *out)
{
	if (!strcmp(v, "TRUE") || !strcmp(v, "true") || !strcmp(v, "1"))
		*out = true;
	else if (!strcmp(v, "FALSE") || !strcmp(v, "false") || !strcmp(v, "0"))
		*out = false;
	else
		return -1;
	return 0;
}

static int
hc_cfg_set(void *cfgv, const char *k, const char *v)
{
	HcCfg	   *c = cfgv;
	char		buf[64];
	size_t		l;

	/* strip TLC quotes */
	snprintf(buf, sizeof(buf), "%s", v);
	l = strlen(buf);
	if (l >= 2 && buf[0] == '"' && buf[l - 1] == '"')
	{
		memmove(buf, buf + 1, l - 2);
		buf[l - 2] = 0;
	}
	v = buf;
	if (!strcmp(k, "N"))
	{
		c->n = atoi(v);
		return c->n >= 1 && c->n <= HC_MAXN ? 0 : -1;
	}
	if (!strcmp(k, "MaxEpoch"))
	{
		c->maxe = atoi(v);
		return c->maxe >= 1 && c->maxe <= HC_MAXE ? 0 : -1;
	}
	if (!strcmp(k, "Writes"))
	{
		/* a count, or a TLC set literal {w1, w2} */
		if (v[0] == '{')
		{
			int			cnt = 0;

			for (const char *p = v; *p; p++)
				if (*p == 'w')
					cnt++;
			c->writes = cnt;
		}
		else
			c->writes = atoi(v);
		return c->writes >= 0 && c->writes <= HC_MAXW ? 0 : -1;
	}
	if (!strcmp(k, "CPMode"))
		return parse_bool(v, &c->cp);
	if (!strcmp(k, "Confirm"))
		return parse_bool(v, &c->confirm);
	if (!strcmp(k, "Zerf"))
		return parse_bool(v, &c->zerf);
	if (!strcmp(k, "tlc_mult"))
		return parse_bool(v, &c->tlc_mult);
	if (!strcmp(k, "UnanimityRule"))
	{
		if (!strcmp(v, "code"))
			c->code = true;
		else if (!strcmp(v, "paper"))
			c->code = false;
		else
			return -1;
		return 0;
	}
	return -1;
}

static int
hc_nnodes(const void *cfg)
{
	(void) cfg;
	return 1;					/* direct mode: one shared state */
}

/* ---- descriptions ------------------------------------------------------ */

static void
put_proj(HcProj P, FILE *f)
{
	HcPj		p;

	hc_unpack(P, &p);
	fprintf(f, "[e%d a%d <", p.e, p.a);
	for (int i = 0; i < p.ulen; i++)
		fprintf(f, "%s%d", i ? "," : "", p.upi[i]);
	fprintf(f, "|");
	for (int i = 0; i < p.rlen; i++)
		fprintf(f, "%s%d", i ? "," : "", p.rep[i]);
	fprintf(f, ">]");
}

static void
hc_describe(const void *cfgv, int node, const void *sv, FILE *f)
{
	const HcCfg *c = cfgv;
	const HcState *s = sv;
	bool		first = true;

	(void) node;
	fprintf(f, "  net = {");
	for (int i = 1; i <= c->n; i++)
		for (int j = i + 1; j <= c->n; j++)
			if (hc_reach(c, s, i, j))
			{
				fprintf(f, "%s%d-%d", first ? "" : ", ", i, j);
				first = false;
			}
	fprintf(f, "}\n");
	for (int n = 1; n <= c->n; n++)
	{
		fprintf(f, "  node %d: cur=", n);
		put_proj(s->cur[n - 1], f);
		fprintf(f, " conf={");
		first = true;
		for (int e = 1; e <= c->maxe; e++)
			if (s->conf[n - 1] & BIT(e))
			{
				fprintf(f, "%s%d", first ? "" : ",", e);
				first = false;
			}
		fprintf(f, "} hist={");
		first = true;
		for (int w = 1; w <= c->writes; w++)
			if (s->hist[n - 1] & BIT(w))
			{
				fprintf(f, "%sw%d", first ? "" : ",", w);
				first = false;
			}
		fprintf(f, "}%s\n    pub:", hc_wedged(c, s, n) ? " WEDGED" : "");
		for (int e = 1; e <= c->maxe; e++)
		{
			fprintf(f, " ");
			if (s->pub[n - 1][e - 1])
				put_proj(s->pub[n - 1][e - 1], f);
			else
				fprintf(f, "-");
		}
		fprintf(f, "\n    priv:");
		for (int e = 1; e <= c->maxe; e++)
		{
			fprintf(f, " ");
			if (s->priv[n - 1][e - 1])
				put_proj(s->priv[n - 1][e - 1], f);
			else
				fprintf(f, "-");
		}
		fprintf(f, "\n    stamp:");
		for (int i = 0; i < s->nstamp[n - 1]; i++)
		{
			fprintf(f, " <w%d,", HC_WP_W(s->stamp[n - 1][i]));
			put_proj(HC_WP_P(s->stamp[n - 1][i]), f);
			fprintf(f, ">");
		}
		fprintf(f, "\n");
	}
	fprintf(f, "  props:");
	for (int i = 0; i < s->nprops; i++)
	{
		fprintf(f, " ");
		put_proj(s->props[i], f);
	}
	fprintf(f, "\n  acked:");
	for (int i = 0; i < s->nacked; i++)
	{
		fprintf(f, " <w%d,", HC_WP_W(s->acked[i]));
		put_proj(HC_WP_P(s->acked[i]), f);
		fprintf(f, ">");
	}
	if (s->bad)
		fprintf(f, "\n  BAD (capacity exceeded)");
}

static void
hc_describe_action(const void *cfgv, CmcAction a, FILE *f)
{
	unsigned	A = CMC_ACT_A(a),
				B = CMC_ACT_B(a),
				W = CMC_ACT_C(a);
	uint32_t	D = CMC_ACT_D(a);

	(void) cfgv;
	switch (CMC_ACT_KIND(a))
	{
		case HC_NETCHANGE:
			fprintf(f, "NetChange net-mask=%#x", D);
			return;
		case HC_PROPOSE:
			fprintf(f, "Propose(%u) ", A);
			put_proj(D, f);
			return;
		case HC_ADOPT:
			fprintf(f, "Adopt(%u) ", A);
			put_proj(D, f);
			return;
		case HC_DOCONFIRM:
			fprintf(f, "DoConfirm(%u)", A);
			return;
		case HC_DELIVER:
			fprintf(f, "Deliver(");
			put_proj(D, f);
			fprintf(f, ", %u)", A);
			return;
		case HC_READREPAIR:
			fprintf(f, "ReadRepair(%u, %u) ", A, B);
			put_proj(D, f);
			return;
		case HC_REPAIR:
			fprintf(f, "Repair(%u, %u)", A, B);
			return;
		case HC_CLIENTWRITE:
			fprintf(f, "ClientWrite(%u, w%u)", A, W);
			return;
		case HC_FORWARD:
			fprintf(f, "Forward(%u, %u, w%u) ", A, B, W);
			put_proj(D, f);
			return;
		case HC_ACK:
			fprintf(f, "Ack(w%u, ", W);
			put_proj(D, f);
			fprintf(f, ")");
			return;
	}
	fprintf(f, "?%016llx", (unsigned long long) a);
}

const CmcCore hc_core = {
	.name = "hc",
	.state_size = sizeof(HcState),
	.durable_size = 0,
	.cfg_size = sizeof(HcCfg),
	.cfg_default = hc_cfg_default,
	.cfg_set = hc_cfg_set,
	.nnodes = hc_nnodes,
	.init = hc_init,
	.step = hc_step,
	.describe = hc_describe,
	.direct_actions = hc_actions,
	.describe_action = hc_describe_action,
};
