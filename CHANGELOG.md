# Changelog

libcmc follows semantic versioning.  Before 1.0.0 a minor release may
change the API in `include/cmc.h`; this file says when it does.

## Unreleased

- The in-process engine's BFS (`cmc --model`, `cmc_check()`) keeps a
  32-byte record per state (fingerprint, parent record number, action)
  in 256 shards whose index grows without a second copy, instead of a
  48-byte table entry that doubled; the queue holds record numbers, not
  parent fingerprints.  pg_chain's 161,613,053-state Paxos row peaks at
  17.5 GB instead of 28.8 GB, with the same counts
  (docs/measurements/2026-11-24-library-engine-memory.md).
- `--workers N` (`CmcOptions.workers`): BFS expands each batch of queued
  states in N threads (C11 `<threads.h>`).  Counts, depth, witness
  depths, coverage and the counterexample are those of one worker.
  Other searches, and `--spec`, refuse it.  Cores and properties must be
  reentrant.
- `--queue-mem MB` (`CmcOptions.queue_mem`): BFS queue chunks beyond the
  budget spill to files in `$CMC_SCRATCH` or `$TMPDIR`.
- `make check-pgchain` runs the fast rows of pg_chain's gate
  (test/pgchain) with 8 workers; `PGCHAIN_MC_EXTRA` adds options to the
  gate's runs.

## 2.3.1 (2026-11-24)

The release of 2.3.0 stopped before publishing: its tag's CI ran the
calibration job past its 60-minute limit, now that the job also ran the
TLA+ twins and reader against TLC.  That step moves to the bridge job
and both jobs get 90 minutes.  The code is 2.3.0's.

## 2.3.0 (2026-11-24, tagged, not published)

Gives `cmc --spec` what a TLA+ user brings to it
(docs/plans/2.3-tla-users.md): TLC's deadlock check, state and action
constraints, records, bounded sequences and `def` operators, `IF`,
per-action and strong fairness, `[][A]_vars` properties, action
coverage, lasso counterexamples, ITF and JSON trace files, a reader for
TLA+ modules and TLC configurations (`cmc --spec FILE.tla --config
FILE.cfg`), spec terms in refinement traces, and proof hooks
(`[inductive]` invariants, `--check-inductive`, an emitted TLAPS
outline).  Each is checked against single-worker TLC on the same model,
and every emitted module is parsed by SANY.  docs/guide/04-from-tla.md
takes Lamport's TCommit from TLA+ to a C implementation.  Three reviews
found loader, emitter and reader inputs read to a different model than
TLC checks (some also in 2.2.0: a second `when` replaced the first, a
domain containing `f[i]` read as an action tag, a duplicate `const`
ignored); each is fixed or refused with a test.  Results change for the
same input: the deadlock check is on by default, as in TLC, so a model
whose terminal states are intended needs `deadlock = off`.

Fixed: a state-level `--refine` violation found by breadth-first search
left the nodes of the reached world running after cmc exited.  That path
reported without killing them first, and the replay that prints the
trace launches a fresh set, so the first set was never reaped.  A node
that retries a failed `accept()` then spins forever as an orphan.  Gate:
proc_gate "C1: a refinement violation leaves no node running"
(test/proc/orphan.c).

`cmc --spec FILE.tla --config FILE.cfg` reads a TLA+ module and its TLC
configuration directly (docs/plans/2.3-tla-users.md, items 4 and 5).
The new src/cmc_tla.c translates the module to the `.cml` model it
means and the `.cml` checker checks that; `cmc --emit-cml` prints the
translation.  It reads the subset `.cml` expresses (CONSTANTS bound by
the .cfg, VARIABLES, non-recursive definitions, an Init of `x = e`, a
Next of actions under `\E`, `WF_vars`/`SF_vars` per action, a TypeOK
as the variables' types, THEOREMs skipped, records, sequences,
`EXCEPT` paths and `@`, `IF`, bullet lists by column) and the .cfg keys
CONSTANT(S), SPECIFICATION or INIT/NEXT, INVARIANT(S), PROPERTY,
CONSTRAINT, ACTION_CONSTRAINT, CHECK_DEADLOCK, SYMMETRY over model
values and a trivial VIEW; it refuses everything else with the file,
line and construct (exit 3).  Variable types are inferred from Init and
the assignments.  Gates: tla_gate reads back every module `--emit-tla`
writes (69 round trips over examples/spec, test/spec, test/spec/fair
and test/spec/ind) to the same
verdict, counts and trace; spec_calibrate reads the counter, mutex,
guard, deadlock, constrain and logrep twins directly and matches TLC;
spec_gate has a row for each refusal.  The paxos-family and zab twins
are refused (strings, `\X`).

An adversarial review of the reader against TLC found three inputs it
read to a different model than TLC checks, each now refused with a
spec_gate row: `a /\ b \/ c` without parentheses (SANY: a precedence
conflict), a definition parameter or bound name that names a variable,
constant, definition or enclosing bound name (SANY: multiply defined),
and arithmetic or order on a model value (TLC stops; the reader held
model values as integers from 1000000 and computed with them, a false
pass).  Model values now start at 2^40, past TLC's integers, so no
integer equals one, and a variable may hold an integer in one state and
a model value in another, as TLC allows.

A second review found two `x \in T` invariants on one variable read as
two types: the last replaced the first, and both were checked as TRUE,
a false pass where TLC reports the tighter one violated.  Now one
invariant gives the types, TypeOK if the .cfg lists it, else the first
of that form, and every other is checked as written.  Gate: spec_gate
and spec_calibrate typeinv rows (test/spec/typeinv.tla, TLC's verdict
and trace).

A variable's type that read a variable (`var x : y .. 3`, or a TypeOK
conjunct `x \in y..5`) evaluated it as 0, while the emitted TLA+ read
its value: a false pass, and `--check-inductive` reported inductive a
TypeOK that TLC and Apalache find a CTI for.  A `.cml` type or `const`
that reads a variable is now refused at load (exit 3), and the reader
checks such an `x \in T` as an ordinary invariant, as TLC does.  Gate:
spec_gate vardom rows (test/spec/sound/vardom.cml), spec_calibrate
vardom row (test/spec/vardom.tla, TLC's verdict and trace).

A computed integer of 2^40 + k equalled the k-th model value, so
`x # Big + 1` with `Big == 1024 * 1024 * 1024 * 1024` was violated in a
state where TLA+ says it holds (TLC stops: overflow).  An arithmetic
result of 2^40 or more in magnitude is now an overflow, exit 3.  Gate:
spec_gate "at 2^40" and "computed 2^40" rows.

`.cml` gains `if C then A else B`, TLA+'s `IF`: only the branch taken
is evaluated.  `cmc --emit-tla` writes it as `IF THEN ELSE`
(test/spec/ifexpr.cml agrees with TLC: 11 distinct states, depth 7).
An action's parameter set may now be a literal with commas, `p in {0,
1}`.

Fairness per action, strong fairness, action properties and action
coverage for `cmc --spec` (docs/plans/2.3-tla-users.md, items 6 and 7),
each checked against single-worker TLC 2.19 on the emitted module
(emit_gate rows on test/spec/fair/).

- `fair weak A` and `fair strong A` give one action WF_vars or SF_vars,
  and `fairness strong` gives every action SF_vars (`fairness weak` is
  unchanged).  The liveness check handles strong fairness as TLC does,
  splitting a component again after dropping the states that enable a
  strongly fair action it never takes.  `--emit-tla` writes the
  fairness into `Spec`.  A parameterized action is fair as its
  disjunction over the parameters.  An unknown action name in a fair
  line is an error.
- `property NAME: always [E]_vars`, TLA+'s `[][E]_vars` over `x` and
  `x'`, is checked on every generated step but a stutter, with the
  shortest trace ("Error: Action property NAME is violated.").  It is
  allowed under a constraint, since it needs no state graph.
- `cmc --spec --coverage` prints each action's distinct:generated
  counts, as TLC's `-coverage`, and how many states enable it, with a
  warning for an action no explored state enables.  TLC counts the
  successors of a guard with an existential once per witness, and of
  a guard with a disjunction once per true disjunct, so its generated
  count can be larger (Paxos phase2a, ZAB commit); the distinct counts
  match.
- A liveness violation prints its lasso as TLC does: the states from
  the initial one, then `Back to state N: <action>` or `State N:
  Stuttering`.  `--trace-out` writes it, in ITF with ADR-015's `loop`
  index (and `loopAction` in `#meta`), in JSON with the loop state
  repeated and a `back_to` index; before, no file was written.  The
  lasso is a valid fair counterexample but in general not TLC's (its
  prefix or cycle can differ in length or loop state); on the emit
  gate's TEMPORAL rows it is TLC's, and the gate compares the two.

`cmc --spec` has TLC's checking options (docs/plans/2.3-tla-users.md,
item 1), each calibrated against single-worker TLC 2.19 on the same
model for verdict, distinct states and depth (spec_gate,
spec_calibrate and emit_gate rows on test/spec/deadlock*.cml and
constrain*.cml).

- A reachable state with no enabled action is a deadlock, reported as
  TLC reports it ("Error: Deadlock reached." and the shortest trace,
  exit 1).  It is on by default, as in TLC; `deadlock = off` in the
  model or `--no-deadlock` turns it off.  This changes results: a model
  whose terminal states are intended now fails until it says
  `deadlock = off`, as raft, ticker, twophase, zab and several test
  models now do.
