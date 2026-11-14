# Tier 3 Ask 6: relaunch a crashed node (RESTART)

## The problem

In the process front end (`cmc --model-file`) a crash is final: `run_seq`
launches every node once at the start of a replay, and `crash_node`
SIGKILLs a node and never relaunches it.  So "a worker crashes and
restarts while the coordinator reconnects" and "the server restarts and
the client retries" are not explorable.  A recovery property (a value
acked-durable before a crash is still present after the node comes back)
cannot be stated: the node never comes back.

## The risk (libcmc-change step 1)

A RESTART action relaunches a process mid-prefix.  The engine is
stateless-replay: every state relaunches all nodes and re-drives the
action prefix.  If a relaunched process does not reach the SAME quiescent
state on every replay, the search diverges and the checker is unsound (a
crash that "restarts" but silently diverges is worse than no restart).
The new action must therefore be a pure function of the world at the
point it fires.

## The design

### Budget
- `[environment] restarts = N` model-file key (default 0), like
  crashes/drops/timeouts.
- `bud.restarts` in Budget, set from `M.restarts` in reset_world, in the
  fingerprint next to the other budgets.
- A RESTART is enabled only for a node that is currently CRASHED
  (`!alive && crashed`), and only while `bud.restarts > 0`.

### Durable read-back
Today a target only WRITES fd 193; nothing reads it.  A real recovering
process reads its write-ahead log.  So on RESTART the engine relaunches
the node with its surviving durable record readable on fd 193: the fd-193
pipe is pre-filled with `[len][durable bytes]` before exec, and the
recovering target reads its own record back and re-emits it.  A target
that does NOT read fd 193 on restart recovers nothing (its canon is
empty until it rebuilds it), which is the -DBUG the checker catches.

The durable bytes survive in `Node.durable[]` across the crash already
(crash_node keeps them).  relaunch() seeds the child's fd-193 read end
with them, then continues to accept new durable writes on the same fd.

### Connection reset at peers
Any peer with a live connection to the crashed node must see it gone.
The crash already left the node dead; a peer parked in recv gets EOF via
the existing A_EOF action.  On RESTART we additionally reset every
node-pair involving the restarted node (reuse reset_conn from v1.2.0):
the restarted process starts with NO connections and re-dials/re-listens
as its code does, and a peer that still held a fd to the old process sees
a reset (EOF), never a silent hand-over to the new process.

### Threads
A crashed multi-thread node already loses its thread table (crash_node
clears nth/th_pending/pending_ctid).  relaunch() rebuilds it empty; the
restarted single-main-thread process repopulates it through th_for on its
first owned call, exactly as a fresh launch does.

### Determinism + replay
relaunch() is a pure function of (durable bytes at the crash point, the
node's code).  The durable bytes are deterministic along a replay (the
crash drained canon deterministically).  The relaunched process reads the
same bytes, runs the same code, and is driven to quiescence by
drive_quiescent exactly as a fresh launch is.  So the same prefix yields
the same state every replay.

### Fingerprint
A restarted-and-reconnected node is `alive` again (coded 3 in world_fp),
distinct from a crashed node (0xfe).  The `restarts` budget decremented,
the reset connection pair, and the recovered-vs-empty canon all enter the
fingerprint, so "crashed" and "crashed-then-restarted-and-reconnected"
are different worlds.

## The red test (test/proc/restart.c)

Two nodes.  Node 1 (server) stores a client value durably (fd 193) and
acks durable.  Node 0 (client) writes, waits for the durable ack.  The
checker crashes node 1, then RESTARTS it.  On restart the server reads
its fd-193 record back and re-emits it (canon recovered).

Invariant `no_lost_ack`: a value acked-durable is present on some node
after the restart.

-DBUG: on restart the server does NOT read fd 193 (forgets to recover),
so after the restart the committed value is on neither node and the
checker reports no_lost_ack violated.

Without a RESTART action (restarts = 0) the restart scenario is simply
not explorable -- the crash is final -- so the -DBUG build passes
vacuously, which pins that RESTART is what makes the property checkable.

## Implementation notes

The durable record travels to the restarted process through descriptor
193, which the engine now intercepts on read: a read of fd 193 serves
the surviving record (framed [count][bytes]) from the engine-owned
nd->durable and returns EOF once consumed, so the read is a pure
function of the world and replays identically.  A fresh launch has an
empty durable record, so the first read there returns EOF; every
existing fixture only writes fd 193, so this read path is inert for all
of them (durlog unchanged at 16 states).

A peer parked in recv on a connection to the restarting node is given
EOF and its stale connection is dropped (as a close of a reset socket
would); a re-dial queued at a peer survives so the new process accepts
it.  A peer parked in poll on such a connection is the one case the
pair-keyed reset model cannot represent faithfully alongside a fresh
re-dial, so the checker errors there rather than guess (an honest
boundary; ask 6 request/reply recovery does not reach it).

A found-and-fixed soundness bug during bring-up: crash_node left the
dead process listener and two pipe read-ends open in the parent, and a
RESTART opened fresh ones, so a long search (or a 100-action simulate
walk that crashed and restarted) leaked three fds per crash until the
parent ran out and a socket/recvmsg returned a bogus listener, which
surfaced as "node N reads fd 500, which is no connection".  crash_node
now closes those fds after its final drain.  BFS stays deterministic
(restart.c: 41 generated, 28 distinct, three identical runs); simulate
runs 3000 walks clean across seeds and still catches the -DBUG.
