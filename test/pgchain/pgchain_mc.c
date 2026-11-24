/*
 * pgchain_mc.c: main() of pg_chain's model checker binary.  The engine
 * is libcmc (third_party/libcmc); the models are pg_chain's shipped
 * protocol cores.  One binary per planted-bug variant (PGCHAIN_MC_BUG).
 */
#include "cmc.h"

extern const CmcModel chain_member_model;
extern const CmcModel paxos_model;

static const CmcModelEntry models[] = {
	{"chain_member", &chain_member_model},
	{"paxos", &paxos_model},
};

int
main(int argc, char **argv)
{
	return cmc_main(argc, argv, models, sizeof(models) / sizeof(models[0]));
}