- `constraint NAME: EXPR` and `action_constraint NAME: EXPR`, TLC's
  `CONSTRAINT` and `ACTION_CONSTRAINT`: a state outside the model is
  not counted or expanded, but its invariants are checked.  A liveness
  property under a constraint, and `--refine` against a spec with one,
  are refused.
- Invariants are now checked as each state is generated, not as it is
  expanded, as TLC does, so where an invariant and a deadlock are both
  reachable the error reported is TLC's.  Counts and traces of the
  existing models are unchanged.
- `cmc --emit-cfg FILE.cml` writes the TLC configuration for the module
  `--emit-tla` writes: constants, invariants, properties, constraints
  and `CHECK_DEADLOCK FALSE`.  emit_gate now uses it and compares
  distinct states and depth with TLC, not only the verdict.  A module
  with no action now emits `Next == FALSE`, so TLC finds the deadlock
  `cmc --spec` finds (it emitted `UNCHANGED vars`, a stutter).

- Machine-readable counterexamples.  `cmc --spec` takes `--trace-out
  FILE` and `--trace-format itf|json`: ITF (the default) is laid out as
  Apalache writes a counterexample for the module `--emit-tla` makes,
  with the action per state in `#meta`; json is a plain array of states
  with the action taken.  `cmc --model-file` takes `--trace-out FILE`
  and writes json: each listed step and the world after it (node
  records, durable records, crashed flags, replies).  Gates: spec_gate
  and proc_gate trace rows (the file parses, has the shortest trace's
  length, and its steps and final state are the printed ones), and
  apalache_gate, which compares cmc's ITF file for each planted bug
  with the one Apalache writes.

Proof hooks (docs/plans/2.3-tla-users.md, item 10).  `invariant NAME
[inductive]: EXPR` claims an invariant inductive, and `cmc --spec FILE
--check-inductive` checks the claim: it enumerates every state of the
declared types, not only the reachable ones, and for each that
satisfies TypeOK and the marked invariants takes every step, reporting
a counterexample to induction (the state, and the step with its
parameters) or that the conjunction is inductive.  A TypeOK of more
than 1,000,000 states is refused (exit 3).  `cmc --emit-tla` now writes
`TypeOK` for every model, and `--emit-cfg` names it, so TLC checks the
declared types too; for marked invariants it also writes `IndInv`, the
constants as an `ASSUME`, and `THEOREM Spec => []IndInv` with a TLAPS
proof outline.  A model that names something TypeOK, IndInv, ConstOK,
PTL or Inductive is refused by `--emit-tla`.  Gates: spec_gate
proof-hook rows; emit_gate inductive rows, where TLC from `INIT IndInv`
gives the same verdict and as many states as cmc counts in IndInv;
apalache_gate inductive rows, where Apalache's inductive check gives
the same verdict, on test/spec/ind (a mutex invariant that is
inductive only with its strengthening, a queue of records, and a step
out of a type).  TLAPS 1.6.0-pre proves the outline for counter in
full and 12 of 14 obligations for the mutex; it is not part of a gate.

Counterexamples of `cmc --model-file ... --refine SPEC --bind FILE` read
in spec terms (docs/plans/2.3-tla-users.md, item 8).  Below each listed
step after which the refinement check judges the world, a `spec:` line
gives the abstract state the binding built (the bound variables), and
with `--refine-steps` the abstract action the step refined, with its
parameters, or `stutter`; `--trace-out` json carries the same in each
world's `spec` object.  A binding still builds only integers and
functions of integers, and refuses record and sequence variables.
Gate: proc_gate trace rows on mono-bug (C2) and paxos2-bug (C1), which
trace_check.py checks line by line against the file.

