# Process mode (prototype)

libcmc 0.1.0 checks the shipped object files, but only code written as
pure, deterministic step functions (`CmcCore`).  Process mode is the path
to the north star: proving C code written **as it will be deployed** --
real syscalls, normal control flow, no rewrite for the checker.

`DESIGN.md` is the design.  This is a time-boxed prototype in nine
milestones, not part of the engine, not a gate, not built by `make`.

- **M0** (`checker.c`, `demo/server.c`): catch a durability bug in a
  server written as it ships.
- **M1** (`search.c`, `demo/kv.c`): a real BFS over interleavings with a
  canonical state, so a state reached two ways is counted once.
- **M2** (`search2.c`, `demo/list.c`, `demo/detmalloc.c`): a real
  `malloc` heap, and the boundary it draws.
- **M3** (`mc.c`, `list.model`, `demo/list.c`): a declarative model file
  drives the checker -- the decided shape (one engine, a model file, the
  shipping binary).  See `docs/model-file.md` and the decision record
  `docs/background/decision-model-file.md`.
- **M4** (`mc2.c`, `repl.model`, `demo/repl.c`): multi-node.  The checker
  runs N node processes and owns the network between them, exploring the
  order inter-node messages are delivered.  First step toward the
  consensus examples.
- **M5** (`mc2.c`, `elect.model`, `demo/elect.c`): time as an environment
  action.  A blocked `recv` is satisfied by a message or by a bounded
  timeout the search fires, so leader election is explorable; a split
  brain is caught.
- **M5b** (`mc2.c`, `lease.model`, `demo/lease.c`): absolute time is
  engine-controlled.  `detmalloc.so` forces `clock_gettime` off the vDSO;
  the checker answers with a logical clock advanced only at timeouts, so
  a stale-lease write is caught and replay stays deterministic.
- **M6** (`mc2.c`, `durlog.model`, `demo/durlog.c`): crash and partition.
  The checker may CRASH a node (losing volatile fd-3 state, keeping
  durable fd-193 state) or DROP a message, both budgeted; a protocol that
  acks a client before the write is durable loses a committed write, and
  that is caught.
- **M7** (`mc2.c`, `demo/{idle,fifo,chain,bigmsg,paxos}.c`): honest
  verdicts.  Probes the M6 engine got wrong now give the right answer; a
  bounded run is INCOMPLETE (exit 2), a cap an error; any topology the
  nodes dial works; single-decree Paxos with five processes is checked
  exhaustively, and seeded `--simulate` finds its bug too.

## What M0 shows

`demo/server.c` is an ordinary one-connection TCP server: blocking
sockets, a real `write()`+`fsync()` to a file, no libcmc header and no
step function.  Built with `-DBUG` it sends its ACK before the `fsync`,
the classic "acknowledge before durable" bug.

`checker.c` runs that binary unmodified and model-checks it at the
syscall boundary.  It catches the bug:

```
$ ./run.sh
-- M0: a durability bug in a server written as it ships
ok   M0 correct, no crash  -> exit 0 (client sent W 1; server ACKed; disk=1)
ok   M0 correct, one crash -> exit 0 (client sent W 1; server ACKed; disk=1)
ok   M0 bug, no crash      -> exit 0 (client sent W 1; server ACKed; disk=1)
ok   M0 bug, one crash     -> exit 1 (client sent W 1; server ACKed; CRASH before flush; disk=-1)
```

The bug needs a crash to show, exactly as in the step-function
quickstart (`docs/guide/02-first-model.md`) -- but here the code under
test is written as it ships.

## What M1 shows

`demo/kv.c` is an ordinary server with two independent one-byte cells in
a declared region, `unsigned char cmc_state[2]`.  The client may SET a or
SET b; the two orders reach the same state.  `search.c` does a real
breadth-first search over every interleaving: it finds the `cmc_state`
address by its ELF symbol (so the target carries no checker API), hashes
that region after each step, and dedups with the same 128-bit
fingerprint actor mode uses.

```
ok   M1 correct kv (diamond merges) -> exit 0 (distinct states: 4; dedup merges: 5)
ok   M1 bug kv (invariant caught)   -> exit 1 (prefix: SET-b SET-a)
```

Four distinct states (`00, 10, 01, 11`), and the two paths to `11` hash
equal, so the diamond closes and the search terminates instead of
enumerating orderings forever -- the property that makes an exhaustive
search possible.  Built `-DBUG`, SET a also clears b; the invariant "a
cell once set stays set" catches it, and BFS reports the shortest prefix
that triggers it, `SET-b SET-a`.

## What M2 shows

`demo/list.c` keeps its protocol state in a `malloc`'d linked list, run
under a deterministic arena allocator (`demo/detmalloc.c`) the checker
`LD_PRELOAD`s.  It draws the honest boundary of process mode:

```
ok   M2 raw heap hits the wall (WALL)
ok   M2 canon merges reorders       -> exit 0 (distinct=8)
ok   M2 canon bug (lost key caught) -> exit 1 (prefix: INS-2 INS-1)
```

