# libcmc: an explicit-state model checker for C protocol code.
# Plain make, C11, no dependencies beyond libc.
#
#   make                     libcmc.a, the cmc binary, the engine tests and
#                            (Linux) the process front end's preload
#   make check               fast gates: engine tests, toy bug gate, process
#                            front end gate, purity gate and its teeth, docs
#                            gate, fast calibration, history on pg_chain's Paxos
#   make check-msan          the engine tests under MemorySanitizer (clang)
#   make check-calibration   every HC row against TLC (about 9 minutes)
#   make check-hc-bugs       planted HC port bugs must fail calibration
#   make check-spec-calibration  the .cml twins and the TLA+ reader against
#                            TLC (needs nix or the pinned java)
#   make check-pgchain       pg_chain's gate on test/pgchain: exact counts
#                            and planted bugs (about 7.5 minutes)
#   make check-all           all of the above
#   make coverage            every gate of make check on a gcc --coverage
#                            build; fails below 90% of functions or of
#                            branches in any src/*.c file (needs gcovr)
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
PRELOAD = $(if $(filter Linux,$(UNAME_S)),$(B)/cmc-preload.so $(B)/libcmc-race.a)

LIB    = $(B)/libcmc.a
TOY    = $(B)/toy0.o $(B)/toy1.o $(B)/toy2.o $(B)/toy3.o \
         $(B)/toym0.o $(B)/toym1.o $(B)/toym2.o $(B)/toym3.o
HC     = $(B)/hc_core.o $(B)/hc_glue.o $(B)/hc_inv.o
# core objects and sources the purity gate checks (step code only)
PURE   = $(B)/hc_core.o $(B)/toy0.o $(B)/toy1.o $(B)/toy2.o $(B)/toy3.o
PURE_SRC = calib/hc/hc_core.c calib/hc/hc_core.h test/toy/toy_core.c test/toy/toy.h
MSAN_B ?= build-msan
ASAN_B ?= build-asan

.PHONY: all check check-unit check-purity check-docs check-msan \
        check-calibration check-calibration-fast check-hc-bugs check-all \
        check-spec-calibration check-emit-tla check-emit-apalache \
        check-examples check-examples-slow check-asan check-pgchain check-pgchain-hist \
        check-pgchain-sym \
        codeberg-release \
        install docs-site clean

all: $(LIB) $(B)/cmc $(B)/test_cmc $(PRELOAD)

$(B):
	mkdir -p $(B)

$(B)/cmc.o: src/cmc.c include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_cli.o: src/cmc_cli.c src/cmc_proc.h src/cmc_spec.h src/cmc_tla.h include/cmc.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_proc.o: src/cmc_proc.c src/cmc_proc.h src/cmc_fs.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_fs.o: src/cmc_fs.c src/cmc_fs.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_spec.o: src/cmc_spec.c src/cmc_spec.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/cmc_tla.o: src/cmc_tla.c src/cmc_tla.h | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(LIB): $(B)/cmc.o $(B)/cmc_cli.o $(B)/cmc_proc.o $(B)/cmc_spec.o $(B)/cmc_tla.o $(B)/cmc_fs.o
	$(AR) rcs $@ $^
$(B)/cmc-preload.so: src/cmc_preload.c | $(B)
	$(CC) $(ALL_CFLAGS) -fPIC -shared -o $@ $<
# the race runtime a node built with -fsanitize=thread links instead of
# the sanitizer's own (cmc(7), THREADS)
$(B)/cmc_race.o: src/cmc_race.c | $(B)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<
$(B)/libcmc-race.a: $(B)/cmc_race.o
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

check-unit: $(B)/test_cmc $(B)/cmc $(PRELOAD) check-purity check-docs
	CMC_BUILD=$(B) $(B)/test_cmc
	test/toy_gate.sh $(B)/cmc

# man pages lint clean and cover the API and every option; the guide's
# model builds and behaves as the guide says (test/docs_gate.sh)
check-docs: $(LIB) $(B)/cmc $(PRELOAD)
	CC=$(CC) test/docs_gate.sh $(B)

check: check-unit check-examples check-calibration-fast check-pgchain-hist check-pgchain-sym

# pg_chain's cores (test/pgchain), built as pg_chain builds them: the
# correct binary and one per planted bug, $(B)/pgchain_mc-bugN
PGC = test/pgchain
PGC_SRC = $(PGC)/chain_member.c $(PGC)/paxos.c $(PGC)/chain_member_model.c \
          $(PGC)/paxos_model.c $(PGC)/pgchain_mc.c
$(B)/pgchain_mc: $(PGC_SRC) $(LIB)
	$(CC) $(ALL_CFLAGS) -I$(PGC)/include -o $@ $(PGC_SRC) $(LIB)
$(B)/pgchain_mc-bug%: $(PGC_SRC) $(LIB)
	$(CC) $(ALL_CFLAGS) -I$(PGC)/include -DPGCHAIN_MC_BUG=$* -o $@ $(PGC_SRC) $(LIB)

# history variables on pg_chain's Paxos (about a minute)
check-pgchain-hist: $(B)/pgchain_mc $(B)/pgchain_mc-bug107 $(B)/pgchain_mc-bug110
	$(PGC)/hist-gate.sh $(B)