The abstract language (`.cml`) gains records, bounded sequences and
named operators, as a TLA+ user writes them.  A variable can be a record
`[a : T, b : T]`, a sequence `Seq(T, N)` of at most N elements, or a
function `[K -> T]` of any of these, nested; values are written `[a |->
E]`, `<<E, E>>` (a tuple is a sequence), with `r.a`, `s[i]`, `Len`,
`Head`, `Tail`, `Append`, `SubSeq` and `\o`.  An update takes TLA+'s
selector paths and `@`: `[f except ![i].d = Append(@, v)]`.  `def
NAME(p, ...) = EXPR` declares an operator, expanded where it is used; a
def may use only the defs before it, so recursion is refused at load
time.  A sequence longer than its bound, like any value outside its
variable's type, is a model error (exit 3), never cut short.  `cmc
--emit-tla` writes the TLA+ forms, extends `Sequences`, and annotates
records and sequences for Apalache.  Every value now has one canonical
encoding of words that the seen set, the liveness graph, C2 matching and
symmetry all use, so two states that differ in one field, element or
length stay distinct.  The new examples/spec/logrep.cml (log replication
over records and sequences) and its hand-written TLA+ twin agree with
TLC: 4439 distinct states at depth 16, and a 5-action counterexample for
its planted bug.  examples/spec/raft.cml now keeps each server as a
record, with the same 38 states.  The deadlock check, `constraint` and
`action_constraint` read records and sequences like any expression
(test/spec/constrain_rec*.cml agree with TLC), and `--trace-out` writes
them: in ITF a record is an object of its fields in name order and a
sequence an array, with `varTypes` such as `(Int -> Seq({ idx: Int, val:
Int }))`, as Apalache 0.63 writes them (apalache_gate tracerec and
logrep rows compare the files).  Gates: spec_gate records/sequences rows
and distinct, spec_calibrate logrep, emit_gate logrep, apalache_gate
logrep.

An update whose path is outside its value, `[f except [5] = 0]` on a
function of keys 0 .. 1, is now a model error.  cmc 2.2 added the key,
so `[f except [5] = 0] == f` was FALSE, where TLC leaves the function
unchanged and finds TRUE.  A def name, like a constant or variable, can
no longer be rebound by a quantifier, and up to 8 names can be bound at
once, not 4.

The process gate's rows that compare cmc with a native Linux run
(nbconnect, syscorner, tcpsem) no longer depend on timing.  Their
native runs used fixed loopback ports, so two checks on one host (gcc
and clang at once) met each other's sockets, and sleeps ordered the
native processes.  Node 0 now forks its peer itself, every native
listener is on a port the kernel picks, and pipes order the two
processes.  Measured on a 32-core host under load, with two copies
running at once: nbconnect early gave Linux's record in 72 of 100 runs
and now 200 of 200, tcpsem 96 of 100 and now 400 of 400, syscorner
selfdial 99 of 100 and now 200 of 200.  cmc's answers did not vary
(200 of 200 runs of the early model alike).  The records, and so what
the rows check, are unchanged.

Fixes to the `.cml` loader and `--emit-tla`, from an independent review
of `cmc --spec` against TLC (each a false pass or a broken TLC twin
before):

- Two `when` lines in one action are two conjuncts, as TLA+ `/\` lines,
  in the check and in the emitted module.  The second line used to
  replace the first.  A `when` line outside an action (after an
  `invariant`, say) is an error; it used to attach to the last action.
- An action's `[TAG]` is the bracket after its parameter list.  The
  loader took the last `[` in the header, so `action pick(i in 1 ..
  2, j in 0 .. f[i]):` read as an action tagged `i`, inactive without
  `--define i`, and the model passed with the action left out (of the
  emitted module too).  A header with text it cannot place, or a tag
  that is not a name, is an error.
- A `const` declared twice, in one module or over an `extends`, is an
  error.  The loader kept the first value while `--emit-cfg` wrote both
  and TLC took the last, so a model "overriding" a base module's
  constant checked the base's value and passed where its TLC twin
  failed (also in 2.2).  The override is refused rather than honoured
  because the base already evaluated with the first value (a const
  defined from it, a Seq bound).
- Operator mixes TLA+ refuses or reads otherwise are refused: `/` or
  `mod` beside `*`, `/` or `mod`, `mod` beside `+` or `-`, and `and`
  beside `or` need parentheses.  cmc read `x mod 3 * 2` as `(x mod 3) *
  2` and emitted `x % 3 * 2`, which TLC reads as `x % 6`, so the model
  and its TLC twin checked different formulas; the other mixes emitted
  modules SANY rejects.  test/spec/prec.cml holds the parenthesised
  forms, gated against TLC.
- `--emit-tla` rewrites a keyword only when it is a whole word.  It
  checked only the character after one, so `votedFor` came out as
  `votedF\/`, `small` as `sm\A` and `min` as `m\in`, which SANY
  rejects.  A `count` inside brackets (`g[count i in S : E]`) ended at
  the bracket that encloses it; its body used to run past the `]`.
  test/spec/names.cml holds both, gated against TLC.
- Under `symmetry`, every reached state is checked against its images
  under the transpositions of the set: each must lie in the domains and
  give every invariant and constraint the same value, or the run is exit
  3.  test/spec/symrec.cml broke the obligation itself (its request
  numbers 0 .. 1 overlapped the client ids 1 .. 2, so its 278 states
  were no orbit count); its numbers are now 5 .. 6, and the gate pins
  179, the orbit count of TLC's 357-state dump under swapping clients.
- The emit gate runs SANY on the module emitted from every `.cml` in
  examples/spec and test/spec that `cmc --spec` accepts, with each
  `[TAG]` defined in turn (77 modules), so an emitter syntax bug shows
  on any model, not only on a row TLC runs.

## 2.2.0 (2026-11-22)

Takes the process engine toward checking a program in place, as Greg
Burd's notes on 2.1.0 asked (docs/plans/2.2-in-place.md): a crash can
fall between any two calls that reach outside the process, nodes have
their own clocks with finite timeouts and drift, a node can have a
modelled file system, and a client can connect the way libpq does.
Three rounds of independent review of this work found false passes,
most of them also in 2.1.0; each is fixed with a test that fails
before the fix.  Results change for the same input: several programs
that passed now fail, as they do on Linux, and state counts grow,
often by several times, where crashes or mid-step connects are in play
(paxos 561 to 7711 distinct states; the full hc example is no longer
in any gate, which runs it to depth 15).

While a node is stopped inside a step the clocks are frozen, but a
deadline already due there still fires: two nodes whose timers fall due
at one instant could not interleave before, since the first to wake ran
its whole step first.  A poll that asks for no events, waiting only for
a hangup, now wakes with `POLLHUP` when the peer's FIN arrives after
this end's own `SHUT_WR`; it never woke.  Gates: proc_gate eqdeadline
and hupwait.

A zero-timeout `poll` or `epoll_wait` of the listener mid-step stops,
as a non-blocking `accept` does, while any other node can still act:
a dial may come first.  It answered at once, so a loop that polls its
listener right after dialling a peer never saw the peer's dial back.
raft goes from 10309 to 10320 distinct states and hc at depth 12 from
12570 to 12715, at depth 15 from 46184 to 49539.  The planted hc
bug's seed 1 now finds it at walk 583.  Gate: proc_gate baredial
pollbk.

A blocking `connect` from a node with more than one thread to a
crashed host, or across a cut link, waits and times out as from a
single-threaded node.  95459af had extended a refusal (exit 3) from
cut links to crashed hosts, so with `crashes = 1` any threaded client
that might dial a crashed server could not be checked at all; it had
run (to INCOMPLETE, without the race runtime) before.  The wait uses
the same timed park as a futex wait, so the node's other threads run
meanwhile.  No example's state count changes.  Gate: proc_gate mtdial
timeout and norefuse.

A DROP, and a peer's restart, no longer discard bytes the reader has
started to read or that a `poll` or `epoll_wait` has reported to it.
Those bytes are in the reader's kernel, and Linux reads them before
it reports the reset; cmc dropped them, so a server that got
`EPOLLIN` and then a reset reported a lost request Linux never
shows.  No example's state count changes.  Gate: proc_gate inkernel
wakerst, with a drop and with a crash and restart.

`poll` reports `POLLHUP` once a connection's own `SHUT_WR` is done
and the peer's FIN has arrived, before the FIN is read, as Linux does
(`tcp_poll()` sets receive shutdown when the FIN arrives).  cmc
reported it only after the read, so a client that half-closes and
waits for `POLLHUP` was checked on a path Linux skips (measured: 17,
then `POLLIN|POLLOUT|POLLHUP`, then read 0, where cmc gave 1 and 5).
No example's state count changes.  Gate: proc_gate sockapi revents
(a native record, now with this case).

An RST sent by a `close` behind bytes still on their way to the peer
arrives after them, as a search action (`rst n1->n0`), not at once.
Linux delivers the bytes first, and a reader can see `POLLIN` alone
before the reset comes in (measured: with a pause before the close,
`epoll_wait` gives `EPOLLIN`, then the read 1 byte, then
`ECONNRESET`).  Since a59d6d9 reported the reset's `POLLERR` and
`POLLHUP` with the first `POLLIN`, that state was unreachable, so a
loop that handles `POLLIN` and only later meets the error was not
checked on that path.  No example's state count changes.  Gate:
proc_gate midstep rstdata.

A blocking `connect` after a call with an effect earlier in the step
stops while the dialled node is running or its host may restart, and
a non-blocking `accept` with nothing waiting stops while any other
node can still act.  Both stopped only when the caller had bytes
unread at a node, and only that node's reaction counted, so three
Linux outcomes were never reached: the dialled node accepting a bare
dial (no bytes) and exiting or dialling back, a message relayed
through a third node that makes the dialled node exit, and a crash or
cut of the dialled host between two dials (which needs no reaction at
all).  State counts grow: paxos from 561 to 7711 distinct states
(about two minutes), raft from 5182 to 10309, raftlog and zab from 107
to 115, hc at depth 12 from 14534 to 12570 and at depth 15 from 36625
to 46184; repl and the guide's kvserver do not change.  A launch now
also clears the node's record of a call with an effect in the current
step, which a replay could inherit from the run before and diverge.
Gate: proc_gate baredial redial, dialbk and relay.

No deadline fires while a node is stopped inside a step.  A
single-threaded node held between two calls of one step (at a crash
point, or at a call another node may answer first) had its own dial's
SYN deadline and every other node's deadlines still able to fire, so
127 s could pass between a non-blocking `connect` and the
`getsockopt` two writes later, and a client was checked on an
`ETIMEDOUT` that Linux never gives there.  Only a call parked on the
handshake itself was exempt.  Now every clock is frozen at such a
world: no EXPIRE and no TIMEOUT is offered until the node reaches a
call that waits (cmc(7), Clocks and deadlines).  The hc example at
depth 12 goes from 23787 to 14534 distinct states and at depth 15
from 78155 to 36625 (about 7 minutes, the check-examples-slow row);
no other example's count changes.  The planted hc bug's simulation
now uses seed 1 (walk 1457, about 12 s), as seed 3 finds it only at
walk 29761.  Gate: proc_gate synmid own and other.

The full search of the hc example no longer finishes in a gate's time.
Since the mid-step dial became a point where the peer may act first,
hc's search space grew: at depth 12 from 8095 distinct states (125 s)
to 23787 (276 s), at depth 15 to 78155 (877 s); the full search, which
took 36331 states and about 13 minutes before, did not finish within
two hours.  `make check-examples-slow` now runs hc to depth 15
(INCOMPLETE, no violation) instead of to completion, and the planted
bug's simulation used seed 3 (walk 15905, about two minutes), as seed
4 no longer finds it in 30,000 walks.  The numbers are in
docs/measurements/2026-11-20-hc-midstep-dial.md; the later entries
above change them again (docs/measurements/2026-11-21-midstep-wide-park.md).

A dial to a node that crashed times out instead of being refused.  A
CRASH is a host losing power, and a host that is down sends no RST, so
Linux retries the SYN and fails with `ETIMEDOUT` after about 127 s
(or connects, if the host comes back first).  cmc answered
`ECONNREFUSED` at once, which needs a live kernel with no listener, so
a client whose timeout path was wrong passed, and one that fails over
fast on a refusal took a path a real outage does not give.  A dial to
a node whose process exited is still refused.  Now a blocking dial to
a crashed node waits (as across a cut link) until a RESTART lets it
through or its deadline fails it, and a non-blocking one fails at its
deadline.  The tcpsem redial row, whose node 1 crashes under cmc, now
expects `ETIMEDOUT` there where the native run (a stopped process)
gets `ECONNREFUSED`.  State counts grow where a node dials a crashed
peer: raftlog and zab from 105 to 107, the guide's kvserver from 30 to
32.  Gates: proc_gate syndial crashblock, crashrefblock, crashnb,
crashrefnb and exit, and tcpsem redial.

A non-blocking `connect` across a cut link times out, as a blocking
one already did: after 127 s on the dialler's clock (EXPIRE) the dial
fails with `ETIMEDOUT` for `SO_ERROR`, and `poll` reports `POLLOUT`,
`POLLERR` and `POLLHUP`, as measured on Linux.  The handshake simply
waited for the link to heal, so a program whose timeout path was wrong
(libpq's connect is non-blocking) passed.  The deadline is in the
world encoding.  No example's state count changes.  Gate: proc_gate
syndial nbcut and nbcutrev.

A crash no longer loses a message its peer has read part of, or that a
`poll` or `epoll_wait` has reported to the peer, nor any message
before it: those bytes are in the peer's kernel.  A crash after a
short read made the rest of the message vanish and the peer read EOF
mid-message, a spurious violation.  The rest of a message read in
part is also readable at once: a non-blocking read of it got EAGAIN
and a poll did not report it.  Whole messages the peer has not touched
may still be lost, which over-approximates Linux on loopback (cmc(7)).
No example's state count changes.  Gate: proc_gate inkernel partial,
pollwake and nbrest.

A blocking `connect` after a send earlier in the step, to a node that
has not read those bytes, stops so that node may react first; it may
exit, and Linux then refuses the dial.  A non-blocking `accept` with
nothing waiting stops the same way while any node holds unread bytes
from the caller, since that node may dial it in reaction.  Neither was
a point where the others could act, so a second dial never saw the
peer gone and a drain of the backlog never found a reply dial.  raft
goes from 4078 to 5182 distinct states and the hc example at depth 12
from 7518 to 23787: each dial that follows a reply in hc's loop is now
such a point.  Gate: proc_gate midstep reconn and nbaccept.

A `poll` or `epoll_wait` that returns at once and asks only for
`POLLOUT` (or `EPOLLOUT`) stops after an earlier send of the step, as
a write does, when the peer could reset the connection first.  Only a
poll for `POLLIN` stopped, so a zero-timeout writability check after a
send never saw the peer's RST.  No example's state count changes.
Gate: proc_gate midstep rstout and rsteout.

`poll` and `epoll_wait` report `POLLERR` and `POLLHUP` on a connection
its peer reset, and `POLLIN` once its receiving side is done, as Linux
does.  A reset connection reported only what was asked for (and a
blocked poll it woke, `POLLIN` alone), so an event loop that closes a
connection on `POLLERR|POLLHUP` without reading was checked on a path
Linux never takes (also on 2.1.0).  The revents for a reset, a FIN, a
shut side and a refused dial now follow a native measurement.  raft's
state count moves from 4072 to 4078 distinct states, and the hc
example at depth 12 from 8095 to 7518: a poll over a connection
already reset or past its FIN is ready at once, as on Linux, where it
waited for the search to wake it.  Gates:
proc_gate sockapi revents, midstep rstwake and rstewake.

A non-blocking read, and a `poll` or `epoll_wait` that would return at
once, can see bytes that have arrived.  They read EAGAIN, or reported
nothing ready, unless a poll had just woken that fd, and the node ran
on with no other node acting first, so a program that tries a read
before waiting never took its fast path (also on 2.1.0).  Now such a
call stops when bytes are queued, or a peer could send, close or reset
after the node's last call on a connection earlier in the step; the
search lets the others act and delivers (or wakes the poll), or runs
it as it stands.  Such a world is not judged by the invariants and
never merged.  No example's state count changes.  Gate: proc_gate
nbread read, poll and epoll.

A peer's RST can arrive between two writes of one step.  A write after
an earlier send in the step, to a peer that holds unread bytes from
the writer (so its close or exit sends an RST), or with a DROP or
RESTART in the budget that could reset the connection, stops so that
may happen first.  A write to a peer with nothing unread is not
stopped: nothing the peer does can make that write fail on Linux.
Stopping every write made paxos and raft time out; the narrow rule
changes no example's state count.  Gate: proc_gate rstmid.

A crash can fall between any two system calls of a single-threaded node
that reach outside the process, not only between its steps.  While
crash budget is left, a node stops after each write or send on a
checker-owned fd, each close of one, and each durable write, and the
search may crash it there.  Before, a node that acked a client and then
wrote its durable record, with no blocking call between, passed.  This
changes verdicts: a fixture whose backup acks from memory now fails
(guarded with `--crashes 1`), and state counts with crashes grow (the
guide's server: 15 to 26 distinct states).  No invariant is checked at
such a mid-step point, since a program reports its state after its
effects.  Gates: proc_gate rows durack-single and ackfirst, the guarded
row, and the guide's trace in docs_gate.

A crash may lose the bytes the node sent that its peer has not read.
A write returns once the bytes are in the sender's kernel, and a host
that loses power loses what it had not transmitted, so for each
connection to another node the crash keeps a chosen prefix of the
node's unread messages (its FIN included): one CRASH action per
combination, the trace naming any loss.  Before, every byte sent was
delivered later, so a primary that acked once REPL(v) was queued,
before the backup had it, passed under crashes.  State counts with
crashes grow.  Gate: proc_gate sendlost.

A connect() is a crash point too: the connection waits in the peer's
backlog once it returns, so a crash before the first write leaves the
peer an empty connection.  The guide's server grows from 26 to 30
distinct states, and its counterexample gains a `run`.  Gate: proc_gate
conncrash, and the guide's trace in docs_gate.

A writev() to the durable record (fd 193) or the state hook is served
by the checker as one record write, as write() is, and on fd 193 it is
a crash point.  It went to the kernel pipe, so a crash could not fall
between it and the next send.  A pwrite() there stays the kernel's
ESPIPE.  Gate: proc_gate "a crash between a durable writev and the
ack".

Hand-written TLA+ twins for `fpaxos.cml`, `caspaxos.cml`, `zab.cml` and
`fastpaxos.cml` (examples/spec/*.tla, in the style of `paxos.tla`), each
cited to its paper.  Single-worker TLC gives the same distinct-state
count and depth as `cmc --spec`, and the same shortest BUG trace:
fpaxos 36,756 states, depth 20, trace 12; caspaxos 3,280, 17, 12; zab
27,913, 26, 13; fastpaxos 1,976, 12, 11.  The Fast Paxos twin keeps the
sender of each promise, which the `.cml` only counts, so its count is
taken under a VIEW that forgets the sender (2,112 states without it).
Gate: `make check-spec-calibration`.

Apalache runs past `--length 10`, measured on one host
(docs/measurements/2026-11-20-apalache-length.md): no violation up to
14 steps for paxos, 13 for fpaxos and caspaxos, 16 for zab, and
fastpaxos complete at 11.  Each step costs 2 to 3 times the last, so
the full depths of paxos, fpaxos, caspaxos and zab are out of reach.
apalache_gate's fastpaxos pass row now runs at length 11, its depth
less one, so it covers every reachable state; the other rows stay at
10 to fit the CI job.

In `cmc --spec`, `and` and `or` short-circuit left to right as in TLC,
so a guard such as `x > 0 and y / x == 2` or `i == N or f[i] == 0`
protects its partial operation.  Before, both operands were evaluated
and such a spec was refused (exit 3).  The skipped operand is still
parsed.  Gates: spec_gate guard and short-circuit rows, and the guard
row of spec_calibrate against TLC.

A non-blocking `connect()` is modelled instead of refused, so a client
that connects the way libpq does (connect returning `EINPROGRESS`, a
poll for `POLLOUT`, then `getsockopt(SO_ERROR)`) runs under the checker.
The handshake completes when the search says, into a live peer's
backlog or refused, and a call on the socket before then may see it
either way, as on loopback.  `setsockopt` and `getsockopt` of
`SO_KEEPALIVE`, `SO_REUSEADDR`, `SO_REUSEPORT` and `TCP_NODELAY` are
recorded and read back and change nothing; `SO_RCVBUF` and `SO_SNDBUF`
may be set; `SO_ERROR` reads the pending error; other options, which
were accepted and ignored, are now refused.  `getsockname` and
`getpeername` answer on owned sockets instead of being refused.  A
socket redialled after a refusal keeps its `O_NONBLOCK` and
`FD_CLOEXEC` (they were lost).  Gate: proc_gate nbconnect ok, early and
refused, each against a native run.

`readv`, `writev`, `sendmsg`, `recvmsg`, `shutdown`, `dup`, `dup2` and
`dup3` on a checker-owned socket are modelled instead of refused.  The
vector calls act as one read or write of the iovec array's bytes, with
ancillary data and a destination address refused.  `shutdown` sends a
FIN or stops reads as Linux does, including the RST that bytes arriving
at a socket shut both ways draw.  A dup shares the connection and its
`O_NONBLOCK` until its last number closes.  A refused dial's pending
`ECONNREFUSED` was left out of the world encoding; it is in now.  Gate:
proc_gate sockapi iov, shut, dup and opts, each against a native run.

A full send buffer is a budgeted choice, `[environment] short_writes =
N`.  While it lasts, a send on a non-blocking connection with bytes
still in flight may get `EAGAIN` or send a prefix (1 byte, half, or all
but one), and the buffer then stays full, with no `POLLOUT`, until the
peer reads.  On 2.1.0 every send succeeded whole, so a writer that
ignored a short count passed.  Gate: proc_gate shortw.

Network partitions, `[environment] partitions = N`: the search may cut
a directed link between two nodes (asymmetric partitions included) and
heal it later.  A cut link holds back its bytes, FINs and handshakes,
in order, as TCP's retransmission does; nothing is lost.  An RST that
would cross a cut link is refused (exit 3).  A blocking connect across
a cut link waits until the link heals, or fails with ETIMEDOUT when a
127 s deadline on the node's clock (Linux's six SYN retries) fires
first; it used to succeed at once when the node dialled was up.  The
search already delays any delivery as long as it likes, so a cut alone
adds little; it matters with deadlines.  Gate: proc_gate partitions
rows, among them cutconn.

The race runtime provides the 16-byte atomics (`__tsan_atomic128_*`)
that gcc and clang emit for `__int128` atomics under `-mcx16`.  A
program that used them failed to link against `libcmc-race.a`.  Gate:
proc_gate rows atomic128 (a seq_cst handoff holds, a relaxed one is a
race, fetch_add and compare-exchange compute) and race_rt.

A thread that ends by `pthread_exit` or `thrd_exit` gives its join the
same happens-before edge as one that returns from its start function.
The race runtime took the joined thread's clock only on a return, so
the joiner's reads of what such a thread wrote were reported as data
races; and glibc's first `pthread_exit` loads libgcc_s, a read of a
kernel fd the checker refused in a multi-threaded node.  The runtime
now loads libgcc_s at `__tsan_init` and the join takes the thread's
final clock.  Gate: proc_gate rows threadexit (exit, thrd, nested, and
a read before the join that is still a race).

The race runtime sees `memcpy`, `memmove` and `memset` called from
instrumented code.  gcc leaves them as calls to the C library, and with
`_FORTIFY_SOURCE` (on by default in many distributions' compilers)
both gcc and clang call `__memcpy_chk` and its siblings; the runtime
saw only clang's unfortified `__tsan_memcpy`.  An unlocked `memcpy` into
a buffer another thread read passed with either compiler.  The runtime
now defines the plain and fortified forms in the executable, records
the bytes each reads and writes, and copies with the C library's
function.  Being in the executable, they also serve calls from
uninstrumented libraries, which now count as accesses.  Gate: proc_gate
rows memcopy and memcopy-chk (memcpy, memset and memmove race
unlocked, a locked memcpy holds) and race_rt.

The race runtime also sees the C library's string and format writers:
`strcpy`, `strncpy`, `strcat`, `stpcpy`, `sprintf`, `snprintf`,
`vsprintf` and `vsnprintf`, with their fortified `__*_chk` forms.  An
unlocked `strcpy` or `snprintf` into a buffer another thread read
passed.  gcc expands a `strcpy` or `memcpy` of a short constant into
plain stores it does not instrument (at any optimisation level, and
with `-fno-builtin`), so that race stays unseen with gcc; cmc(7) now
says so and recommends clang.  Gate: proc_gate memcopy rows scpy
through vsnpr, plain and fortified.

A review of the race runtime (docs/research/04-review-race-runtime.md)
found it passing programs that have a data race or a reachable failure.
It added happens-before edges POSIX and C11 do not promise: from one
reader's rwlock unlock to another's read lock, from one barrier round
to the next, from a condition signal to the woken wait, from a store
(which acquires nothing), and from a failed compare-exchange whose
failure order is relaxed.  Each is now as the standards say, so the
race each hid is reported.  And the reduction that makes only
acquisitions choice points does not cover a try or timed acquisition,
whose failure needs the other thread inside a section the search runs
without a switch, nor a semaphore that admits two holders: a pass with
either is now INCOMPLETE.  The clock forms of the locks, waits and
semaphore wait are wrapped, and an initialiser that calls `pthread_once`
keeps its own edge.  The bytes a `read`, `recv`, `readv`, `recvmsg`,
`pread`, `poll` or `epoll_wait` stores into the program's memory, and
those a write or send loads, now count as accesses (fortified
`__read_chk` and its siblings too): a buffer one thread read into while
another read it unlocked passed.  State counts of the
existing fixtures are unchanged.  Gate: proc_gate rows syncedge
(rwread, casfail, casok, bargen, sem2, trymutex, trysem, tryrw,
readbuf) and race_rt.

Finite timeouts and per-node clocks.  Each node has its own clock, and
a finite timeout on `poll`, `ppoll`, `epoll_wait`, `select` and
`pselect` with no descriptor, `nanosleep`, `clock_nanosleep` and a
futex wait (`pthread_cond_timedwait`, in a node with one thread or
several) is a deadline on it.  A new action, EXPIRE, fires a node's
earliest deadline, offered only when no other node would pass a
deadline of its own first, so deadlines fire in order and not one
state per tick.  The model file's `drift = R` lets one node's clock
run up to R per thousand slow or fast against the others between two
fired deadlines; only those extreme rates are explored (cmc(7), Clocks
and deadlines).  Before, these timeouts ended the check with exit 3.
`setitimer`, `alarm`, `timer_create` and `timerfd_create` used to reach
the real kernel unchecked, where a real-time signal or a ready timerfd
broke replay; they now end the check with exit 3.  A budgeted TIMEOUT
still moves every clock by one second.  Gates: proc_gate rows
leasedrift (a lease of L(1 - rho) holds under drift 50, L(1 + rho) is
caught only with the holder's clock slow, the correct margin fails at
drift 200), sleeps (one row per call, three select and clock refusals,
four timer refusals), timedwait (signal and timeout each win), and
unmodelled pollto (now fires).

A node can keep its durable state in files.  A model file's new
`[files]` section names a directory (`dir = /data`) that the checker
models for each node, in the same way it owns the node's sockets.  The
checker answers the open, read, write, seek, truncate, fsync, stat,
mkdir, unlink and rename calls there, and holds the contents in the
world.  A crash keeps each file's and each directory's synced contents
plus a prefix of its later changes, chosen by the search, so a create
or a rename survives a crash only once its directory is synced.  The
`eio` and `enospc` budgets make a write or an fsync fail, and a failed
fsync forgets the unsynced data.  A `bytes` budget bounds each node's
files.  Calls the model does not have on these files end the check
(exit 3), among them `O_DIRECT`, `mmap`, links and xattrs.  Paths
outside the directory keep the read-only behaviour.  Gates: proc_gate
rows fslog (a missing directory fsync and a missing fdatasync are each
caught) and fsfail (EIO, ENOSPC, the bytes budget, and the refusals).

Fixes found by `make coverage`: in the modelled file system a rename
to a path under a missing directory returned EBUSY, where Linux
returns ENOENT (gate: fsmodel); and the `cmc --spec` loader accepted
an action tag without its `]`, action parameters without their `)`,
and a function variable without its `]`, and read them as something
else (an untagged action, a guessed parameter list).  Each is now
refused with exit 3 (gate: spec_gate rows).

## 2.1.0 (2026-11-19)

Closes the four gaps 2.0.0 documented in the process engine (a
`sendmsg` on a kernel fd, the file system, memory interleavings between
threads, and the untested tree bound) and adds a coverage gate holding
function and branch coverage of every source file at 90% or more.
Writing the tests found more than twenty other places where cmc
checked less than it was asked or answered unlike Linux; each is fixed
with a test that fails on 2.0.0.  As in 2.0.0, results can change for
the same input: a multi-threaded node built without the race runtime,
which could pass, is now INCOMPLETE, and several constructs that ran
unseen are refused.

Process engine (`cmc --model-file`):

- Every lock acquisition is a choice point between threads, including
  one taken on its uncontended path with no system call: the preload
  wraps the pthread mutex, rwlock, spinlock, semaphore, barrier and once
  calls and C11 `mtx_lock`, `mtx_trylock`, `mtx_timedlock` and
  `call_once`.  On 2.0.0 a thread switch there was never explored.
  [proc_gate: syncpoint, pthread and C11]
- A race runtime, `libcmc-race.a`: a node compiled with
  `-fsanitize=thread` (gcc or clang) and linked against it in place of
  the sanitizer's runtime has every instrumented access and atomic
  checked against happens-before.  A data race is a violation, printed
  with both code addresses and the schedule.  Atomics are choice points.
  [proc_gate: memrace unlocked and relaxed, raceemit]
- A search in which a node ran more than one thread without the race
  runtime cannot pass: it ends INCOMPLETE, and so does an unmet
  `eventually`, since memory interleavings between choice points were
  not checked.  An atomic weaker than `seq_cst` does the same, since the
  search runs a sequentially consistent machine.  On 2.0.0 such a node
  could pass with a lost update reachable.  [proc_gate: memrace
  unlocked, relaxedadd; twothread]
- `sendmsg` on a kernel fd (below 500) is seen.  A second seccomp
  filter stops it for the checker, which sends it to the kernel in a
  node with one thread and refuses it (exit 3) in a node with more, like
  every other call on a kernel fd.  On 2.0.0 the launch child's own
  `sendmsg` of the listener kept the filter from matching it, so a
  worker could send on a socket another thread read, unseen.  The
  launch socketpair is now close-on-exec.  [proc_gate: sendmsg worker,
  sendmsg alone]
- Which node runs first is a search choice.  The engine runs one node
  at a time, until it parks, exits or wakes a parked node, and when two
  or more can run (at launch, after a restart, after such a wake) it
  offers `step nN` for each.  On 2.0.0 every running node was served at
  once, so a call whose answer depends on another node's progress (a
  `connect` to a node that may already have exited) got whichever answer
  the kernel's scheduling gave: a model could give different verdicts
  on different runs, and one outcome could never be seen.  STEPs are not
  checked, counted, or part of the depth; a counterexample lists them
  unnumbered.  Distinct-state counts and depths of every model whose
  nodes run one thread are unchanged; "states generated" grows by the
  worlds the STEPs reach (paxos 964 to 983).  Two-node models with a
  threaded node double (twothread 515 to 1,028, twothread_join 275 to
  549, raceborn 278 to 556), since the start order of the nodes is now
  part of each threaded tree.
  [proc_gate: dialexit, which needs both outcomes; badptr and dialexit
  each run 300 times and must give one verdict and count]
- The file system is read-only to a node.  Every open is intercepted:
  a read-only one goes to the kernel, and one with write access,
  `O_CREAT`, `O_TRUNC`, `O_APPEND` or `O_TMPFILE` ends the check (exit
  3) naming the path, except `/dev/null`.  The calls that change the
  namespace or metadata are refused, as are `ftruncate`, `fallocate`,
  `flock` and `io_uring_setup`.  On 2.0.0 a node that kept its state in
  a file passed a crash test on bytes that outlived the crash outside
  the world, and a worker's `lseek` on a shared fd was not seen.
  `lseek`, `fsync`, `fdatasync`, `ftruncate` and `sync_file_range` on a
  checker-owned fd get Linux's answers for a socket, and `fsync` of fd
  193 succeeds.  A node built with `--coverage` is refused: it writes
  its counters to files.  [proc_gate: files persist, lseek, config,
  devnull, badpath, durfsync, sockerr (against a native run), and one
  row for each refused call]
- The default bound on a threaded program's tree search (1,000,000
  worlds) can be set at build time with `-DCMC_TREE_CAP=n`, so a gate
  reaches it: the search stops there INCOMPLETE (exit 2) and
  `--max-states` still takes precedence.  On 2.0.0 no gate reached the
  default.  One run to the real default is recorded in
  `docs/measurements/2026-11-17-tree-cap.md`.  [proc_gate: tree bound
  rows; docs_gate: the default cmc(1) and cmc(7) state]

- Each checker-owned fd keeps its own flags.  On 2.0.0 a `fcntl` on an
  epoll fd or a socket not yet dialled read and wrote the listener's,
  so a client made non-blocking before `connect` made the node's
  listener non-blocking and itself blocking, and an eventfd's
  `FD_CLOEXEC` read back 0.  A non-blocking `connect` (EINPROGRESS)
  ends the check (exit 3).  `poll` and `epoll_wait` no longer call a
  listening socket or an epoll instance writable, report a socket never
  dialled with `POLLOUT | POLLHUP`, and report a closed fd with
  `POLLNVAL` at once instead of parking.  [proc_gate: syscorner eventfd,
  poll, nbepoll, nbclient, pollclosed, each against a native run]
- A socket call on the wrong kind of fd gets Linux's errno.  On 2.0.0
  `accept` on a connected socket, a plain socket or an eventfd took the
  listener's next connection, `listen` on an eventfd made it the
  listener, `connect`, `bind` and `setsockopt` on an eventfd or an
  epoll instance succeeded, and `sendto` and `recvfrom` on an eventfd
  worked as a write and a read: a program with such a slip passed.
  Now ENOTSOCK, EINVAL, EISCONN, EBADF, ENOTCONN or EPIPE, as Linux
  gives.  [proc_gate: syscorner wrongfd, notconn, against a native run]
- A node's channels to other processes end the check (exit 3): a
  second `execve`, SysV shared memory, message queues and semaphores
  and POSIX message queues (the SysV objects outlive the process, so
  their contents survived a crash outside the world), `process_vm_readv`
  and `process_vm_writev`, and a signal or pidfd to any other process,
  the checker's process group included.  A signal to the node itself
  still goes to the kernel.  On 2.0.0 each went unseen.  [proc_gate:
  edges exec, shm, vmread, mq, killparent, killgroup, pidfd, killself]
- `epoll_wait` into memory the checker cannot write returns EFAULT, as
  Linux does, instead of ending the check.  [proc_gate: syscorner robuf]
- A refusal of `dup2`, `dup3`, `splice` or `sendfile` with a checker-owned
  fd at either end names the owned fd, not the call's first one.
  [proc_gate: edges dup2, dup3, splice, sendfile]

Model files:

- An invariant that mixes `and` and `or` at one level without
  parentheses is a parse error.  They associated left to right with
  equal precedence, so `a or b and c` meant `(a or b) and c`, which a
  reader who expects `and` to bind tighter takes for a different
  invariant.  Every shipped model file still parses.  [proc_gate:
  inv-mixed, inv-grouped]

Coverage:

- `make coverage` builds with gcc `--coverage`, runs every gate of
  `make check`, and fails unless function and branch coverage of each
  `src/*.c` file and overall are at least 90%; CI runs it.  The node
  side (the preload and the race runtime) is measured in process
  (`test/proc/preload_alloc.c`, `test/proc/race_rt.c`).  Each
  `GCOVR_EXCL` marker sits on a check that cannot fail in a test, or on
  the node launcher's code past the point its counts can be written,
  with its reason; the report lists them.  At release: functions
  98.2%, branches 91.5% (cmc.c 98.7% and 91.5%, cmc_cli.c 100% and
  99.0%, cmc_preload.c 100% and 100%, cmc_proc.c 96.2% and 90.2%,
  cmc_race.c 100% and 100%, cmc_spec.c 100% and 90.1%).  At 2.0.0:
  96.7% and 69.6%.

Command line, API and the abstract checker:

- A number given to a `cmc` option must be a whole decimal number in
  the option's range: `--crashes x` used to read as 0 and pass a bug
  that needs a crash.  `--walks 0` and, with `--model-file`, `--depth 0`
  are refused, as is `--refine` without `--model-file`.
  [test_cmc: test_cli_input]
- API: `cmc_run_new()` returns NULL for a `CMC_SIMULATE` search with
  `walks` of 0, which looked at no state and passed.
  [test_cmc: test_min4_negative_bounds]
- API: `cmc_run_new()` returns NULL for a `search` that is none of
  `CMC_BFS`, `CMC_DFS`, `CMC_SIMULATE` and `CMC_REPLAY`: `cmc_check()`
  ran no search for it and reported `CMC_PASS` with 0 states.  It also
  returns NULL for `CMC_REPLAY` with no `replay_in`, which would open a
  NULL path.  [test_cmc: test_min4_negative_bounds]
- API: a BFS stopped by `stop_on_witness` is `CMC_INCOMPLETE`, not
  `CMC_PASS`: it had not searched the rest of the graph, so toy-bug1's
  Mutex violation passed with it set.  [test_cmc: test_stop_on_witness]
- `cmc --spec` checks the type of every operand: `not {1}` held, and
  `x < {1}` compared x with 0, so an invariant with a type slip passed
  vacuously.  Arithmetic, orders, ranges, set elements and function
  entries take integers; `and`, `or`, `not`, quantifier bodies, guards,
  invariants and goals take booleans; `==` and `!=` refuse two types.
  Each breach is a model error, exit 3.  No shipped spec changed its
  counts or verdict.  [spec_gate: "the evaluator refuses an operand of
  the wrong type"]
- A `--bind` file is read whole.  A second binding of a variable used
  to replace the first, so `x = "node[0][0]"` then `x = "0"` hid the
  observable and accepted a step the spec forbids.  A variable bound
  twice, an unknown section or variable, a line with no `=`, an index
  on a scalar variable or none on a function, and a `[fields]` offset
  that is not 0 to 254 are errors now.  [spec_gate: "a variable bound
  twice is an error" and the rows after it]
- A `symmetry` declaration that cannot be evaluated, or that names
  more than 8 values, is an error; such a spec was searched without the
  reduction and no word said so.  [spec_gate: "symmetry x is an error"]
- `cmc --refine` checks each bound variable of the observed abstract
  state against its declared domain.  An implementation whose observed
  value left the spec's state space (x = 7 for `var x : 0 .. 2`) used
  to refine whenever the invariant held there; it is an error now
  (exit 3).  [spec_gate: "C1: an observed value outside its domain is
  refused"]
- `cmc --emit-tla` translates `not`, `in`, `some` and `all` inside a
  `count` body, which it copied as .cml words, so TLC refused the
  module.  [emit_gate: countops; spec_gate: "emit-tla: not, in, some
  and all in a count body"]
- `--define TAG` that gates no action in the spec is an error, so a
  mistyped tag no longer checks the spec without its planted bug.
  [spec_gate: "a --define that gates no action is an error"]
- `--tlc-cfg` reads TLC's one-line forms (`INVARIANT TypeOK`,
  `CONSTANT N = 3`), which it used to drop, and refuses a keyword it
  does not implement (`SYMMETRY`, `CONSTRAINT`, ...).  Core settings
  longer than the buffer are an error, not cut off.
  [test_cmc: test_cli_input]

## 2.0.0 (2026-11-17)

A soundness release, from an audit of 1.3.0
([docs/plans/soundness-remediation.md](docs/plans/soundness-remediation.md)).
The audit found ways for every engine to report a pass where a bug
existed, gates that passed after their tool failed, and documentation
that claimed more than the gates showed.  Each fix below has a test that
fails on 1.3.0, named in brackets after it (a gate script and its row
or fixture), unless the bracket says otherwise.  [docs/qualification.md](docs/qualification.md) lists what
each engine answers exactly, approximates, or refuses, with its gate.

The major version changes because results change for the same input.  A
model that passed on 1.3.0 may now fail, end INCOMPLETE (exit 2), or be
refused (exit 3), and state counts move.  Where an example's 1.3.0
result was an artifact of a bug, its README says so.

Abstract checker (`cmc --spec`):

- Liveness follows TLA+ semantics.  Stuttering counts as a behaviour,
  every fair bad component is checked, and the backward search for
  `A ~> B` stops at B.  The verdicts agree with TLC on five probes
  where 1.3.0 disagreed, and a graph with many edges per state is
  checked whole.  [spec_gate: sound/leads, leadscross, once, unfair,
  dead, edges; emit_gate runs the first five through TLC]
- The seen set compares states exactly, under symmetry as well: a
  fingerprint match is confirmed against the stored canonical state.
  [no failing test: a 64-bit collision cannot be produced at gate
  sizes; spec_gate's symmetry and count rows show no count moved]
- Model errors are errors.  These used to change the model silently and
  now exit 3: text that overflows a buffer; a non-set domain; a value
  outside its declared domain; integer overflow; `/` or `mod` on a
  negative; a primed variable read; an init that reads an unset
  variable; a variable assigned twice; a bound name that shadows another.
  [spec_gate: sound/truncate, typepass, boundpass, succbound, overflow,
  primed, init_order, duplicate, quantifier]
- Liveness under `symmetry` is refused.  [spec_gate: sound/symlive]
- `--refine-steps` (C2) checks the first state against init, skips
  actions gated off by `--define`, and compares states exactly.
  [spec_gate: sound/c2gate, c2init, scalar.bind]
- The spec gate also runs under ASan and UBSan.  [make check-asan]

Process engine (`cmc --model-file`):

- Each TCP connection has its own queues.  On 1.3.0, two connections
  between the same two nodes shared bytes.  [proc_gate: crossrpc,
  twoconn]
- `close()` sends a FIN, or an RST when bytes are left unread; a reset
  keeps the bytes already sent; an exited listener resets its backlog;
  a refused socket reads `ENOTCONN` and can be dialled again.  After a
  reset the next call reports `ECONNRESET`, then writes fail with
  `EPIPE` and the `SIGPIPE` the node's disposition decides.  Each case
  matches Linux loopback, which the gate runs natively beside cmc.
  [proc_gate: tcpsem finwrite, rstdata, backlog, redial]
- A `connect()` to a node that is down is refused.  [proc_gate:
  unmodelled refused]
- A node starts with every signal at its default and none blocked,
  whatever cmc inherited.  [proc_gate: the SIGPIPE row under `trap ''
  PIPE`]
- Worlds are deduplicated by their exact encoding, not a hash.  The
  encoding gains the durable record and its read offset, every polled
  fd, epoll user data, the futex word, a recv's requested length, and
  the connection flags.  Nodes run without ASLR, so addresses repeat
  across replays.  [proc_gate: fingerprint; fpfuzz, 3000 random worlds
  with mutations that must change the encoding and changes it must
  hide]
- Threads are scheduled by the search.  In a node with more than one
  thread, each `futex` call, call on a checker-owned fd, state-hook
  write, `clone` and thread exit ends the thread's step, and the search
  chooses which thread runs next (`run n1 t1`), births included: a new
  thread and its parent are held at the clone's return.  A world with
  such a node is never merged with another, so the search over it is a
  tree, INCOMPLETE after `--max-states` or 1,000,000 such worlds.  A
  call on a kernel fd (a pipe, a file) from such a node is refused,
  except writes to stdout and stderr.  Interleavings of memory accesses
  between two visible calls are not explored.  A single-threaded node
  is unchanged.  On 1.3.0 a multi-threaded node ran in one order.
  [proc_gate: racewake, raceemit, raceborn, threadpos, durack,
  kernelfd, racepipe, twothread, twothread_join; twothread_join run
  1000 times on two loaded CPUs]
- State-hook writes are carried out by the checker, so a crash can fall
  between a durable write and the next call.  [proc_gate: durack]
- A node that aborts (a failed `assert`) or faults is reported as a
  violation, and a thread blocked in a call the checker does not see
  ends the check after 10 s; both used to hang it.  [proc_gate: fault,
  kernelfd]
- These are refused (exit 3): a finite `poll`, `ppoll` or `epoll_wait`
  timeout; a timed futex wait; `EPOLLET`, `EPOLLONESHOT` and other epoll
  flags; `EFD_SEMAPHORE`; a state record longer than 64 bytes or left
  unfinished; a `SIGPIPE` handler or blocked `SIGPIPE` on a write into
  a reset connection; `fork`, `vfork` and a `clone` that is not a
  thread; a socket other than `AF_INET` `SOCK_STREAM`; `accept4`,
  `recvfrom` and `sendto` flags or addresses the model does not
  follow; and `readv`, `writev`, `pread`, `pwrite`, `recvmsg`,
  `sendmsg`, `shutdown`, `dup`, `ioctl`, `splice`, `sendfile` and the
  `*sockopt`/`*name` queries on a checker-owned fd.  [proc_gate:
  unmodelled pollto, epollet, semaphore, longrec, partrec, rsthandler,
  fork, udp, nbaccept, peek, writev, sendmsg]
- A write from a buffer the node does not own gets `EFAULT`, and a
  partial transfer of a call's arguments ends the check.  On 1.3.0 it
  was a zero-filled success.  The preload allocator checks sizes and
  copies exactly on `realloc`.  [proc_gate: unmodelled badptr,
  preload_alloc]
- A safety-only model whose initial state enables no action is
  INCOMPLETE, not a pass.  [proc_gate: noprogress]

In-process engine (`cmc --model`, `libcmc(3)`):

- A crash clears the node's messages to itself.  [test_cmc]
- Simulation and replay check every state, including the last.
  [test_cmc]
- `cmc_run_new()` refuses more than 64 properties of one kind, and a
  property of an unknown kind.  [test_cmc]
- The fair schedule cannot postpone an armed timer forever.  [test_cmc]

Examples:

- raft.c and hc.c served no requests while waiting on their own.  Under
  1.3.0's shared channels, a waiting node was handed its peer's request
  as the reply.  Both now serve while they wait.  raft's double-vote bug
  is caught again, through real schedules.  [make check-examples,
  check-examples-slow]
- hc's pass on 1.3.0 was vacuous: no participant ever adopted.  Now a
  participant adopts, and the model requires it.  [make
  check-examples-slow]
- paxos2 decides, and its planted bug breaks the shared spec's
  agreement.  [make check-examples: paxos2-refine]
- The Paxos family follows Lamport's Paxos.tla: `paxos.cml` (with a TLA+
  twin), and new `fpaxos.cml` (Flexible Paxos quorums), `caspaxos.cml`,
  `zab.cml` (epochs, discovery and the committed prefix) and
  `fastpaxos.cml`.  cmc's counts agree with TLC, and Apalache agrees.
  [emit_gate, apalache_gate]

Gates and release:

- The Apalache gate fails on a tool error.  A missing tool fails the
  gate under `CMC_REQUIRE_TOOLS=1`.  [apalache_gate --self-test]
- CI runs the TLA+ bridge gates (TLC and Apalache), the spec gate under
  ASan and UBSan (`make check-asan`), and the slow example rows (`make
  check-examples-slow`) before a tag.

## 1.3.0 (2026-11-14)

Tier 3 of the davros evaluation: the process front end now models the
runtime machinery a production event-loop server actually uses, so it
can be checked as it ships.  A node may run multiple threads
(`clone`/`futex`, with a search-driven schedule); a crashed node can be
relaunched (a budgeted RESTART with durable recovery); and `epoll` (not
just `poll`) is modelled, which is libxtc's default Linux backend.

Per-thread scheduling in the process front end (`cmc --model-file`,
Tier 3 of the davros evaluation): a node may now run more than one
thread.  The checker intercepts `clone`/`clone3` and `futex`, keeps a
deferred slot per thread (keyed by the kernel thread id), and treats a
thread parked in `futex` (a mutex, a condition variable, or a
`pthread_join`) as blocked.  The world is a search state only once every
thread of every node is blocked or has exited, so a loop thread in
`poll`/`accept` and a worker thread that waits on a condition variable
are both driven by the search rather than tripping the LULL timeout or
racing unmodelled.  A `FUTEX_WAKE` wakes waiters only when the program
issues it, as part of the waking thread's step; woken threads run one at
a time so the kernel's atomic futex and mutex handoffs are never torn.
The fingerprint carries each thread's blocked state as a canonical
multiset keyed by role, not by the raw (arbitrary) kernel tid, so two
runs are deduplicated identically.  A single-threaded node behaves
exactly as before.  A `FUTEX_WAKE` that would wake a strict subset of
several waiters on one word (a lock contended by a thread pool) is an
error rather than a silently chosen schedule; PI and requeue futex
operations are likewise refused.  Example: `test/proc/twothread.c` (the
handoff holds; `-DBUG` drops the data across it and the checker prints
the two-action counterexample) and `test/proc/twothread_join.c` (the
exit and join path).

Relaunch a crashed node in the process front end (Tier 3 of the davros
evaluation): a new `[environment] restarts = N` budget (default 0)
enables a RESTART action that relaunches a crashed node as a fresh
process.  A crash still keeps only the durable encoding (descriptor
193); a RESTART hands that surviving record back to the new process, so
its first read of descriptor 193 returns what it had made durable before
the crash (a process that never crashed reads EOF there) and a server
that reads its own log on startup recovers.  The relaunched process
starts with no connections and re-dials and re-listens as its code does;
a peer that still held a connection to the crashed process reads EOF on
it.  The relaunch is a pure function of the surviving durable record, so
stateless replay stays deterministic, and a restarted node is driven to
quiescence like any launch.  With no restart budget a crash stays final,
exactly as before.  Example: `test/proc/restart.c` (crash-and-recover
holds; `-DBUG` forgets to recover and the checker prints the
`crash` then `restart` counterexample; without the budget the crash is
final and the bug is not explorable).

Model `epoll` in the process front end (Tier 3 of the davros
evaluation, ask 9): the checker intercepts
`epoll_create1`/`epoll_create`/`epoll_ctl`/`epoll_wait`/`epoll_pwait` with
complete virtual-fd semantics.  An epoll instance holds a persistent
interest list built by `epoll_ctl` (`ADD`/`MOD` capture the requested
events and the opaque `epoll_event.data`; `DEL` removes; an unknown op is
an error); `epoll_wait` answers local readiness (an eventfd counter, a
requested `EPOLLOUT`) at once and otherwise defers as the same `BK_POLL`
quiescence point `poll` uses, so the search drives the loop and epoll
readiness never auto-delivers (M5).  When the search makes a watched fd
ready, `epoll_wait` returns the `epoll_event.data` the loop registered
for that fd verbatim (a `struct epoll_event`, not a `pollfd`), because
that data is how an event loop routes to the fd's handler; the data is
opaque (an ASLR pointer in a real loop) and stays out of the fingerprint,
while the interest list itself goes in, canonicalised by each fd's
semantic role.  This matters because libxtc's default Linux backend is
epoll, so without the model the checked binary ran the loop on real
kernel fds and the pass was a fiction.  Level-triggered only; `EPOLLET`
(edge-triggered) is not modelled.  Example: `test/proc/epollloop.c`, the
epoll twin of `pollloop` (the correct loop holds; `-DBUG` stores the
wrong byte and the checker prints the counterexample naming the epoll
wake).

## 1.2.0 (2026-11-14)

Event-loop runtime support hardened from the davros evaluation: the
process front end now models the socket setup, non-blocking accept and
TCP reset semantics a real event loop needs, and no longer passes a
program that never advances past its initial state.

Process front end (`cmc --model-file`):

- Event-loop socket setup is modelled: `fcntl` now handles `F_GETFD` and
  `F_SETFD` (the `FD_CLOEXEC` flag, stored and returned, inert in the
  model) instead of failing, so a runtime that sets close-on-exec on
  every socket gets past `fcntl`.
- `accept` on an `O_NONBLOCK` listener with nothing queued returns
  `EAGAIN`, so an event loop that drains the backlog (accept until
  `EAGAIN`) makes progress instead of parking on the second accept.
- `listen` records the listener fd; the poll wake matches the listener
  by that fd and ignores negative fds, instead of treating any polled fd
  that is neither a connection nor an eventfd as the listener (which
  wrongly woke a kernel self-pipe or an unused `fd -1` slot).
- A no-progress self-loop is reported, not passed vacuously: if the
  search is exhaustive and every invariant holds but no action ever
  changed the world (every step was a stutter), the program never
  advanced past its initial state, so `cmc` reports INCOMPLETE (exit 2)
  and advises pairing the safety invariant with an `eventually` property,
  rather than a clean pass.  `test/proc/noprogress.c` is the guard.
- `[target] max_write` raises the per-write limit (default 64) up to a
  4 KiB cap, for real payloads larger than a tiny control message.
- `[target] args = ...` passes extra argv after the node id, so a target
  that takes flags runs under `--model-file` without a wrapper.
- A drop models a connection reset, not a lost byte.  TCP never loses a
  byte from a live stream, so losing one message from the middle of a
  channel was a counterexample the network cannot produce.  A drop now
  discards every queued message on the connection in both directions and
  both ends read EOF (as after `ECONNRESET`); a write into a reset
  connection reaches no one.  The reset set is part of the fingerprint.
  `test/proc/reset.model` is the regression guard: with the old silent
  loss it reported a spurious violation, with the reset it holds.
- `time()` and the aligned allocators (`posix_memalign`, `aligned_alloc`,
  `memalign`) go through the preload.  A target that seeded from
  `time(NULL)` read the real wall clock, and an aligned buffer came from
  glibc's ASLR'd heap, either of which broke replay determinism.  `time()`
  now reads the logical clock and the aligned allocators carve from the
  fixed-address arena.  `CMC_ARENA_MB` sets the arena size in MiB
  (default 64) for a larger target.
- The checker sets `CMC_STATE_FD` (the volatile fd, 3) and
  `CMC_DURABLE_FD` (the durable fd, 193) for every node, so a program
  that keeps its state hook in the build it ships can guard every emit on
  `CMC_STATE_FD` and never write a stray descriptor outside a check.
- `test/proc/guarded.c` exercises the guarded hook and the preload's
  determinism; it is gated in `proc_gate`.
- cmc(1), cmc(7) and the model-file guide describe the process front
  end's drop (a reset), crash (final, no restart yet) and determinism
  contract accurately.

## 1.1.0 (2026-11-13)

Async-I/O support for event-loop runtimes, so a program that waits in
`poll` with non-blocking sockets and `eventfd` wakeups (rather than
blocking sockets) can be model-checked as it ships.  Plus the Fast Paxos
certified model and the automated Codeberg release from 1.0.0's tail.

- Async-I/O support for event-loop runtimes: the process front end
  intercepts `poll`/`ppoll`, `fcntl(F_GETFL/F_SETFL)` for the
  `O_NONBLOCK` flag, and `eventfd2`.  A `poll` with nothing locally
  ready (only a requested `POLLOUT` or a non-zero eventfd answer at
  once) is a quiescence point: the node defers, and a new WAKE action
  marks one polled fd ready so the node accepts or reads it and defers
  again, which the existing ACCEPT/DELIVER/TIMEOUT actions service.
  Poll readiness is a search decision, never automatic, so the M5
  no-auto-delivery property holds for event loops as it does for
  blocking recv.  A non-blocking read with nothing deliverable returns
  EAGAIN; an `eventfd` is a counter a write bumps and a read drains.
  The world fingerprint carries each connection's `O_NONBLOCK` flag, a
  polling node's fd set, and the eventfd counters.  A new example,
  `test/proc/pollloop.c`, checks a poll-driven server and is caught by
  `-DBUG` when it stores the wrong byte (gated in `proc_gate`).

- The Codeberg release is published by `make codeberg-release VER=X.Y.Z`
  (`scripts/codeberg-release.sh`), run after the GitHub release for the
  tag publishes.  Codeberg Actions are disabled, so the forgejo release
  workflow does not run; the script creates the Codeberg release through
  the API and attaches the same tarballs the GitHub workflow built, so
  both forges carry identical artifacts.  It is idempotent.  The release
  procedure is written up in `RELEASING.md`.

- A certified abstract model for Fast Paxos
  (`examples/spec/fastpaxos.cml`).  A fast round lets clients send
  values directly to acceptors and so may collide; its distinctive
  safety result is that the fast quorum must exceed `3A/4` for a classic
  recovery round to resolve a collision.  The model has two rounds (a
  fast round, then a classic recovery) and tracks the bound faithfully:
  with `A=5, F=4` (> 3.75) it holds, and with the correct recovery rule
  but `F=3` (<= 3A/4) Agreement breaks, as Lamport's analysis says.  The
  `--define BUG` lets recovery use a classic-majority threshold where a
  fast quorum is needed.  Certified against TLC and Apalache; the
  Apalache gate's bound is raised to length 10 so the seven-step Fast
  Paxos counterexample is reachable.

## 1.0.0 (2026-11-12)

The first stable release.  The public interface in `include/cmc.h`, the
command line, the model file and the abstract `.cml` language are now
stable under semantic versioning.  The engine checks the program you
ship, with the TLA+-scope milestones A through F in place (a value
model, the abstract `.cml` language with modules, the binding and
refinement checker with action-level refinement, the TLA+ bridge to TLC
and Apalache, symmetry reduction and weak-fairness liveness), and a
library of certified abstract models spanning the consensus family and
beyond.  Milestone G (scale) continues past 1.0 as a performance track:
the fork-from-snapshot search is parked on a branch and the default
breadth-first engine is the sound path.

Since 0.5.0:


- A certified abstract model for two-phase commit
  (`examples/spec/twophase.cml`), the canonical non-consensus
  distributed-systems example (Lamport's `TwoPhase.tla`).  Consistency
  (atomicity) -- a committed and an aborted resource manager never
  coexist -- holds because a resource manager commits only on a
  unanimous prepared vote; the `--define BUG` commits without that
  check.  Certified against TLC and Apalache.  This broadens the example
  set beyond the quorum-based consensus family to an all-or-nothing
  atomic-commit protocol.

- A certified abstract model for CASPaxos (`examples/spec/caspaxos.cml`),
  single-decree Paxos on a register via a client change function.
  Agreement (one value per register version) holds because the client
  reads from a quorum before committing, so its read intersects any
  prior commit; the `--define BUG` stale read overwrites an acceptor and
  two values commit for one version (a lost update).  Certified against
  TLC and Apalache.
- Running every shipped example is now an explicit release gate: the
  `check-examples` target builds and checks every process-mode example
  (`examples/*/`) and every abstract model (`examples/spec/*.cml`), and
  `make check` (which the release workflow runs on the unpacked tarball)
  depends on it.  Previously the example gates ran inside `check-unit`
  as a side effect; naming the dependency makes a release unable to ship
  an example that no longer runs.
- `calib/hc` is kept, not retired.  Milestone D envisaged retiring the
  hand-kept `HummingConsensus.tla` once a certified HC `.cml` reproduced
  its rows through the bridge; `examples/spec/hc.cml` is certified but a
  minimal single-property model that does not reproduce those rows (many
  properties and variants at up to tens of millions of states), and the
  in-process engine it calibrates is a documented, shipped path.  The
  design notes and plan are corrected to say so.

- A certified abstract model for Flexible Paxos
  (`examples/spec/fpaxos.cml`), the flagship of the Paxos-variant test
  matrix and a direct expression of Heidi Howard's theorem: Agreement
  holds iff every Phase 1 quorum intersects every Phase 2 quorum, for
  which `Q1 + Q2 > A` suffices.  With `A=5, Q1=4, Q2=2` it holds; the
  `--define BUG` uses the Phase 2 quorum size for the Phase 1 guard, so
  the cross-phase intersection fails and two values are chosen.  The
  model tracks the inequality faithfully: with the correct guard but
  `Q1=3` (so `Q1 + Q2 = A`, not `> A`) Agreement also breaks.  Certified
  against TLC and Apalache like the other specs.
- `docs/fit.md` ("When libcmc fits"): where an explicit-state checker of
  the shipping code is the right tool (crash-fault agreement and
  replication at a small instance, the quorum-intersection knob, commit
  durability, refinement of a real binary) and where it is the wrong one
  (liveness and partial synchrony, real-time and timing, Byzantine and
  cryptography, performance, state spaces no bounded search reaches).
- `docs/research/03-paxos-variants-and-howard.md`: a survey of Heidi
  Howard's consensus research and the Paxos family, with the one safety
  invariant and the one planted-bug knob for each variant, and a fit
  assessment for explicit-state bounded checking.

## 0.5.0 (2026-11-11)

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
