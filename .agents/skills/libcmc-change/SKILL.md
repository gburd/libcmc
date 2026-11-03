---
name: libcmc-change
description: >
  How to change libcmc without weakening it: the engine (src/cmc.c), the
  API (include/cmc.h), the CLI, the purity gate, the toy and HC cores, or
  the calibration. Use before any edit there, when adding an environment
  action (TICK, LINKS, APPLY), when a calibration row moves, or when
  reviewing a libcmc change. Triggers on: "cmc", "libcmc", "model
  checker", "calibration", "purity gate", "fingerprint", "replay",
  "actor mode", "new action", "liveness".
---

# Changing libcmc

## Steps

1. **Name the risk.**  Write one sentence on how this change could make
   cmc miss a real bug: a new action enabled from core state, state kept
   outside the world, a cap that truncates, a search-order change that
   breaks shortest traces.  If you cannot name the risk, read
   `docs/DESIGN.md` again before you start.
2. **Red first.**  Add the test that pins the behaviour.  For a
   soundness fix, it is a test or probe on which today's engine gives the
   wrong verdict (`test/test_cmc.c`, or `test/probe/` with a teeth
   script).  Run it, and see it fail.
3. **Change the code.**  New environment state goes into the world
   layout comment at the top of `src/cmc.c`, into the encoding, and into
   `cmc_describe_action`.  A new action kind gets a fair-schedule
   decision in `fair_kind()` (is it something the environment owes the
   cores?) and a replay round-trip test.
4. **Green, then the gates.**  Run `make check` with gcc and with clang.
   If you touched search, encoding, fingerprints or `calib/hc/`, also run
   `make check-calibration` (about 9 min) and `make check-hc-bugs`.  If you
   touched memory handling, run `make check-msan`.  Done = every gate green,
   and the calibration table says "every row matches TLC".
5. **Record.**  An API change goes in `CHANGELOG.md` and
   `man/man3/libcmc.3`; a new option in `man/man1/cmc.1`
   (`make check-docs` fails until it is there).  A design change goes in
   `docs/DESIGN.md`, and in `man/man7/cmc.7` if it changes the model.  A new measurement goes in
   `docs/measurements/YYYY-MM-DD-<topic>.md`, with its raw output beside
   it.

## Calibration rows

`calib/hc/calibrate.sh` holds the expected TLC values.  They come from
TLC with `-workers 1`: multi-worker TLC does not report shortest
counterexamples.  A row that moves is a bug in cmc or in the HC port
until shown otherwise.  Never edit an expected value to match cmc.
Re-run TLC (`calib/hc/tlc.sh HummingConsensus.tla cfg/<row>.cfg
-workers 1`) and explain the difference in a measurement file.
