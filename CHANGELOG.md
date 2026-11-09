# Changelog

libcmc follows semantic versioning.  Before 1.0.0 a minor release may
change the API in `include/cmc.h`; this file says when it does.

## Unreleased

- The invariant language gained the expressiveness to state a protocol's
  real safety property instead of a proxy.  `all i in nodes : EXPR` and
  `some i in nodes : EXPR` quantify over node indices, so agreement over
  all pairs reads like its TLA+ mirror rather than a hand-expanded list.
  An optional `[fields]` section names byte offsets, so an invariant
  says `node[i].epoch`, not `node[i][7]`, and does not depend on the
  hook's layout.  An invariant prefixed `eventually` is a bounded
  reachability check: some reached state must satisfy it, reported at
  the end of an exhaustive search (not fairness-based liveness, which
  the safety engine does not do).
- Two more worked examples, checked as they ship.  `hc/`: Humming
  Consensus / chain replication (a replicated log down a chain of three
  nodes) checked for no-lost-ack across a crash and a reconfiguration;
  an early-ack bug loses an acknowledged write to a crash and is caught.
  `zab/`: ZooKeeper Atomic Broadcast (a leader and two followers)
  checked for no-lost-commit; an early-ack bug loses a committed
  transaction to a leader crash.  `raft/raftlog.c`: Raft log
  replication, checked for State Machine Safety (a committed entry is
  never lost); the leader that commits before a quorum has the entry
  loses it on a crash -- the failure Marc Brooker (AWS) animates.  Both are the acknowledged-write-lost
  failure Marc Brooker (AWS) animates in "Animations for System
  Builders".
- Each example cites its canonical formal reference -- Paxos (Lamport's
  `Paxos.tla`), Raft (Ongaro's `raft.tla`), chain replication (van
  Renesse & Schneider, OSDI 2004), ZAB (Apache ZooKeeper's `Zab.tla`) --
  and the safety property the libcmc invariant corresponds to, plus
  links to animated/interactive visualizations.

## 0.2.0 (2026-11-09)

Checking the program you ship.  `cmc --model-file` runs an unmodified
binary as processes under interception and checks it; the in-process
`CmcCore` interface becomes the engine's calibration path.  Two real
consensus protocols ship as worked examples.

- `cmc --model-file FILE` checks a program as it ships: each node is the
  shipped binary, run unmodified as a process under seccomp user
  notification (Linux x86-64), and the checker is the network, the
  clock and the crash between them.  A model file names the program,
  the environment (a client write, timeouts, crashes, drops, a depth
  bound) and the invariants, in a small expression language over each
  node's reported state and the replies clients received.  Breadth-first
  search gives the shortest counterexample, named action by action;
  `--simulate` takes seeded random walks.  `--binary` checks another
  build against the same file.  This is now the way to use libcmc; the
  in-process `CmcCore` interface is the engine's calibration path.
- The loader refuses every model-file section, key or value it does not
  implement, and every invariant it cannot parse whole, instead of
  ignoring it.
- Worked examples under `examples/`, checked as they ship.  `paxos/`:
  single-decree Paxos (two proposers, three acceptors, five processes)
  checked for agreement, with a planted promised-values bug caught by
  search and by simulation.  `raft/`: Raft leader election (three nodes)
  checked for Election Safety with and without a crash, with a
  double-vote bug that elects two leaders in a term.  `make check`
  builds and checks both.
- A timeout can now fire at a node idle in `accept()`, not only one
  blocked in `recv`: the node accepts a synthetic timer connection and
  reads `T` from it.  A server that waits for a peer to connect can thus
  detect an election timeout, which real leader election needs.  Pinned
  by `test/proc/accept_timeout.c`.
- Every intercepted syscall is pinned by a fixture with teeth
  (`test/proc/clocks.c`).  This caught a bug: `gettimeofday` wrote the
  logical clock to the wrong argument (the `tz` pointer, not the `tv`
  buffer), so a target reading wall time through it got zero instead of
  the checker's clock; fixed.
- `make install` installs the `cmc` binary and `lib/libcmc/cmc-preload.so`,
  the deterministic heap and logical clock a checked program runs under;
  an installed `cmc` finds it from its own location, or from
  `CMC_PRELOAD`.
- No change to `include/cmc.h`.  `cmc_main()` accepts `--model-file`.

## 0.1.0 (2026-11-03)

The first release: an explicit-state model checker for protocol code
written as pure C step functions, linked from the same object files the
product ships.

- Search: breadth-first with TLC semantics (shortest counterexamples,
  exact distinct and generated counts, depth counting the initial state
  as 1), depth-first to a bound, seeded random walks, and replay of a
  saved counterexample through the real cores.
- Environment, enabled only from what the cores emit and from budgets:
  FIFO connections that reset on a drop or crash, or an unordered bag
  with duplication; timers; persists that complete in order; crashes
  that keep any prefix of the pending persists; wakes; client requests.
- Properties: invariants, action properties, witnesses, quiescence
  properties, and bounded liveness under a fair schedule.
- Gates: a determinism gate (`--det`), a purity gate over core objects
  and sources and the probes that prove it rejects impurity, a
  MemorySanitizer gate, planted-bug gates for the toy and calibration
  cores, and a documentation gate.
- Calibration: on 22 configurations of HummingConsensus, the same
  verdicts, counterexample depths and distinct-state counts as
  single-worker TLC.
- `libcmc.a`, `cmc.h` (`CMC_VERSION` is "0.1.0"), and the command line
  as the library function `cmc_main()`, with `cmc --version`.
- Man pages `cmc(1)`, `libcmc(3)` and `cmc(7)`; `make install`; a Nix
  flake; documentation at <https://gregburd.codeberg.page/libcmc/>.
