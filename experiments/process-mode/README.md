# Process mode (prototype)

libcmc 0.1.0 checks the shipped object files, but only code written as
pure, deterministic step functions (`CmcCore`).  Process mode is the path
to the north star: proving C code written **as it will be deployed** --
real syscalls, normal control flow, no rewrite for the checker.

`DESIGN.md` is the design.  This is a time-boxed prototype in two
milestones, not part of the engine, not a gate, not built by `make`.

- **M0** (`checker.c`, `demo/server.c`): catch a durability bug in a
  server written as it ships.
- **M1** (`search.c`, `demo/kv.c`): a real BFS over interleavings with a
  canonical state, so a state reached two ways is counted once.

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

M1 canonicalises only a **declared region** the target names, not a real
allocator's heap, the stack or TLS -- generalising that is the next
milestone and the one that decides whether process mode scales.  Also
out: several connections and nodes, the clock and the vDSO, threads with
a controlled scheduler, and wiring libcmc's own engine (properties,
replay, `--simulate`, the TLC calibration) onto the process frontier.
Those are the milestones in `DESIGN.md`, each with its own kill
criterion; only after them does process mode become a second engine mode
in `src/`.

## Run it

```
./run.sh          # Linux only; builds and runs the four cases
```

Artifacts (`server`, `server-bug`, `checker`, `kv`, `kv-bug`, `search`,
`pm-store.dat`) are gitignored.
