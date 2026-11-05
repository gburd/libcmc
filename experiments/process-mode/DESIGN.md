# Process mode: model-check the code as deployed

Status: prototype, 2026-11-03.  Not part of the engine or any gate yet.
Lives in `experiments/process-mode/`.  This note is the design; the
prototype is judged against the kill criterion at the end.

## The ask

libcmc 0.1.0 checks the shipped object files, but only code written as
pure, deterministic step functions (`CmcCore`).  Code that uses sockets,
disk and timers the way it will in production is not checked, and its I/O
glue is not checked at all.

The north star is to prove C code written **as it will be deployed** --
real syscalls, normal control flow, no rewrite for the checker.  Process
mode is the path to it.  It follows CMC (Musuvathi et al., OSDI 2002),
which model-checked unmodified C protocol implementations, and MoDist
(NSDI 2009), which did it to production binaries by interposing on the
OS.

## The idea

Each node is the real program, built normally and run as a real process.
The checker owns the world outside the process: the network, the disks,
the clocks, which thread runs, and crashes.  It reaches that world by
intercepting the system calls the program makes.

- **A step is run-to-syscall.**  The process runs its own code until it
  makes a syscall the checker owns (send, recv, poll, fsync, a clock
  read, a lock).  Everything between two such calls is the deployed code,
  unmodified.
- **Nondeterminism is a branch.**  When the outcome of an owned syscall is
  a choice the environment makes -- which message arrives, whether a peer
  is up, how much of a write survived a crash -- the checker `fork()`s the
  process and lets each child take one outcome.  `fork()` is the
  state copy: copy-on-write makes it cheap.
- **A state is the program's memory.**  Two states are the same when the
  process's writable memory and the environment bytes are equal.  The
  checker hashes both and keeps a visited set, exactly as actor mode does
  with world bytes.

The existing engine is reused whole: the fingerprint table, the BFS
frontier, the environment model (FIFO/bag network, persists, crashes,
the ghost reply log), the properties, replay, and the TLC calibration.
What changes is where a "state" comes from and how a "step" runs.

## Interception

`SIGSYS` via `seccomp` user-notification is the mechanism (verified on
Linux 6.18, glibc 2.44, no ptrace restriction):

- A seccomp filter traps every syscall except the one issued from a
  single allowed instruction (the checker's own `raw()` helper, matched by
  instruction pointer) and `rt_sigreturn`.
- The `SIGSYS` handler reads the syscall number and arguments from the
  signal's `ucontext`, decides the outcome with the engine, and writes the
  return value back into the context.
- Pure-compute and memory syscalls (`mmap`, `brk`, `read`/`write` of a
  real local file the program owns) are satisfied by the handler issuing
  the real syscall through `raw()`.  Only the syscalls that touch the
  modelled world (network, modelled disk, clock, futex/park) become engine
  actions.

Interception is in the child, so it adds nothing to a normal build and
needs no `LD_PRELOAD` or `ptrace`.

## What the prototype proves (M0)

The smallest thing that answers "does this reach the north star": a
server written as it would ship, not as step functions, with the classic
durability bug, found and replayed.

- `demo/server.c`: a one-connection TCP server.  On a WRITE it issues a
  real `write()`+`fsync()` to a file, then `send()`s an ACK.  Built with
  `-DBUG`, it sends the ACK before the `fsync()`.  No libcmc header, no
  step function; it is an ordinary blocking server.
- The checker injects one client WRITE and runs the server under
  interception with two owned events:
  the ACK send, and a crash that may happen with the `fsync` still
  pending.  Property: after a crash-restart, a value the client saw
  acknowledged is on disk.
- Pass: the correct server passes; the `-DBUG` server fails with a
  replayable trace; and the correct server with no crash budget also
  passes (the bug needs a crash, as in the step-function quickstart).

M0 is deliberately single-threaded, one connection, one disk.  It is a
feasibility gate, not a product.

## The hard parts, named up front

