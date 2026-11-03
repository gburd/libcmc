---
title: Using libcmc in your project
parent: Guide
nav_order: 3
permalink: /guide/03-in-your-project/
lede: >-
  Split the core from the product, link both against the same object, and gate it.
---

1. TOC
{:toc}

---

## Get libcmc

Any of these works; pick one and pin a commit.

- A git submodule, built by your build:
  ```sh
  git submodule add https://codeberg.org/gregburd/libcmc.git third_party/libcmc
  make -C third_party/libcmc build/libcmc.a
  ```
- An installed copy: `make install PREFIX=...`, then `-lcmc`.
- Nix: the flake's default package has `lib/libcmc.a`, `include/cmc.h`
  and the man pages.

## Split the core out

The core is the protocol's decision logic: given this state and this
event, what changes and what gets sent.  It lives in its own source
files, which both the product and the checker compile, and it includes
`cmc.h` for the event and output types only.

```
src/core/raft_core.c     pure step code: the purity gate checks this
src/core/raft_core.h     the state struct, shared by product and model
src/raft_glue.c          the product: sockets, fsync, timers -> step()
test/mc/raft_model.c     CmcCore, properties, describe hooks
test/mc/main.c           cmc_main() with your model table
```

The product's glue turns real events into `CmcEvent`s and carries out
the `CmcOutput`s: it sends the messages, arms real timers, writes and
fsyncs the persist records, then delivers `CMC_EV_FLUSHED`.  The checker
links the same `raft_core.o` against libcmc instead of the glue.

## Gate the core

Three gates keep the core honest.  Run them in CI on every change to
the core.

**Purity.**  `scripts/purity_gate.sh "OBJECTS" "SOURCES"` inspects the
core's object files and sources.  It fails on a call to anything but
`mem*`, writable or thread-local data, an instruction that reads a
clock, randomness or the kernel, `asm`, or a direct write to `out->o`.

```make
check-purity: build/raft_core.o
	third_party/libcmc/scripts/purity_gate.sh "build/raft_core.o" "src/core/raft_core.c"
```

**Determinism.**  Run every check with `--det`.  Each step runs twice
from copies of the same state, and any difference fails the run.

**Planted bugs.**  Put each bug you want the checker to catch behind an
`#if` in the shipped core source:

```c
#if RAFT_BUG == 1
	/* vote for anyone, ignoring the log check */
#else
	if (!log_up_to_date(s, ev))
		return;
#endif
```

Build one object per variant, and require each to fail with the
property you expect, within a state budget, by search and by
simulation.  `test/toy_gate.sh` in this repository is the pattern.  A
bug the checker misses is a hole: in the budgets, the properties or
the engine.  Treat it as a failure.

## Choose budgets

A pass covers only the budgets you gave.  Start small and raise them
until every planted bug is caught:

| option | what it bounds |
|---|---|
| `--clients N` | client requests per node |
| `--drops N` | messages lost (FIFO: connection resets) |
| `--crashes N` | crashes, each keeping any prefix of pending writes |
| `--net bag --dups N` | duplication, for transports without order |
| `--max-states N`, `--time S` | stop early: the result is "no violation found in N states", never a pass |

When the full search is too big, run `--simulate` with many walks
first, then exhaustive search with smaller budgets.

## In CI

```yaml
- run: make -C third_party/libcmc build/libcmc.a
- run: make check-purity check-mc
```

where `check-mc` runs your checker on each model with `--det`, and your
bug gate.  The exit status is 0 for a pass, 1 for a violation, 2 for an
incomplete search and 3 for a usage error, so a CI step fails on
anything but a full pass.

## When a check fails

1. Save the counterexample with `--trace-out bug.replay`.
2. Replay it with `--replay bug.replay`: the same events, through the
   real step code, with every state printed.  Commit the replay file
   next to the fix as a regression test.
3. If the violation is `cmc:engine`, a core broke an engine rule: it was
   not deterministic, changed durable bytes outside `apply_persist`, or
   went past a cap.  The message says which.