The deterministic arena defeats ASLR, so one schedule is reproducible.
But raw heap bytes still cannot merge two commuting insert orders -- the
set `{1,2}` built as 1-then-2 and as 2-then-1 places the nodes in opposite
arena slots, so the bytes differ (16 raw states where the logical count
is 8).  A ~8-line canonical-emit hook (the target writes its sorted keys
on fd 3) makes the orders merge and the search terminate, and the planted
lost-key bug is then caught.  The conclusion: process mode checks
arbitrary heap-using C along a fixed schedule with no target change; a
reconverging exhaustive search needs the small hook, far less than step
functions.

## What M3 shows

`mc.c` reads a declarative **model file** -- the decided shape (one
engine, a model file, the shipping binary; see `docs/model-file.md` and
the decision record).  It drives the whole run from the file:

```
[target]
nodes = 1
state = canon
[environment]
clients = 3
depth   = 4
[invariants]
no_lost_key = "all k in acted : k in node[0]"
```

```
ok   M3 correct (model file + DSL invariant) -> exit 0 (8 distinct states)
ok   M3 bug (DSL invariant caught)           -> exit 1 (client inputs: 2 1)
```

Nothing about the protocol is in `mc.c` or the model file; the running
binary is the transition relation.  The invariant is a small expression
DSL over `node[0]` (the canonical state) and `acted` (the client inputs
on the path here): quantifiers, membership, comparison, `and`/`or`/`not`,
and nothing more.  `all k in acted : k in node[0]` -- every inserted key
is still present -- holds for the correct build and is violated by the
planted lost-key bug, caught with the shortest input sequence `2 1`.

## What M4 shows

`demo/repl.c` is a two-node replicated register (primary + backup),
ordinary blocking sockets, no libcmc header.  `mc2.c` runs both as real
processes and is the network between them: it chooses the order inter-node
messages are delivered.  `repl.model` declares
`agree_on_ack = "all r in replies : r.val in node[1]"` -- every value the
client was acked is on the backup.

```
ok   M4 correct repl (agree-on-ack holds)   -> exit 0 (3 distinct states)
ok   M4 bug repl (acked write backup lacks) -> exit 1 (node[0]=5 node[1]=0)
```

The correct primary acks only after the backup stores the value, so the
invariant holds.  The `-DBUG` primary acks early; the checker finds the
schedule where the backup is still behind an acknowledged write.  A
blocked `recv` is a *deferred* seccomp notification -- the node stays
stopped in the kernel until the search delivers it a message -- which is
how one checker schedules several real processes.

## What M5 shows

`demo/elect.c` is a two-node election sketch: node 0 is the initial
leader and sends a heartbeat; node 1 (follower) blocks on `recv`.  The
checker delivers either the heartbeat or an election **timeout** -- time
is an environment action, bounded by `timeouts` in the model file.
`elect.model` asserts `one_leader_per_term`.

```
ok   M5 correct elect (one leader per term) -> exit 0 (3 states)
ok   M5 bug elect (split brain caught)      -> exit 1 (node[0]=1,1 node[1]=1,1)
```

The correct follower times out to leader at term+1 (never the same term
as the old leader); the `-DBUG` follower forgets to bump the term, and
the checker finds the split brain -- both leader at term 1.  A timeout
reuses the deferred-recv machinery (it is a second way to satisfy a
blocked `recv`), so M5 needed no new syscall interception.

## How it works

Seccomp **user notification** (Linux >= 5.0).  The child installs a
filter that returns `SECCOMP_RET_USER_NOTIF` for the syscalls the model
owns (sockets, the store file's open/pwrite/fsync, read/write of the
connection) and `ALLOW` for the rest, then `exec`s the server.  The
kernel gives the checker a listener fd that **survives the exec** -- the
reason a `SIGSYS` handler cannot be used (`execve` resets it).  The
checker reads each trapped syscall, serves it from the modelled world
(the network bytes, the staged disk byte, the crash), and reads or writes
the child's memory with `process_vm_readv`/`process_vm_writev` (after a
`PTRACE_SEIZE` for the rights).  A real file holds the durable byte;
`fsync` is what makes a staged write durable, so a crash modelled before
`fsync` loses it.

M1's search is **stateless prefix replay**: to reach a state it launches a
fresh server and replays the client messages that lead there, then hashes
the declared region.  No process snapshots; the cost is one re-execution
per state.  The region address comes from the binary's ELF symbol, which
is the runtime address only for a non-PIE build -- the checker refuses a
PIE rather than read relocated memory and report a wrong answer.

## What the prototype does NOT do

M2 drew the boundary: a real `malloc` heap canonicalises along one
schedule, but a reconverging search needs the small opt-in canonical-emit
hook; without it process mode still runs but counts reordered-equal
states separately.  Still out: several connections and nodes, the clock
and the vDSO, threads with a controlled scheduler, PIE targets, and
wiring libcmc's own engine (properties, replay, `--simulate`, the TLC
calibration) onto the process frontier.  Those are the milestones in
`DESIGN.md`, each with its own kill criterion; only after them does
process mode become a second engine mode in `src/`.

## Run it

```
./run.sh          # Linux only; builds and runs the four cases
```

Artifacts (`server`, `server-bug`, `checker`, `kv`, `kv-bug`, `search`,
`list`, `list-canon`, `list-bug`, `search2`, `detmalloc.so`,
`pm-store.dat`) are gitignored.
