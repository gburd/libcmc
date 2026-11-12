# libcmc: an explicit-state model checker for C protocol code.
# Plain make, C11, no dependencies beyond libc.
#
#   make                     libcmc.a, the cmc binary, the engine tests and
#                            (Linux) the process front end's preload
#   make check               fast gates: engine tests, toy bug gate, process
#                            front end gate, purity gate and its teeth, docs
#                            gate, fast calibration
#   make check-msan          the engine tests under MemorySanitizer (clang)
#   make check-calibration   every HC row against TLC (about 9 minutes)
#   make check-hc-bugs       planted HC port bugs must fail calibration
#   make check-all           all of the above
#   make CC=clang check
#   make install             cmc, libcmc.a, the preload, cmc.h and the man
#                            pages under $(PREFIX)
#   make docs-site           the documentation site, into docs/_site
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
PREFIX ?= /usr/local
DESTDIR ?=
export CMC_SCRATCH
UNAME_S := $(shell uname -s)
# the process front end's preload (Linux only): the deterministic heap
# and the logical clock every checked node runs under
PRELOAD = $(if $(filter Linux,$(UNAME_S)),$(B)/cmc-preload.so)

LIB    = $(B)/libcmc.a
TOY    = $(B)/toy0.o $(B)/toy1.o $(B)/toy2.o $(B)/toy3.o \
         $(B)/toym0.o $(B)/toym1.o $(B)/toym2.o $(B)/toym3.o
HC     = $(B)/hc_core.o $(B)/hc_glue.o $(B)/hc_inv.o
# core objects and sources the purity gate checks (step code only)
PURE   = $(B)/hc_core.o $(B)/toy0.o $(B)/toy1.o $(B)/toy2.o $(B)/toy3.o
PURE_SRC = calib/hc/hc_core.c calib/hc/hc_core.h test/toy/toy_core.c test/toy/toy.h
MSAN_B ?= build-msan

.PHONY: all check check-unit check-purity check-docs check-msan \
        check-calibration check-calibration-fast check-hc-bugs check-all \
        check-spec-calibration check-emit-tla check-emit-apalache \
        check-examples \
        install docs-site clean

all: $(LIB) $(B)/cmc $(B)/test_cmc $(PRELOAD)

$(B):
	mkdir -p $(B)

$(B)/cmc.o: src/cmc.c include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_cli.o: src/cmc_cli.c src/cmc_proc.h src/cmc_spec.h include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_proc.o: src/cmc_proc.c src/cmc_proc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_spec.o: src/cmc_spec.c src/cmc_spec.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(LIB): $(B)/cmc.o $(B)/cmc_cli.o $(B)/cmc_proc.o $(B)/cmc_spec.o
	$(AR) rcs $@ $^
$(B)/cmc-preload.so: src/cmc_preload.c | $(B)
	$(CC) $(ALL_CFLAGS) -fPIC -shared -o $@ $<

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

check-unit: $(B)/test_cmc $(B)/cmc $(PRELOAD) check-purity check-docs
	CMC_BUILD=$(B) $(B)/test_cmc
	test/toy_gate.sh $(B)/cmc

# man pages lint clean and cover the API and every option; the guide's
# model builds and behaves as the guide says (test/docs_gate.sh)
check-docs: $(LIB)
	CC=$(CC) test/docs_gate.sh $(B)

check: check-unit check-examples check-calibration-fast

# every shipped example runs and gives its expected verdict: the C
# process-mode examples (examples/*/) built and checked by proc_gate,
# and the abstract .cml models (examples/spec/) by spec_gate.  This is a
# named gate so a release cannot ship an example that no longer runs;
# the release workflow depends on `make check`, which depends on this.
# (Both scripts also run under check-unit; naming the dependency here
# makes "the examples ran" an explicit release gate, not a side effect.)
check-examples: $(B)/test_cmc $(B)/cmc $(PRELOAD)
	CC=$(CC) test/proc_gate.sh $(B)
	test/spec_gate.sh $(B)

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

# the abstract-model checker against live single-worker TLC: cmc --spec
# and TLC must agree on verdict, distinct-state count and shortest-
# counterexample depth for every .cml fixture with a .tla twin.  Needs
# nix (tlc); run under `nix develop`.
check-spec-calibration: $(B)/cmc
	test/spec_calibrate.sh $(B)/cmc

# the TLA+ bridge against live TLC: cmc --emit-tla must produce a module
# TLC accepts and that agrees with cmc --spec.  Needs nix (tlc).
check-emit-tla: $(B)/cmc
	test/emit_gate.sh $(B)/cmc

# the TLA+ bridge against Apalache (the symbolic, Z3-backed engine):
# cmc --emit-tla must produce a module Apalache type-checks and checks
# with the same verdict as cmc --spec.  Apalache is optional (not in
# nixpkgs); the gate fetches it once to $CMC_SCRATCH, or set APALACHE_MC.
# Needs Java 21+ (uses nix jdk21 if the host java is older).
check-emit-apalache: $(B)/cmc
	test/apalache_gate.sh $(B)/cmc

check-all: check-unit check-msan check-calibration check-hc-bugs

install: $(LIB) $(B)/cmc $(PRELOAD)
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include \
		$(DESTDIR)$(PREFIX)/share/man/man1 $(DESTDIR)$(PREFIX)/share/man/man3 \
		$(DESTDIR)$(PREFIX)/share/man/man7
	cp $(B)/cmc $(DESTDIR)$(PREFIX)/bin/
	cp $(LIB) $(DESTDIR)$(PREFIX)/lib/
	$(if $(PRELOAD),mkdir -p $(DESTDIR)$(PREFIX)/lib/libcmc && cp $(PRELOAD) $(DESTDIR)$(PREFIX)/lib/libcmc/)
	cp include/cmc.h $(DESTDIR)$(PREFIX)/include/
	cp man/man1/cmc.1 $(DESTDIR)$(PREFIX)/share/man/man1/
	cp man/man3/libcmc.3 $(DESTDIR)$(PREFIX)/share/man/man3/
	cp man/man7/cmc.7 $(DESTDIR)$(PREFIX)/share/man/man7/

# needs mandoc, ruby and bundler (nix develop has them)
docs-site:
	sh docs/gen_man_pages.sh
	cd docs && bundle install --quiet && bundle exec jekyll build --destination _site

clean:
	rm -f $(B)/*.o $(B)/*.a $(B)/*.so $(B)/cmc $(B)/test_cmc $(B)/quickstart $(B)/quickstart-bug \
		$(B)/*.replay $(B)/probe/* $(B)/proc/*
