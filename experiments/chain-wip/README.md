# Chain replication (work in progress): an engine limitation

`hc.c` is a partial Humming-Consensus-style chain-replication example
(the protocol libcmc was extracted from, pg_chain).  It is **not** a
shipped example, because it exposes a limitation of the process front
end that must be lifted before it can be a real one.

## What works

The correct protocol -- a client write replicated down a two-node chain,
committed when the tail stores it, with a reconfiguration that promotes a
spare and inherits the committed value -- checks clean (101 states) for
agreement and that a committed write is held by the chain.

## The limitation it exposes

The intended planted bug is the classic chain-replication mistake: the
head acknowledges the client *before* the write reaches the tail.  That
loses an acknowledged write only if the head crashes in the window
between the client ack and the replication to the tail.

The checker schedules a crash (and every environment action) only at a
**quiescent point** -- a node blocked in a syscall.  The buggy head runs
"ack the client, then replicate to the tail" as one uninterrupted burst
between two `accept()` calls, so a crash cannot be scheduled inside that
window, and the bug is not caught.  Shipping it would be a worthless
example: the correct build passes and the buggy build also passes.

Catching this bug needs the checker to interrupt a node *between*
syscalls -- finer-grained than the run-to-the-next-blocking-syscall
model the engine is built on.  That is a real future capability (it is
adjacent to the single-step/ptrace machinery proven in
`experiments/fork-snapshot/`): a node could be stopped and crashed at an
instruction boundary, not only at a blocking call.

## Why not distort the protocol to fit

One could force a blocking operation into the ack/replicate window so a
crash could interpose, but that would no longer be chain replication --
it would be a toy shaped to the tool.  The project's rule is to ship
real protocols or none, so this waits for the engine capability.

Humming Consensus and ZAB both have lost-commit bugs of this shape, so
both wait on the same capability.  Paxos (agreement) and Raft (election
safety) do not: their bugs surface through message ordering and
crash-at-quiescence, which the engine already explores, and both ship as
examples.