1. **Canonical memory.**  Two runs that are the "same state" must hash
   equal.  Addresses from `mmap`/`malloc` and stack layout differ run to
   run.  M0 sidesteps this by hashing only a declared region (the
   server's data segment) plus the environment, and by pinning the
   allocator.  Beyond M0 this is the central problem; CMC spent 165-185
   lines per program on it.  If it cannot be made to work, process mode
   does not scale, and the step-function mode remains the answer for deep
   search.
2. **Threads.**  The scheduler must come under the checker.  Out of scope
   for M0 (single thread).
3. **The vDSO.**  `clock_gettime` and friends can be served from the vDSO
   without a syscall, escaping seccomp.  M0 reads the clock through a real
   syscall (or does not read it); a real target needs the vDSO disabled or
   patched.
4. **Speed.**  A fork and a memory hash per step is thousands of states
   per second, not the millions actor mode does.  Process mode is for
   checking real glue at shallow-to-moderate depth, not for the deep
   exhaustive runs the step-function cores get.
5. **Coverage gap by design.**  Bugs the protocol never drives the code
   into (a malformed packet in a parser) are for CBMC or a fuzzer, as a
   separate check.  Process mode checks reachable protocol behaviour.

## If M0 passes

Then, in order, each its own milestone with its own kill criterion:
canonical memory for a real allocator; several connections and nodes;
the clock and timers; threads with a controlled scheduler; wiring the
existing properties, replay and `--simulate` onto the process frontier.
Only after that does process mode become a second engine mode in
`src/`, with its own gates, rather than an experiment.

## If M0 fails

Record why in a measurement file, keep the step-function engine as the
shipping answer, and say plainly in `docs/WHY.md` and the README that
libcmc checks step-function cores, not arbitrary deployed code.  A fork
of CBMC is still not the answer (`docs/WHY.md`): it needs the same
environment model written as harness code, does not scale to protocol
depth, and refuses threaded shared-pointer code.

## M1: canonical state and a real search (2026-11-04)

M0 ran one path with one branch.  M1 takes the first milestone above --
canonical memory -- together with the smallest search that forces it: a
state reached by two different orders must be recognised as one, or an
exhaustive search over a reconverging protocol never terminates.  That
is M1's kill criterion.

**The checkable state is a declared region.**  Canonicalising a whole
glibc process image (an ASLR'd heap, stack, TLS, loader state) is the
research problem; M1 does not attempt it.  Instead the target names its
protocol state: `unsigned char cmc_state[N]` in BSS, which it zeroes at
startup and owns entirely.  The checker finds that region's address by
reading the `cmc_state` symbol from the binary with `nm` -- the target
carries no libcmc header and no checker API, so it is still code written
as it ships.  The fingerprint is `hash(cmc_state[0..N) || environment
bytes)`, the same 128-bit scheme actor mode uses.  For M1 the demo is
built `-no-pie` so the BSS address is fixed at link time; a PIE would add
reading the load bias from `/proc/PID/maps`, deferred.

**The search is stateless prefix replay** (MoDist, NSDI 2009), not
process snapshots.  The checker keeps a worklist of action prefixes.  To
explore a prefix it launches a fresh server under interception, replays
exactly that sequence of environment choices, reaches the end state,
hashes the declared region plus the environment, and -- if the
fingerprint is new -- enumerates the enabled next actions and pushes
each extended prefix.  No stopped processes are held; the cost is one
re-execution per state, which is the price of not snapshotting.  BFS
order gives shortest counterexamples, as in actor mode.

**The demo that forces dedup** (`demo/kv.c`): a server holding two
independent bytes `a`, `b` in `cmc_state`.  The client may SET a or SET b,
one message each.  Reachable states are `00, 10, 01, 11`; `11` is reached
both by `10 -> 11` and by `01 -> 11`.  A correct checker finds exactly
four distinct states, the diamond merged.  Built `-DBUG`, SET a also
zeroes b, so the order "set b, then set a" loses b; the invariant "a byte
once set stays set" catches it with a replayable prefix.  This is the
actor-mode toy lock-server's role, for process mode.

**Pass:** correct `kv` gives four distinct states with at least one
dedup hit (the diamond closed), invariant holds; `kv-bug` reports the
invariant violated with a prefix that reproduces it.  **Fail:** the two
orders to `11` hash differently (no merge, over-count, or a search that
does not terminate) -- then canonicalisation is the blocker and the
measurement says so.

Still out of M1: a real allocator's heap in the region, several nodes,
the clock, threads, and wiring libcmc's own engine onto the frontier.

## M2: a real malloc heap, and the wall it hits (2026-11-04)

M1 hashed a fixed BSS array: canonical because the bytes *are* the state
at a fixed address.  M2 asks the milestone question -- can process mode
check code whose protocol state is a **dynamic, pointer-linked structure
built with `malloc`**?  The answer is the honest boundary of the whole
approach, so M2 is built to find it, not to paper over it.

**A real heap breaks three things a fixed region did not.**  (1) `malloc`
returns different addresses run to run (heap-base ASLR, and placement that
depends on the allocation sequence).  (2) a pointer stored in the state is
an absolute address, so two logically equal states hold different pointer
bytes.  (3) chunk metadata, padding and freed-not-zeroed bytes are
nondeterministic noise between the live bytes.

**Step one: a deterministic arena defeats address nondeterminism.**  The
checker `LD_PRELOAD`s a tiny allocator (`demo/detmalloc.c`) that carves
`malloc`/`calloc`/`realloc` sequentially from an arena `mmap`'d
`MAP_FIXED` at a fixed base; `free` is a no-op (sound for a bounded
model-checking run).  The Nth allocation in a given sequence then lands at
the same address every run, whatever ASLR does, and the live state is
exactly `arena[0..bump)`.  The target still calls plain `malloc` and is
unchanged -- the preload is set by the checker at `exec`, so this stays
code written as it ships.  Hashing `arena[0..bump)` canonicalises the
heap *for a fixed allocation order*: pointers into the arena are now
reproducible, so re-running the same schedule dedups.  This is necessary,
and it works.

**Step two: the wall.**  A deterministic arena fixes *address*
nondeterminism but not *allocation-order* nondeterminism, and order is
exactly what two interleavings of commuting operations differ in.
`demo/list.c` keeps an ordered set as a `malloc`'d linked list; the client
inserts keys.  Insert 1 then 2 and insert 2 then 1 reach the same logical
set `{1,2}` -- but in the first order node 1 sits at arena offset 0 and
node 2 at offset 16, and in the second order they swap.  Same list, different
arena bytes.  So the diamond does **not** merge, and a reconverging search
over heap-using code explodes.  Raw-heap hashing cannot canonicalise a
logical state that is independent of allocation order.  This is the same
wall CMC (OSDI 2002) paid 165-185 lines per program to climb: it did not
hash memory, it walked the live object graph from roots in a deterministic
order and emitted a position-based encoding -- which needs the target to
describe its roots and layout.

**Step three: the fix is a small opt-in hook, not a rewrite.**  The
target may, when built for checking, emit a canonical encoding of its
logical state on a declared side channel (fd 3) after each change -- for
the list, the keys in sorted order.  The checker reads that and hashes it
instead of the raw arena; when the target emits nothing it falls back to
the raw arena and says so.  With the hook, insert 1 then 2 and insert 2
then 1 both emit `1,2` and the diamond merges: `demo/list.c` built
`-DCANON` gives the merged search, and `-DBUG` (an insert that drops an
existing key) is caught with the shortest prefix, checkable because the
encoding is parseable where raw arena bytes are not.  The hook is ~8
lines and leaves the protocol code as it ships; it is not the
step-function rewrite the whole project exists to avoid.

**Pass (what M2 proves):** (a) with the deterministic arena, re-running
one schedule dedups -- ASLR and run-to-run address noise are gone; (b)
without a canonicaliser, two commuting orders do **not** merge (measured,
the wall is real); (c) with the ~8-line fd-3 emit they do merge, the
search terminates, and the planted bug is caught.  The conclusion for
libcmc: process mode checks arbitrary heap-using C along a fixed schedule
(replay, simulation, DAG-shaped search) with no target change; a
reconverging exhaustive search needs the small canonical-emit hook, which
is far less than step functions.  **Fail:** if even (a) did not hold -- if
the deterministic arena did not make one schedule reproducible -- process
mode could not dedup at all and the step-function mode would be the only
answer.

Still out of M2: several nodes, the clock and the vDSO, threads with a
controlled scheduler, PIE load-bias, and wiring libcmc's own engine
(properties, replay, `--simulate`, the TLC calibration) onto the process
frontier.  Whether the canonical-emit hook becomes the standard way to
use process mode, or the declared-region/fixed-schedule forms suffice, is
the decision the next milestone turns on.

## M3: a model file drives the checker (2026-11-04)

The decision in `docs/background/decision-model-file.md` settled the
shape: one engine, a declarative **model file**, and the shipping binary
as the protocol.  M3 is the first cut of that -- the model file driving a
real run, end to end, so the pieces M0-M2 built stop being three separate
programs and become one checker reading a file.

`mc.c` reads a `.model` file (`docs/model-file.md`): the binary to run,
the node count, how to read state (`canon`), the environment budget, and
the invariants.  It runs the target under the M0-M2 interception, does the
M2 canonical-emit BFS, and evaluates each invariant -- written in a small
expression DSL over the node's canonical state and `acted` (the log of
client inputs on the path to this state) -- in every reached state.
Nothing about the protocol is in `mc.c` or the model file; the running
binary is the transition relation.

The DSL is deliberately not Turing-complete: quantifiers (`all`/`some V
in SET`), membership (`V in SET`), comparisons, `and`/`or`/`not`, over
`node[0]` (the canonical bytes) and `acted`.  `list.model` declares one
invariant, `all k in acted : k in node[0]` -- every key the client
inserted is still in the set.

**Pass (what M3 shows):** `mc list.model ./list-canon` holds over 8
distinct states; `mc list.model ./list-bug` reports the invariant
violated with the shortest client-input sequence (`2 1`) and the offending
state (`node[0] = 1`, key 2 lost).  The model file alone, no code change,
selects the binary, the environment and the property.  Separate tests
confirm the DSL evaluates quantifiers over every element (not just the
last) and that `some`/`all`/`not`/comparisons behave.

**`acted` is honest.**  It is the environment's own record of what it fed
the target, exactly like actor mode's ghost request log -- not a read of
the target's internals.  An invariant that needs the reply log ("every
acked value is durable") waits on M4, which adds the ghost reply log to
process mode.

Still out of M3: several nodes (the file allows `nodes` but `mc` handles
one), crashes and the reply log in process mode, named state fields and
the C-predicate escape hatch from the schema, and lifting `mc` out of
`experiments/` into `src/` as the engine's second front end.  The next
milestone is multi-node: several real node processes under one checker,
driven by the model file, against a small replicated server.

