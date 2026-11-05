---
title: Using libcmc in your project
parent: Guide
nav_order: 3
permalink: /guide/03-in-your-project/
lede: >-
  Point a model file at the program you build, add the state hook, plant your bugs, and gate it in CI.
---

1. TOC
{:toc}

---

## Get libcmc

Any of these works; pick one and pin a commit.

- An installed copy: `make install PREFIX=...` puts `cmc` support
  (`lib/libcmc.a`, `lib/libcmc/cmc-preload.so`), the header and the man
  pages under `PREFIX`.
- A git submodule, built by your build:
  ```sh
  git submodule add https://codeberg.org/gregburd/libcmc.git third_party/libcmc
  make -C third_party/libcmc
  ```
  then run `third_party/libcmc/build/cmc`.
- Nix: the flake's default package.

`cmc --model-file` needs Linux on x86-64.

## What your program must do

libcmc runs your program unmodified, one process per node, and is the
network between them.  It asks four things of the program:

1. **Take the node number** as its first argument, and **listen on
   127.0.0.1 port 20000 + that number**; a connection goes to the node
   whose port was dialled.  This is the one convention.
2. **Do its I/O through the calls libcmc models**: `socket`, `bind`,
   `listen`, `connect`, `accept`, `read`, `write`, `close` on its
   connections, and the clock.  A node that blocks in anything else for
   ten seconds is an error, not a guess.
3. **Report its state** on descriptor 3 after each change, and what it
   has made durable on descriptor 193: a length byte, then a canonical
   and complete encoding.  The state hook in the
   [guide's server]({{ '/guide/02-first-model/' | relative_url }}) is
   eight lines.  This hook is the one place you can make the
   checker miss a bug, by leaving out something the node's future
   depends on; treat it as carefully as the durable/volatile split in
   the protocol itself.
4. **Build with `-no-pie`**: the checker gives the program a heap at
   fixed addresses, so a run is a pure function of the actions it chose.

The client is built in: with `client_write = V`, a client connects to
node 0 and sends the two bytes `W V`.  Any reply on that connection that
starts with `A` is logged in `replies` with its second byte as `.val`.

## Write the model file

```
[target]
binary = ../build/replog      # relative to this file
nodes  = 3

[environment]
client_write = 5
crashes = 1
drops = 1
timeouts = 1

[invariants]
no_lost_ack = "all r in replies : r.val in node[0] or r.val in node[1] or r.val in node[2]"
```

[The model file]({{ '/reference/model-file/' | relative_url }}) lists
every key.  The loader rejects any key it does not implement, so a typo
is an error rather than a smaller search.

## Plant bugs

Put each bug you want the checker to catch behind an `#ifdef` in the
shipped source:

```c
#ifdef REPLOG_BUG_EARLY_ACK
	ack_client(c, v);            /* before the quorum is durable */
#endif
```

Build one binary per bug, and require each to fail with the invariant
you expect, by search and by simulation:

```sh
cmc --model-file replog.model --binary build/replog-bug-early-ack
```

A planted bug the checker misses is a hole, in the budgets, the
invariants, the state hook or the engine.  Treat it as a failure.  Check
also that the states the invariant is about are reached: an invariant
over `replies` passes trivially if no acknowledgement is ever sent.

## Choose budgets

A pass covers only the budgets you gave.  Start small and raise them
until every planted bug is caught.

| setting | what it bounds |
|---|---|
| `crashes` | node crashes; a crash keeps only what the node reported durable |
| `drops` | messages lost |
| `timeouts` | reads satisfied by a timeout instead of a message; each advances the clock |
| `--depth D`, `--max-states N`, `--time S` | stop early: the result is "no violation found", exit 2, never a pass |

Each state costs a fresh run of every node, so process mode is far
slower per state than an in-process model: single-decree Paxos with five
processes and one crash and one drop is 30,075 states in about four
minutes ([measurement]({{ '/background/process-mode-m7/' | relative_url }})).
When the full search is too big, run `--simulate` with many walks.

## In CI

```yaml
- run: make -C third_party/libcmc
- run: cc -no-pie -o build/replog src/*.c
- run: third_party/libcmc/build/cmc --model-file test/replog.model
- run: |
    for b in build/replog-bug-*; do
      if third_party/libcmc/build/cmc --model-file test/replog.model --binary "$b"; then
        echo "planted bug $b was not caught"; exit 1
      fi
    done
```

The exit status is 0 for a pass, 1 for a violation, 2 for an incomplete
search and 3 for a usage or model-file error, so a step fails on
anything but a full pass.  The bug loop above treats exit 2 and 3 as
not caught, which is what you want.

## When a check fails

The counterexample is the shortest sequence of actions, by name:
`accept`, `deliver`, `eof`, `timeout`, `crash`, `drop`, each with its
nodes.  Run the program by hand in that order, or add logging to it:
the checker runs exactly your binary, so anything you add shows up on
the next run.

## In-process models

libcmc also checks protocol logic written as pure C step functions
linked in-process ([libcmc(3)]({{ '/reference/man/libcmc.3/' | relative_url }})).
That is how the engine is calibrated against TLC, and it is far faster
per state, but it checks a rewrite of your protocol rather than the
program you ship.
