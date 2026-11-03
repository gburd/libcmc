# libcmc: an explicit-state model checker for C protocol code.
# Plain make, C11, no dependencies beyond libc.
#
#   make                     libcmc.a, the cmc binary and the engine tests
#   make check               fast gates: engine tests, toy bug gate, purity
#                            gate and its teeth, fast calibration rows
#   make check-msan          the engine tests under MemorySanitizer (clang)
#   make check-calibration   every HC row against TLC (about 9 minutes)
#   make check-hc-bugs       planted HC port bugs must fail calibration
#   make check-all           all of the above
#   make CC=clang check
#
# Build output goes to $(B) (default build/, gitignored).  Logs and
# replay files go to $(CMC_SCRATCH) (default ~/tmp/cmc-build).

CC ?= cc
AR ?= ar
B ?= build
CMC_SCRATCH ?= $(HOME)/tmp/cmc-build
OPT ?= -O2
CFLAGS ?= $(OPT) -g
WARN = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wshadow \
       -Wstrict-prototypes -Wmissing-prototypes -Wno-missing-field-initializers
ALL_CFLAGS = $(WARN) -Iinclude $(CFLAGS)
export CMC_SCRATCH

LIB    = $(B)/libcmc.a
TOY    = $(B)/toy0.o $(B)/toy1.o $(B)/toy2.o $(B)/toy3.o \
         $(B)/toym0.o $(B)/toym1.o $(B)/toym2.o $(B)/toym3.o
HC     = $(B)/hc_core.o $(B)/hc_glue.o $(B)/hc_inv.o
# core objects and sources the purity gate checks (step code only)
PURE   = $(B)/hc_core.o $(B)/toy0.o $(B)/toy1.o $(B)/toy2.o $(B)/toy3.o
PURE_SRC = calib/hc/hc_core.c calib/hc/hc_core.h test/toy/toy_core.c test/toy/toy.h
MSAN_B ?= build-msan

.PHONY: all check check-unit check-purity check-msan check-calibration \
        check-calibration-fast check-hc-bugs check-all clean

all: $(LIB) $(B)/cmc $(B)/test_cmc

$(B):
	mkdir -p $(B)

$(B)/cmc.o: src/cmc.c include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_cli.o: src/cmc_cli.c include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(LIB): $(B)/cmc.o $(B)/cmc_cli.o
	$(AR) rcs $@ $^

$(B)/cmc_models.o: test/cmc_models.c include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/test_cmc.o: test/test_cmc.c include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/hc_%.o: calib/hc/hc_%.c calib/hc/hc_core.h include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
# one object per CMC_TOY_BUG variant: toyN.o (pure step code), toymN.o (model)
$(B)/toy%.o: test/toy/toy_core.c test/toy/toy.h include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -DCMC_TOY_BUG=$* -c -o $@ $<
$(B)/toym%.o: test/toy/toy_model.c test/toy/toy.h include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -DCMC_TOY_BUG=$* -DTOY_MODEL_NAME=$(if $(filter 0,$*),toy_model,toy_bug$*_model) -c -o $@ $<

$(B)/cmc: $(B)/cmc_models.o $(HC) $(TOY) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $^
$(B)/test_cmc: $(B)/test_cmc.o $(TOY) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $^

# Purity gate (scripts/purity_gate.sh): core step code calls nothing but
# mem*, has no writable or TLS data, no clock/randomness/syscall
# instructions, no asm and no out->o[ indexing; the engine includes no
# core's private header.  purity_teeth.sh: each probe must be rejected.
check-purity: $(PURE)
	scripts/purity_gate.sh "$(PURE)" "$(PURE_SRC)"
	scripts/purity_teeth.sh $(CC) $(B)

check-unit: $(B)/test_cmc $(B)/cmc check-purity
	CMC_BUILD=$(B) $(B)/test_cmc
	test/toy_gate.sh $(B)/cmc

check: check-unit check-calibration-fast

# MemorySanitizer (docs/research/02 M2): the engine tests and the toy gate
# under clang -fsanitize=memory, plus the uninitialised-read probe, which
# must be reported.  Separate build tree.
MSAN_FLAGS = -O1 -g -fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer
check-msan:
	$(MAKE) B=$(MSAN_B) CC=clang CFLAGS="$(MSAN_FLAGS)" $(MSAN_B)/test_cmc $(MSAN_B)/cmc
	CMC_BUILD=$(MSAN_B) $(MSAN_B)/test_cmc
	test/toy_gate.sh $(MSAN_B)/cmc
	$(MSAN_B)/cmc --model hc --tlc-cfg calib/hc/cfg/hc_code_cp_n3_S2r_NoStaleRead.cfg --det --progress 0 --quiet | grep -q "S2r_NoStaleRead is violated"
	clang $(WARN) -Iinclude $(MSAN_FLAGS) -o $(MSAN_B)/probe_uninit test/probe/uninit.c src/cmc.c
	@if MSAN_OPTIONS=exitcode=77 $(MSAN_B)/probe_uninit > $(MSAN_B)/probe_uninit.txt 2>&1; then \
	  echo "check-msan: HOLE: the uninitialised read in test/probe/uninit.c was not reported"; exit 1; \
	elif [ $$? = 77 ] && grep -q 'use-of-uninitialized-value' $(MSAN_B)/probe_uninit.txt; then \
	  echo "check-msan: ok (probe reported: $$(grep -m1 -oE 'in copy_garbage [^ ]*uninit.c:[0-9]+' $(MSAN_B)/probe_uninit.txt))"; \
	else echo "check-msan: probe failed unexpectedly"; cat $(MSAN_B)/probe_uninit.txt; exit 1; fi

check-calibration: $(B)/cmc
	calib/hc/calibrate.sh $(B)/cmc all

# the failing configurations only (about a minute)
check-calibration-fast: $(B)/cmc
	calib/hc/calibrate.sh $(B)/cmc fast

# the calibration's own teeth: planted HC port bugs must fail calibration
check-hc-bugs:
	calib/hc/hc_bug_gate.sh

check-all: check-unit check-msan check-calibration check-hc-bugs

clean:
	rm -f $(B)/*.o $(B)/*.a $(B)/cmc $(B)/test_cmc $(B)/*.replay $(B)/probe/*
