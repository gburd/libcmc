---
title: From a TLA+ spec
parent: Guide
nav_order: 4
permalink: /guide/04-from-tla/
lede: >-
  Lamport's TCommit from tlaplus/Examples, checked by TLC and by libcmc with the same counts, then a C implementation checked against it.
---

1. TOC
{:toc}

---

This page is for someone who already writes TLA+.  It takes an
unmodified spec from [tlaplus/Examples](https://github.com/tlaplus/Examples),
Lamport's transaction commit spec
[`TCommit.tla`](https://github.com/tlaplus/Examples/tree/master/specifications/transaction_commit),
and walks it through every libcmc tool that speaks TLA+:

1. the spec and what TLC says about it;
2. the same spec in libcmc's `.cml` language, with the same counts;
3. the round trip back to TLA+, and a planted bug's trace as an ITF file;
4. a small C program of four processes, checked to refine the spec, and
   its planted bug shown in the spec's terms;
5. the invariant claimed inductive, checked, and emitted as a TLAPS
   theorem.

Every file the page uses is under `docs/_includes/snippets/`, and
`make check` runs each `cmc` command shown and checks its output.  The
TLC numbers come from TLC 2.19 with `-workers 1`; `make
check-spec-calibration` checks that cmc on `tcommit.cml` matches TLC on
the upstream `TCommit.tla`, and `make check-emit-tla` that TLC on the
emitted module matches cmc.  The C program needs Linux on x86-64.

## The upstream spec

`TCommit.tla` is copied unmodified, with its license
(`TCommit.LICENSE`, MIT, from tlaplus/Examples).  A set `RM` of
resource managers each go from working to prepared, and then all
commit or all abort:

```tla
CONSTANT RM       \* The set of participating resource managers
VARIABLE rmState  \* `rmState[rm]' is the state of resource manager rm.

TCInit ==   rmState = [rm \in RM |-> "working"]
canCommit == \A rm \in RM : rmState[rm] \in {"prepared", "committed"}
notCommitted == \A rm \in RM : rmState[rm] # "committed"

Prepare(rm) == /\ rmState[rm] = "working"
               /\ rmState' = [rmState EXCEPT ![rm] = "prepared"]

Decide(rm)  == \/ /\ rmState[rm] = "prepared"
                  /\ canCommit
                  /\ rmState' = [rmState EXCEPT ![rm] = "committed"]
               \/ /\ rmState[rm] \in {"working", "prepared"}
                  /\ notCommitted
                  /\ rmState' = [rmState EXCEPT ![rm] = "aborted"]

TCNext == \E rm \in RM : Prepare(rm) \/ Decide(rm)

TCConsistent ==
  \A rm1, rm2 \in RM : ~ /\ rmState[rm1] = "aborted"
                         /\ rmState[rm2] = "committed"
```

(the comments and `TCTypeOK` are left out here).  Its `TCommit.cfg`
uses three resource managers as model values and turns the deadlock
check off, since every run ends with each resource manager decided:

```
CONSTANT RM = {r1, r2, r3}
INVARIANTS TCTypeOK TCConsistent
SPECIFICATION TCSpec
CHECK_DEADLOCK FALSE
```

TLC finds:

```sh
java -cp tla2tools.jar tlc2.TLC -workers 1 -config TCommit.cfg TCommit.tla
```

```
Model checking completed. No error has been found.
94 states generated, 34 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 7.
```

## Reading the module directly

`cmc --spec` reads a TLA+ module and its `.cfg` when the module stays
inside the subset `.cml` can express.  TCommit does not: its states are
strings, and libcmc's values are integers, functions, records and
sequences.  The reader names the first construct outside the subset and
stops, exit 3:

```sh
build/cmc --spec TCommit.tla --config TCommit.cfg
```

```
cmc: TCommit.tla:9: a string literal: strings are not supported
```

So the next step writes the same spec in `.cml` by hand, with the four
strings as the integers 0 to 3.  A spec that uses only integers,
functions, records and sequences is read as it stands, and
`cmc --emit-cml FILE.tla --config FILE.cfg` prints the `.cml` it
becomes.

## The spec in .cml

{% include snippet.html file="tcommit.cml" region="spec" %}

Each construct maps one to one:

| TLA+ | .cml |
|---|---|
| `CONSTANT RM` with `RM = {r1, r2, r3}` in the `.cfg` | `const N = 3` and `def RM = 0 .. N-1`: a model value becomes an integer |
| `"working"`, `"prepared"`, `"committed"`, `"aborted"` | 0, 1, 2 and 3 |
| `VARIABLE rmState` with `TCTypeOK` | `var rmState : [0 .. N-1 -> 0 .. 3]`: the declared type is the type invariant, checked in every state |
| `TCInit`, `rmState = [rm \in RM \|-> "working"]` | `init rmState == [rm in RM -> 0]` |
| `canCommit ==`, `notCommitted ==` | `def canCommit =`, `def notCommitted =` |
| `\A`, `\in`, `#`, `~`, `/\`, `\/` | `all`, `in`, `!=`, `not`, `and`, `or` |
| `x \in {"prepared", "committed"}` | `x == 1 or x == 2` |
| `Prepare(rm)` under `\E rm \in RM` in `TCNext` | `action Prepare(rm in RM):`: the parameter is the existential |
| `Decide(rm)`, a disjunction of two cases | two actions, `DecideCommit` and `DecideAbort` |
| the unprimed conjuncts of an action | its `when` line, the guard |
| `rmState' = [rmState EXCEPT ![rm] = ...]` | `rmState' = [rmState except ![rm] = ...]` |
| `CHECK_DEADLOCK FALSE` | `deadlock = off` |
| `INVARIANT TCConsistent` | `invariant TCConsistent: ...` |

Splitting `Decide` into two actions changes no state or step: an
action's successors are the union of its disjuncts' either way.  It does
change how a trace names a step, `DecideCommit` rather than `Decide`.

```sh
build/cmc --spec tcommit.cml
```

```
cmc: spec tcommit, 1 variable, 4 actions, breadth-first
Model checking completed. No error has been found.
94 states generated, 34 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 7.
  1 invariant, 0 properties held.
```

The same 34 distinct states and depth 7 as TLC, and the same 94
generated.  libcmc's search is TLC's: breadth first, depth counting the
initial state as 1, every successor of every state generated.

## Back to TLA+, and a trace as a file

`--emit-tla` writes the `.cml` back as a TLA+ module and `--emit-cfg`
its TLC configuration, so TLC, Apalache and TLAPS can check what libcmc
checked:

```sh
build/cmc --emit-tla tcommit.cml > tcommit.tla
build/cmc --emit-cfg tcommit.cml > tcommit.cfg
java -cp tla2tools.jar tlc2.TLC -workers 1 -config tcommit.cfg tcommit.tla
```

TLC finds the same 94, 34 and 7 on the emitted module, and so does
`cmc --spec tcommit.tla --config tcommit.cfg`, which reads it back.
The module carries Apalache type annotations and a `CInit` for the
constants, so `apalache-mc check --cinit=CInit --inv=TCConsistent
--no-deadlock tcommit.tla` takes it as written (`--no-deadlock` because
the model turns the deadlock check off, as the module's header says).

Now plant a bug: a resource manager that aborts without checking that
no one has committed.  It sits in the same file behind a tag:

{% include snippet.html file="tcommit.cml" region="bug" %}

```sh
build/cmc --spec tcommit.cml --define BUG --trace-out bug.itf.json
```

```
Error: Invariant TCConsistent is violated.
Error: The behavior up to this point is (5 actions):
    1. Prepare
    2. Prepare
    3. Prepare
    4. DecideCommit
    5. DecideAbortBug
  state: rmState=[0:2,1:3,2:1]
```

All three prepare, the first commits, the second aborts.  TLC on the
module emitted with `--define BUG` prints the same six states.
`bug.itf.json` holds the trace in the Informal Trace Format, laid out as
Apalache would write it for the emitted module; its last state is

```
{"#meta":{"index":5,"action":"DecideAbortBug"},"N":{"#bigint":"3"},"rmState":{"#map":[[{"#bigint":"0"},{"#bigint":"2"}],[{"#bigint":"1"},{"#bigint":"3"}],[{"#bigint":"2"},{"#bigint":"1"}]]}}
```

so `itf-py`, `itf-rs` or any ITF viewer reads it.
`--trace-format json` writes a plainer array instead.

## A C implementation

TCommit says what may happen, not how.  `tcommit.c` is one way: node 0
is a transaction manager that asks each resource manager in turn to
prepare and then tells each the outcome.  It is plain C over blocking
TCP, with no libcmc header, as on
[the first-model page]({{ '/guide/02-first-model/' | relative_url }}).
A resource manager:

{% include snippet.html file="tcommit.c" region="rm" %}

A timeout (the `T` byte libcmc delivers when it fires one) makes a
resource manager that is still working abort on its own, which the spec
allows.  The transaction manager:

{% include snippet.html file="tcommit.c" region="tm" %}

Each node reports its state after each change through the state hook,
a length byte and the state on descriptor 3:

{% include snippet.html file="tcommit.c" region="hook" %}

The model file names the program, runs four nodes and allows one
timeout.  Its own property asks that the manager decides, so a run that
never gets that far fails instead of passing vacuously:

{% include snippet.html file="tcommit.model" %}

The binding says what `rmState` is in terms of what the program
reports: resource manager `rm` of the spec is node `rm + 1`, and its
state is byte 0 of that node's record.

{% include snippet.html file="tcommit.bind" %}

`--refine` checks the spec's invariant on the abstract state the
binding builds at every state the program reaches, and each value
against its declared type.  `--refine-steps` also requires every step
of the program to be a step of the spec or a stutter:

```sh
cc -no-pie -o tcommit docs/_includes/snippets/tcommit.c
build/cmc --model-file tcommit.model --refine tcommit.cml --bind tcommit.bind
build/cmc --model-file tcommit.model --refine tcommit.cml --bind tcommit.bind --refine-steps
```

```
cmc: model file tcommit.model, binary tcommit, 4 nodes, breadth-first search
  environment: client_write -1, timeouts 1, crashes 0, drops 0, restarts 0
Model checking completed. No error has been found.
827 states generated, 391 distinct states found, 0 states left on queue.
The depth of the complete state graph search is 20.
```

Both pass: 391 states of the program, every one a state of TCommit and
every step one of its steps.  The program has many more states than the
spec because they hold the connections, the messages in flight and
where each process is in its code.

## A bug in the implementation

Built with `-DTC_BUG`, the manager keeps only the last vote it read
(`commit = yes` instead of `commit = commit && yes`), so a resource
manager that voted no early is forgotten.  Every resource manager
votes yes unless it has already timed out and aborted, so without a
timeout the bug cannot show:

```sh
cc -no-pie -DTC_BUG -o tcommit-bug docs/_includes/snippets/tcommit.c
build/cmc --model-file tcommit.model --refine tcommit.cml --bind tcommit.bind --binary ./tcommit-bug --timeouts 0
```

```
Model checking completed. No error has been found.
```

With the model's one timeout, libcmc finds it, and each `spec:` line
under a step gives `rmState` as the binding reads it there:

```sh
build/cmc --model-file tcommit.model --refine tcommit.cml --bind tcommit.bind --binary ./tcommit-bug
```

```
Error: the implementation does not refine the spec: abstract invariant TCConsistent is violated.
Error: The behavior up to this point is (11 actions):
       step n3
       step n2
       step n1
         spec: rmState=[0:0,1:0,2:0]
    1. accept n0->n1
         spec: rmState=[0:0,1:0,2:0]
    2. deliver n0->n1
         spec: rmState=[0:1,1:0,2:0]
    3. deliver n1->n0
         spec: rmState=[0:1,1:0,2:0]
    4. accept n0->n2
         spec: rmState=[0:1,1:0,2:0]
    5. timeout n2
         spec: rmState=[0:1,1:3,2:0]
    6. deliver n0->n2
         spec: rmState=[0:1,1:3,2:0]
    7. deliver n2->n0
         spec: rmState=[0:1,1:3,2:0]
    8. accept n0->n3
         spec: rmState=[0:1,1:3,2:0]
    9. deliver n0->n3
         spec: rmState=[0:1,1:3,2:1]
    10. deliver n3->n0
         spec: rmState=[0:1,1:3,2:1]
    11. deliver n0->n1
         spec: rmState=[0:2,1:3,2:1]
  node states: node[0]=1 node[1]=2 node[2]=3 node[3]=1
```

Read in the spec's terms: resource manager 0 prepares (step 2); 1
times out while still working and aborts (step 5), then votes no; 2
prepares and votes yes (step 9), and that last yes is the only vote
the manager remembers.  It decides commit, and when 0 receives the
decision (step 11) one resource manager has committed and another
aborted.  The exit status is 1.

With `--refine-steps` the same build fails sooner, at the first step
TCommit forbids, and each `spec:` line also names the spec action the
step refined:

```sh
build/cmc --model-file tcommit.model --refine tcommit.cml --bind tcommit.bind --binary ./tcommit-bug --refine-steps
```

```
Error: the implementation does not refine the spec: a step has no matching abstract action.
Error: The behavior up to this point is (8 actions):
       step n3
       step n2
       step n1
         spec: init -> rmState=[0:0,1:0,2:0]
    1. accept n0->n1
         spec: stutter -> rmState=[0:0,1:0,2:0]
    2. deliver n0->n1
         spec: Prepare(rm=0) -> rmState=[0:1,1:0,2:0]
    3. deliver n1->n0
         spec: stutter -> rmState=[0:1,1:0,2:0]
    4. timeout n0
         spec: stutter -> rmState=[0:1,1:0,2:0]
    5. accept n0->n3
         spec: stutter -> rmState=[0:1,1:0,2:0]
    6. deliver n0->n3
         spec: Prepare(rm=2) -> rmState=[0:1,1:0,2:1]
    7. deliver n3->n0
         spec: stutter -> rmState=[0:1,1:0,2:1]
    8. deliver n0->n1
         spec: no matching abstract action -> rmState=[0:2,1:0,2:1]
```

Here the timeout fires at the manager itself while it waits for
resource manager 1's vote (step 4), which it takes as a no and then
forgets.  Resource manager 0 commits (step 8) while 1 is still
working, a step no TCommit action allows, since `canCommit` rules it
out.  The state check needs the commit and an abort to coexist; the
step check needs only the commit.  A step that changes no `rmState`
(a connection, a vote in flight) is a `stutter`.

## The proof hook

TLC and libcmc check `TCConsistent` in the states they reach.  A proof
needs more: that it is inductive, so every step from any state of the
right type that satisfies it keeps it.  The `.cml` claims that with
`[inductive]` on the invariant, and `--check-inductive` checks the claim
over every state of the declared types, reachable or not:

```sh
build/cmc --spec tcommit.cml --check-inductive
```

```
cmc: spec tcommit, checking that TypeOK /\ TCConsistent is inductive
The invariant is inductive: Init satisfies it, and every step from each of the 46 states
of TypeOK that satisfy it (of 64 states in TypeOK) keeps it.
```

`--emit-tla` then writes the obligation as a TLAPS theorem with a proof
outline, after `TypeOK` and `IndInv`:

```tla
THEOREM Inductive == Spec => []IndInv
<1>1. Init => IndInv
  BY ConstOK DEF Init, IndInv, TypeOK, TCConsistent, RM, canCommit, notCommitted
<1>2. IndInv /\ [Next]_vars => IndInv'
  BY ConstOK DEF Next, vars, Prepare, DecideCommit, DecideAbort, IndInv, TypeOK, TCConsistent, RM, canCommit, notCommitted
<1>3. QED
  BY <1>1, <1>2, PTL DEF Spec

THEOREM Spec => []TCConsistent
  BY Inductive, PTL DEF IndInv
```

`tlapm tcommit.tla` takes it from there; whether the two leaves go
through depends on its backends.  Apalache checks the same claim with
`apalache-mc check --cinit=CInit --init=IndInv --inv=IndInv --length=1
tcommit.tla`.  The check and the proof are of the spec; the program is
tied to it by the refinement check above, which is model checking, not
proof.

## What carries over, and what does not

- TLC's search and counts carry over exactly: the same states, depth
  and shortest trace, which `make check-spec-calibration` checks on
  every model with a TLA+ twin.
- Strings, `CHOOSE`, `SUBSET`, `UNION`, `\X` and sets of records do
  not; the reader refuses them with the line, and the spec is
  rewritten with integers, as here.  The language is in
  [the abstract model reference]({{ '/reference/abstract-model/' | relative_url }}).
- Model values become integers.  A `SYMMETRY` set over them becomes
  `symmetry 0 .. N-1`.
- The C program is checked against the spec, within the model's
  budgets: a pass means no violation with one timeout and no crashes.
  Raise the budgets until the planted bugs are caught.
