---
title: Install and build
parent: Guide
nav_order: 1
permalink: /guide/01-install/
lede: >-
  A C11 compiler, make and libc.  Nothing else.
---

## Get the source

```sh
git clone https://codeberg.org/gregburd/libcmc.git
cd libcmc
```

The GitHub mirror, `https://github.com/gburd/libcmc`, has the same
history.

## Build and test

```sh
make            # build/libcmc.a, build/cmc, build/test_cmc
make check      # about a minute
```

`make check` runs the engine tests, the toy core's planted-bug gate,
the purity gate and its own teeth, the documentation gate, and the
calibration rows against TLC that finish in about 30 seconds.  It
should end with:

```
check-calibration: every row matches TLC
```

gcc and clang must both build the tree with `-Wall -Wextra -Werror`:

```sh
make CC=clang check
```

The longer gates:

| target | what | time |
|---|---|---|
| `make check-msan` | the engine tests under clang's MemorySanitizer | 10 s |
| `make check-calibration` | all 22 calibration rows against TLC; about 7 GB of RAM | 8 min |
| `make check-hc-bugs` | planted bugs in the calibration core must break calibration | 30 s |
| `make check-all` | all of the above | 10 min |

The times are from a 48-core Xeon; every gate is single-threaded.

Build output goes to `build/`, and logs and replay files to
`$CMC_SCRATCH` (default `~/tmp/cmc-build`).

## Install

```sh
make install PREFIX=/usr/local
```

This installs `lib/libcmc.a`, `include/cmc.h`, and the man pages
`cmc(1)`, `libcmc(3)` and `cmc(7)`.  `DESTDIR` works as usual.

## With Nix

The repository is a flake:

```sh
nix build           # result/lib/libcmc.a, result/include/cmc.h, man pages
nix flake check     # builds, then runs make check (gcc and clang)
nix develop         # gcc, clang, mandoc, ruby for the docs site
```

## Try it

The `cmc` binary built here carries the toy lock server, three copies
of it with planted bugs, and the calibration core.

```sh
build/cmc --model toy --all-props
build/cmc --model toy-bug1 --inv Mutex
```

The first passes, with 34 distinct states.  The second prints the six
events after which two clients hold the lock at once.  The
[next page]({{ '/guide/02-first-model/' | relative_url }}) builds a model
from nothing.