# the fast rows of pg_chain's own gate (test/pgchain/mc-bug-gate.sh): its
# exact distinct-state counts, its witnesses, and every planted bug
# caught.  About 7.5 minutes (one row at a time), so not part of make
# check; CI runs it in its own weekly job.  PGCHAIN_MC_SLOW=1 adds the slow rows.
# Every run uses 8 BFS workers, which must change nothing.
PGC_BUGS = 1 2 3 4 5 6 7 8 9 10 101 102 103 104 105 106 107 108 109 110
check-pgchain: $(B)/pgchain_mc $(PGC_BUGS:%=$(B)/pgchain_mc-bug%)
	MC_B=$(B) PGCHAIN_MC_OUT=$(CMC_SCRATCH)/pgchain PGCHAIN_MC_EXTRA="--workers 8" \
		bash $(PGC)/mc-bug-gate.sh

# symmetry reduction on pg_chain's Paxos: every gate row and planted bug
# with --symmetry on (about five minutes)
PX_BUGS = 101 102 103 104 105 106 107 108 109 110
check-pgchain-sym: $(B)/pgchain_mc $(PX_BUGS:%=$(B)/pgchain_mc-bug%)
	$(PGC)/sym-gate.sh $(B)

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
	test/tla_gate.sh $(B)

# check-examples plus the rows too slow for every push (hc to depth 15
# and its planted bug, about 8 minutes)
check-examples-slow: $(B)/test_cmc $(B)/cmc $(PRELOAD)
	CMC_SLOW=1 CC=$(CC) test/proc_gate.sh $(B)

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

# AddressSanitizer and UBSan over the abstract checker (soundness plan
# 1.8): the spec gate, every .cml model and soundness probe, with any
# out-of-bounds access, leak or undefined operation fatal.  Separate
# build tree; the preload is not part of the spec gate.
ASAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
check-asan:
	$(MAKE) B=$(ASAN_B) CFLAGS="$(ASAN_FLAGS)" $(ASAN_B)/cmc
	CFLAGS="$(ASAN_FLAGS)" test/spec_gate.sh $(ASAN_B)
	test/tla_gate.sh $(ASAN_B)

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

check-all: check-unit check-msan check-asan check-calibration check-hc-bugs check-examples-slow \
	check-spec-calibration check-pgchain

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

# Publish the Codeberg release for a tag.  Codeberg Actions are disabled,
# so the forgejo release workflow does not run; this creates the release
# through the API and attaches the GitHub-built tarballs.  Run it after
# the GitHub release for the tag has published.  Idempotent.
#   make codeberg-release VER=1.0.0
codeberg-release:
	@test -n "$(VER)" || { echo 'usage: make codeberg-release VER=X.Y.Z'; exit 1; }
	sh scripts/codeberg-release.sh $(VER)

clean:
	rm -f $(B)/*.o $(B)/*.a $(B)/*.so $(B)/cmc $(B)/test_cmc $(B)/quickstart $(B)/quickstart-bug \
		$(B)/*.replay $(B)/probe/* $(B)/proc/*

# Coverage (docs/plans/2.1-boundaries.md, section 5).  make coverage
# builds libcmc.a, cmc, test_cmc and the in-process drivers the gates
# compile with gcc --coverage -O0 into $(COV_B), runs every gate of
# `make check` against that build, then scripts/coverage.sh: gcovr over
# src/*.c, a per-file table, and a failure unless function and branch
# coverage are each at least $(COV_MIN)% overall and in every file.
# COV_PENDING="src/a.c ..." reports the named files as PENDING instead of
# failing on them; it is for development, and a release runs with it
# empty.  make coverage-report reruns the report alone.
#
# Two kinds of object stay uninstrumented, because counters would change
# what a gate checks: the cores the purity gate reads (gcov's counters are
# writable data, which that gate rejects), and the preload and the race
# runtime that run inside every checked node (a counter dump at exit is
# file I/O inside a node).  cmc_preload.c and cmc_race.c are measured
# through in-process drivers that include their source
# (test/proc/preload_alloc.c).  The gates pass CFLAGS (exported,
# since it is set on the sub-make's command line) to every in-process
# program they build against libcmc's objects, so those link with gcov.
# COV_FLAGS carries $(WARN) because gcov matches counters to code by
# basic block and gcc's warning passes change the blocks it builds: a
# driver that includes a src/*.c file under other warning flags would
# have its branches counted a second time.
COV_B ?= build-cov
COV_CC ?= gcc
COV_MIN ?= 90
COV_PENDING ?=
# CMC_COVERAGE: the node launcher in cmc_proc.c writes its forked child's
# counts before the child's seccomp filter goes in (exec would lose them)
# -fprofile-update=atomic: BFS workers are threads, and plain counters
# lose increments to races (gcov then reports negative hits)
COV_FLAGS = $(WARN) -O0 -g --coverage -fprofile-update=atomic -DCMC_COVERAGE
COV_S = $(CMC_SCRATCH)/coverage

ifeq ($(COV_BUILD),1)
$(PURE) $(PRELOAD) $(B)/cmc_race.o: override CFLAGS = $(OPT) -g
endif

.PHONY: coverage coverage-report
coverage:
	mkdir -p $(COV_B) $(COV_S)
	find $(COV_B) $(COV_S) -name '*.gcda' -delete
	$(MAKE) B=$(COV_B) CC=$(COV_CC) CFLAGS="$(COV_FLAGS)" COV_BUILD=1 CMC_SCRATCH=$(COV_S) check
	$(MAKE) coverage-report

coverage-report:
	COV_PENDING="$(COV_PENDING)" sh scripts/coverage.sh $(COV_MIN) $(COV_B) $(COV_S)
