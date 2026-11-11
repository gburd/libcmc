# Changelog

libcmc follows semantic versioning.  Before 1.0.0 a minor release may
change the API in `include/cmc.h`; this file says when it does.

## Unreleased

- Certified abstract models for Raft leader election, Humming Consensus
  and ZAB (`examples/spec/raft.cml`, `hc.cml`, `zab.cml`), joining the
  existing Paxos model, so the whole consensus family now has one.  Each
  is checked three ways that agree: `cmc --spec`, TLC
  (`make check-emit-tla`), and Apalache (`make check-emit-apalache`).
  Raft's model is the vote-and-win core (each server grants one vote per
  term, a candidate wins on a quorum) and its property is Election
  Safety (at most one leader per term, following Ongaro's raft.tla); the
  `--define BUG` revote lets two candidates win.  HC's model is
  agreement over write-once epoch register stores with a quorum
  (following Machi's Humming Consensus) and inherits the shared
  `consensus` Agreement; the `--define BUG` overwrite breaks write-once
  so two values are chosen.  ZAB's model is commit-durability (following
  Apache ZooKeeper's Zab.tla): a committed transaction is held by a
  quorum, so it survives a leader change whose recovery quorum must
  intersect; the `--define BUG` early ack commits before a quorum
  stored, so a crash loses the committed transaction.  The emit gates
  now set `CHECK_DEADLOCK FALSE` (TLC) and `--no-deadlock` (Apalache),
  since a terminal state is not a safety fault.

- The TLA+ bridge reaches Apalache (Milestone D): `cmc --emit-tla` now
  writes Apalache `@type` annotations (every constant and scalar
  variable is `Int`, a function variable is `Int -> Int`, the `vars`
  tuple is annotated so Apalache reads it as a tuple, not a sequence)
  and a `CInit` operator binding the constants to their `.cml` values.
  Apalache's symbolic, Z3-backed checker then accepts the generated
  module directly: `apalache-mc check --cinit=CInit --inv=INV
  MODULE.tla`.  The annotations are `\*` comments, so TLC still accepts
  the same module unchanged.  `make check-emit-apalache` verifies that
  the generated TLA+ type-checks in Apalache and agrees with `cmc
  --spec` (counter and Paxos hold, the planted Paxos bug violates).
  Apalache is an optional external tool, like TLC; the gate fetches it
  once to `$CMC_SCRATCH` or uses `$APALACHE_MC`, and needs Java 21+.

- Action-level refinement with stuttering (Milestone C2): `cmc --refine
  SPEC --bind B --refine-steps` checks that each implementation *step*
  maps, under the binding, to either an abstract action step or a
  stutter (the abstraction unchanged), not only that each reached state
  satisfies the invariant (C1).  This catches an implementation that
  reaches only in-range states but gets there by a transition the spec
  forbids, which the state-level check cannot see.  It reuses the same
  successor enumeration and canonical fingerprint the `--spec` search
  uses and TLC calibrates, so a matched step is an exact abstract
  successor.  It requires every spec variable to have a binding and
  refuses otherwise, since matching an action step needs the whole
  abstract state.  `test/proc/mono.c` (a counter the spec allows to step
  up only by one) is the gate: C1 passes a by-two build, C2 catches it.

- One model, many implementations (Milestone C gate): a second,
  structurally different single-decree Paxos (`examples/paxos/paxos2.c`
  -- three symmetric nodes, each both proposer and acceptor, a different
  wire protocol and a different state-hook layout) is checked to refine
  the *same* abstract model `examples/spec/paxos.cml`, through its own
  binding `examples/spec/paxos2.bind` (the spec unchanged).  Both Paxos
  binaries now refine one specification, making "one spec, many
  implementations, all correct" a demonstrated claim.  Gated in
  `test/proc_gate.sh`.

## 0.4.0 (2026-11-10)

- Liveness with weak fairness for the abstract checker (Milestone F): a
  `.cml` can declare `fairness weak` and temporal properties
  `property P: eventually E` (`<>E`) and `property P: A ~> B`
  (leads-to).  After the safety search, the checker builds the reachable
  state graph and finds a fair violating lasso -- a cycle, all of whose
  states miss the goal, that weak fairness does not forbid (no action
  enabled throughout the cycle yet never taken) -- via SCC analysis on
  the sub-graph restricted to goal-free states.  `examples/spec/ticker.cml`
  (a counter that must reach its max) holds under fairness; built with a
  `reset` action (`--define RESET`) a fair cycle avoids the max and the
  property fails.  Cross-checked against TLC: `cmc --emit-tla` now emits
  `WF_vars(...)` fairness and the temporal property, and TLC agrees on
  both verdicts.  This is the in-process-engine liveness the plan scoped
  for the abstract side; process-side liveness still waits on
  fork-from-snapshot.
- Symmetry reduction for the abstract checker (Milestone E): a
  `symmetry SET` declaration in a `.cml` names interchangeable values
  (node ids, say); the checker canonicalises each state to the
  least image under permutations of SET before deduping, collapsing
  states that differ only by a permutation.  `examples/spec/token.cml`
  (a token among N interchangeable nodes) shrinks from 10 distinct
  states to 3, with the same verdict -- the soundness check for the
  declaration.  Obligation (as with TLA+ model values): the symmetric
  values must be a dedicated range, interchangeable everywhere they
  appear and disjoint from other integer uses, or the reduction is
  unsound; the gate checks the verdict is identical with symmetry on and
  off.  Invariants are evaluated on the real reached state, not the
  canonical representative, so a specific-id property is still checked
  correctly.
- The TLA+ bridge, `cmc --emit-tla FILE.cml` (Milestone D): translate an
  abstract `.cml` module to a TLA+ module on stdout, so the same model
  reaches TLC (explicit), Apalache (symbolic), and TLAPS (proof) -- the
  proof and large-domain engines the plan delegates to.  Verified
  against live TLC (`make check-emit-tla`): the generated module parses
  and gives the same verdict as `cmc --spec` -- counter and Paxos hold,
  Paxos with the planted bug violates, at the same depth.  `[TAG]`-gated
  actions are emitted only when `--define`d, and operators are rewritten
  faithfully (`and`->`/\`, `count`->`Cardinality({...})`,
  `[f except ...]`->`[f EXCEPT ![...] ...]`, `/`->`\div`).  This is how
  the hand-kept `calib/hc` TLA+ is, in time, superseded by a mechanical
  bridge.
- Refinement checking, `cmc --refine SPEC.cml --bind FILE.bind --binary
  PROG` (Milestone C, the heart of the plan): the shipping binary is
  checked against the *reusable abstract model* -- the same `.cml` that
  `cmc --spec` checks on its own and TLC calibrates.  A `.bind` file maps
  the implementation's observables (state-hook fields, the reply log) to
  the abstract variables; at each reached implementation state the
  abstract state is built from the binding and the abstract invariants
  are checked.  Demonstrated on Paxos: the shipping
  `examples/paxos/paxos.c` refines `examples/spec/paxos.cml` (561 states,
  no error), and the planted-bug build does not -- the abstract
  Agreement is violated, sourced from the reusable model, not a
  hand-written implementation-side invariant.  This is "one model, many
  implementations": point the same spec and binding at any Paxos binary.
  The binding may reference only observables, never internals (the
  no-omniscient-oracle rule), so the refinement is as honest as the
  state hook is complete.
- Abstract-model modules: a `.cml` can `extends` (or `import`) another,
  merging its constants, variables, actions and invariants, so a shared
  base is written once.  `examples/spec/consensus.cml` is that base for
  the consensus family -- it declares the agreed-value record and the
  Agreement property (at most one value chosen) -- and
  `examples/spec/paxos.cml` extends it: single-decree Paxos in the
  Voting style (acceptors vote per ballot; a value is chosen on a
  quorum) inherits Agreement unchanged.  Calibrated against single-worker
  TLC (examples/spec/paxos.tla): 117 distinct states, depth 5, identical
  verdict; the planted double-vote bug breaks Agreement at the same
  6-action depth as TLC.  This is the shared-base half of "a category of
  protocols models its common requirements the same way".
- The abstract-model front end, `cmc --spec FILE.cml` (Milestone B of the
  TLA+-scope plan): a small, TLC-like explicit-state checker for a
  declarative specification on its own, no implementation.  A `.cml`
  module has `const`, `var` (integer, set and function/record
  variables), `init`, named guarded `action`s (with finite parameters,
  so one action covers a family of transitions), and `invariant`s; the
  search is breadth-first, so a violation is the shortest action
  sequence.  Calibrated against single-worker TLC: on the counter and
  mutex fixtures `cmc --spec` gives the same verdict, distinct-state
  count and shortest-counterexample depth as TLC on the hand-written
  TLA+ twin (test/spec/*.tla; `make check-spec-calibration`).  This is
  the abstract half of "one model, many implementations" -- the model an
  implementation will be checked to refine (Milestone C).

- The invariant language gained integer arithmetic (`+`, `-`, `*`,
  `mod`) and `count V in SET : PRED` (cardinality), so a quorum property
  -- a committed value is held by a majority -- is now expressible; it
  was not before.  This is the first increment of the value model
  (Milestone A of the TLA+-scope plan), kept inside the SMT-encodable
  fragment.  A `quorum` fixture pins it: the count-based invariant holds
  when the coordinator commits only on a majority of stores, and catches
  a sub-quorum commit.

## 0.3.0 (2026-11-09)

- The Humming Consensus example (`hc/`) is now the GENERIC agreement
  protocol it is -- like Paxos, Raft and ZAB -- not chain replication.
  Three participants agree on an opaque configuration per epoch over
  write-once register stores, communicating only indirectly through
  reads and writes of those stores (Fritchie, PaPoC 2016, Figure 2).
  The checked property is HC's safety property, agreement: at most one
  configuration is adopted per epoch.  The correct (CP-mode) build holds
  agreement EVEN UNDER A NETWORK PARTITION -- a quorum majority is
  required to adopt, and quorums intersect -- proven exhaustively; the
  `-DBUG` build runs in eventual-consistency mode (adopt without a
  quorum) and loses agreement under a partition.  The README cites the
  paper, Machi's reference implementation, and the invariant's mapping
  to the stated safety property.  (Supersedes the earlier chain-
  replication framing, which was one use of HC, not HC itself.)
- The Paxos, Raft and ZAB invariants are restated with the new
  node-quantifiers and named fields, so each reads as the safety
  property of its canonical specification -- Paxos agreement over all
  proposer pairs, Raft Election Safety over all node pairs -- rather
  than a hand-expanded list of pairs.
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
- Two more worked examples, checked as they ship.  `zab/`: ZooKeeper
  Atomic Broadcast (a leader and two followers) checked for
  no-lost-commit; an early-ack bug loses a committed transaction to a
  leader crash.  `raft/raftlog.c`: Raft log replication, checked for
  State Machine Safety (a committed entry is never lost); the leader
  that commits before a quorum has the entry loses it on a crash -- the
  acknowledged-write-lost failure Marc Brooker (AWS) animates in
  "Animations for System Builders".  (The `hc/` example is rewritten as
  generic Humming Consensus, above.)
- Each example cites its canonical formal reference -- Paxos (Lamport's
  `Paxos.tla`), Raft (Ongaro's `raft.tla`), Humming Consensus (Fritchie,
  PaPoC 2016), ZAB (Apache ZooKeeper's `Zab.tla`) -- and the safety
  property the libcmc invariant corresponds to, plus links to
  animated/interactive visualizations.

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
