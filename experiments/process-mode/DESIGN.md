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
