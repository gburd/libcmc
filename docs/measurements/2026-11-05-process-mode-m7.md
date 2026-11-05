---
title: "Process mode M7: honest verdicts, and what Paxos costs"
parent: Background
nav_order: 13
permalink: /background/process-mode-m7/
lede: >-
  Before measuring scale, M7 tested whether the checker's verdicts could be trusted. Five could not. Fixed, it then checks single-decree Paxos with five real processes exhaustively.
---

Date: 2026-11-05.  Prototype: `experiments/process-mode/`.  Raw output:
`2026-11-05-process-mode-m7-raw.txt` (same directory).  Follows M6;
roadmap in `experiments/process-mode/DESIGN.md`.

## Why

M7 was planned as a scale milestone: measure what a real consensus
protocol costs and decide, per example, between exhaustive, bounded and
simulated checking.  A measurement is only worth something if the
verdicts are right, so M7 first wrote a probe for each way the M6 engine
might be wrong, and ran each probe against the M6 engine before changing
anything (the project's rule: every fix starts as a failing test).

## Five wrong verdicts

| probe | the right verdict | M6 engine | cause |
|---|---|---|---|
| `idle`: a server ignores an idle timeout, then serves a request late | violation | pass | the fingerprint left out the budgets and the logical clock, so the world after the timeout merged into the initial one |
| `durlog-bug --depth 0` | incomplete | pass (exit 0) | a depth-bounded search printed `ok` |
| `fifo`: two writes on one TCP connection | pass | violation | a connection's messages could be delivered out of order, which TCP never does |
| `chain`: node 0 -> node 1 -> node 2 | violation | pass | every accepted connection was paired with node 0, so node 1's forward was never delivered |
| `bigmsg`: one 65-byte write | engine error | pass | writes over 8 bytes were silently truncated |

Two of these are false passes on real violations, the worst outcome for a
model checker.  M3's `mc.c` had the depth-bound flaw too.

## What changed

- **The fingerprint holds everything the future depends on**: each
  node's state, liveness and what it is blocked in; its open connections
  and where they go; every channel's messages in order; connections
  waiting to be accepted; the reply log; the budgets left; the logical
  clock.
- **Topology is whatever the nodes dial.**  `connect` queues the
  connection at the dialled node's listener; `accept` is deferred, and
  which waiting connection it takes is a search action.
- **Each connection is a FIFO channel**, as TCP is: only its oldest
  message can be delivered; a short read leaves the rest queued; a recv
  from a dead peer can return EOF.
- **A bound is not a pass.**  A search stopped by a depth, state or time
  limit exits 2, `INCOMPLETE (no violation found)`.  Seeded `--simulate`
  random walks also exit 2 when they find nothing.
- **Limits are errors.**  A full table, an over-long write, a node silent
  in a call the checker does not model, or a replay that diverges stops
  the run with exit 1 and says which.  The seen set and the frontier
  grow instead of dropping states.
- Counterexamples print as named actions (`accept n0->n2`, `deliver
  n2->n0`, `crash n1`), and exit codes match `cmc(1)`.

The fix also exposed a flaw in M6's own demo: `durlog-bug` violated its
invariant without any crash, because its volatile state never held the
value.  The demo now keeps the value in memory, so the bug needs a crash
to show, as M6 claimed.  The M6 verdicts are unchanged.

## Paxos, measured

`demo/paxos.c` is single-decree Paxos: two proposers and three
acceptors, each its own process, ordinary blocking TCP with one
connection per request, acceptors persisting before they reply.  The
`-DBUG` proposer ignores the values in the promises, the classic mistake
that lets two proposers decide different values.  The invariant is
agreement.  It is the M7 sizing workload, not yet the Paxos example.

Host: Linux 6.18.60, x86_64, 48 cores, load 5-8; single-threaded.

| environment | correct (exhaustive BFS) | bug |
|---|---|---|
| no faults | 561 states, depth 27, 964 replays, 2.9 s | caught, 24 actions, 2.7 s |
| 1 crash | 7,503 states, depth 29, 14,595 replays, 44.8 s | caught, 40.6 s |
| 1 drop | 1,525 states, depth 27, 3,056 replays, 9.1 s | caught, 8.4 s |
| 1 crash + 1 drop | 30,075 states, depth 32, 68,947 replays, 226.9 s | caught, 192.6 s |

With one crash and one drop, seeded `--simulate` found the bug on all
five seeds tried, in 15 to 350 walks, under one second each.  2,000 walks
of the correct build took 5.0 s and exited 2.

## What the numbers say

Process mode runs about 300-340 replays per second.  Every state costs a
fresh launch of all five processes plus a replay of its prefix.  That is
133 distinct states per second on the crash+drop row, against 237,568 per
second for the in-process engine on HC (7,412,120 states in 31.2 s), about
1,800 times slower, on different models.

So exhaustive checking holds for Paxos with one fault of each kind.
Larger budgets, or a protocol the size of Raft, will need `--simulate` or
a faster transition: forking from a snapshot instead of replaying from
launch.  Either way, the verdict says which it was.

**Verdict: M7 passes.**  The verdicts are now honest, and five-process
Paxos is checked exhaustively, with its bug found both by BFS and by
simulation.  Gate stable 10/10.

## Still out

M8 lifts process mode out of `experiments/` into `src/` as the one front
end, then the Paxos, Raft, Humming Consensus and ZAB examples.  Known
shortcuts, marked in the code: a connect never fails; one write is
delivered per read (real TCP may coalesce); `close` does not notify the
peer; absolute fd numbers are left out of the fingerprint.
