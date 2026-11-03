/*
 * cmc_models.c: main() of the cmc binary built by this repository: the
 * toy core and the HC calibration core.  A project using libcmc writes
 * the same few lines with its own models.
 */
#include "cmc.h"

extern const CmcModel hc_model;
extern const CmcModel toy_model;
extern const CmcModel toy_bug1_model;
extern const CmcModel toy_bug2_model;
extern const CmcModel toy_bug3_model;

static const CmcModelEntry models[] = {
	{"hc", &hc_model},
	{"toy", &toy_model},
	{"toy-bug1", &toy_bug1_model},
	{"toy-bug2", &toy_bug2_model},
	{"toy-bug3", &toy_bug3_model},
};

int
main(int argc, char **argv)
{
	return cmc_main(argc, argv, models, sizeof(models) / sizeof(models[0]));
}
