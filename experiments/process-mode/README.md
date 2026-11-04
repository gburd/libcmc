# Process mode (prototype)

libcmc 0.1.0 checks the shipped object files, but only code written as
pure, deterministic step functions (`CmcCore`).  Process mode is the path
to the north star: proving C code written **as it will be deployed** --
real syscalls, normal control flow, no rewrite for the checker.

`DESIGN.md` is the design.  This is a time-boxed **M0 prototype**: not
part of the engine, not a gate, not built by `make`.  It answers one
question, feasibility, and nothing more.

## What M0 shows

`demo/server.c` is an ordinary one-connection TCP server: blocking
sockets, a real `write()`+`fsync()` to a file, no libcmc header and no
step function.  Built with `-DBUG` it sends its ACK before the `fsync`,
the classic "acknowledge before durable" bug.

`checker.c` runs that binary unmodified and model-checks it at the
syscall boundary.  It catches the bug:

```
$ ./run.sh
ok   correct server, no crash  -> exit 0 (client sent W 1; server ACKed; disk=1)
ok   correct server, one crash -> exit 0 (client sent W 1; server ACKed; disk=1)
ok   buggy server, no crash    -> exit 0 (client sent W 1; server ACKed; disk=1)
ok   buggy server, one crash   -> exit 1 (client sent W 1; server ACKed; CRASH before flush; disk=-1)
process-mode M0: ok
```

The bug needs a crash to show, exactly as in the step-function
quickstart (`docs/guide/02-first-model.md`) -- but here the code under
test is written as it ships.

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

## What M0 does NOT do

One connection, one client request, one branch (crash / no crash), single
thread.  No general search, no memory canonicalization, no `fork()`-based
branching over many interleavings, no clock, no threads.  Those are the
milestones in `DESIGN.md`, each with its own kill criterion, and only
after them does process mode become a second engine mode in `src/`.

## Run it

```
./run.sh          # Linux only; builds and runs the four cases
```

Artifacts (`server`, `server-bug`, `checker`, `pm-store.dat`) are
gitignored.
