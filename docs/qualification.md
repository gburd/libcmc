---
title: "Qualification"
parent: Reference
nav_order: 8
permalink: /reference/qualification/
lede: >-
  What each engine answers exactly, what it approximates, and what it
  refuses, with the gate that checks each claim.
---

# Qualification

This page lists, for each of libcmc's three engines, every construct it
handles and how: **exact** (it answers as Linux or TLA+ would, and a
gate checks that), **approximate** (it answers a stated subset or
abstraction; the limit says which), or **refused** (the check ends with
exit 3 or INCOMPLETE rather than guess).  A construct not listed is not
qualified.

A row cites the gate that pins it.  `proc_gate` is
`test/proc_gate.sh`, `spec_gate` is `test/spec_gate.sh`, `emit_gate`
and `apalache_gate` are the TLA+ bridge gates, and `test_cmc` is the
in-process engine's unit suite.  The status as of the soundness
remediation is in [the plan]({{ '/reference/plan-soundness/' | relative_url }}).

**A pass is evidence for the instance checked, not a proof.**  Each
engine searches a bounded instance: the nodes, budgets and depth the
model gives.  None of this page makes a pass mean more than that.

## Process engine (`cmc --model-file`)

The engine runs each node as a real process under seccomp user
notification and answers the system calls below.  The file system is
read-only to a node (see below).
`pread`, `pwrite`, `sendmmsg`, `recvmmsg`, `ioctl`,
`splice` and `sendfile` are refused on a checker-owned fd (exit 3), and
run in the kernel on any other.  Every other system call runs
in the kernel, untouched.

### Sockets and connections

| construct | status | limit | gate |
|---|---|---|---|
| `socket`, `bind`, `listen` | exact enough | `AF_INET`, `SOCK_STREAM` only (anything else is refused); node i listens on 127.0.0.1:20000+i | proc_gate verdicts, "a UDP socket is refused", syscorner ipv6 |
| `setsockopt`, `getsockopt` | approximate | `SO_KEEPALIVE`, `SO_REUSEADDR`, `SO_REUSEPORT` and `TCP_NODELAY` are recorded, read back and inherited by an accepted socket, and change nothing (no keepalive timer, no Nagle delay, no port reuse); `SO_RCVBUF` and `SO_SNDBUF` may be set and change nothing, and reading them is refused; `SO_ERROR` reads and clears the pending error (`ECONNREFUSED`, `ECONNRESET`, `EPIPE`); any other option is exit 3 | proc_gate nbconnect |
| `getsockname`, `getpeername` | approximate | 127.0.0.1 everywhere; a listener's port is 20000+i, a dialler's own port is 40000 + 256 i + (fd - 500), not an ephemeral port; `ENOTCONN` with no peer; `getsockname` of a socket not connected is exit 3 | proc_gate nbconnect |
| a socket call on the wrong kind of fd (an eventfd, an epoll instance, a socket in the wrong state, a closed fd) | exact | Linux's errno: `ENOTSOCK`, `EINVAL`, `EISCONN`, `EBADF`; a read of a socket never dialled is `ENOTCONN`, a write `EPIPE` with its `SIGPIPE` | proc_gate syscorner wrongfd, notconn, notconnpipe, against a native run |
| `fcntl` `O_NONBLOCK` and `FD_CLOEXEC` | exact | each fd keeps its own flags (a connection, the listener, an eventfd, an epoll instance, a socket not yet dialled); other `fcntl` commands on an owned fd are exit 1 | proc_gate syscorner eventfd, nbepoll, selfdial, fcntldup |
| a non-blocking `connect` | exact, single-threaded | `EINPROGRESS` always; the handshake completes when the search says (CONNECT), into the backlog of a live peer or refused by one that is down, and any call on the socket before then is a choice between both outcomes; while it is out, reads and writes are `EAGAIN`, `connect` is `EALREADY`, `SO_ERROR` 0, `poll` not ready; once refused, the first of `SO_ERROR`, a read, a write or `connect` reports `ECONNREFUSED`, then a read is 0, a write `EPIPE`, `connect` `ECONNABORTED`; `POLLOUT` with `POLLERR`, `POLLHUP` as Linux reports them; a blocking call on a socket still handshaking, or a non-blocking connect in a node with more than one thread, is exit 3 | proc_gate nbconnect ok, early, refused, against a native run |
| `connect` to a live node | approximate | always succeeds and waits at the listener, even before the peer calls `listen()` | proc_gate verdicts |
| `connect` to a node that is down | exact | `ECONNREFUSED`; later reads `ENOTCONN`, writes `EPIPE`; the socket may be dialled again | proc_gate "a dead node refuses the connection", tcpsem redial |
| `accept`, `accept4` | exact | which waiting dialler is a search choice; per dialler, oldest first; the sockaddr out-parameter is not filled; `accept4` flags other than `SOCK_CLOEXEC` are refused | proc_gate verdicts, "accept4 SOCK_NONBLOCK is refused" |
| non-blocking `accept` with nothing waiting | exact | `EAGAIN` | proc_gate pollloop, epollloop |
| one queue per connection direction | exact | FIFO; two connections between one node pair never share bytes | proc_gate crossrpc, fifo |
| `read`, `recvfrom` | approximate | one read takes at most one write's bytes; a short read leaves the rest queued; any `recvfrom` flag or source address is refused | proc_gate verdicts, "MSG_PEEK is refused" (the short read: code only, no fixture yet) |
| `write`, `sendto` | approximate | at most `[target] max_write` bytes (default 64, cap 4096) per call; a longer write is exit 1; `sendto` flags other than `MSG_NOSIGNAL`, or a destination address, are refused | proc_gate bigmsg |
| `readv`, `writev`, `sendmsg`, `recvmsg` | as `read` and `write` | the iovec array is gathered or scattered whole, its total within `[target] max_write`; up to 16 entries for a read; `recvmsg` sets `msg_namelen` and `msg_flags` to 0; a destination address, ancillary data, a `recvmsg` flag or a `sendmsg` flag other than `MSG_NOSIGNAL` is exit 3; a count past 1024 is `EINVAL` | proc_gate sockapi iov, "sendmsg with ancillary data is refused" |
| `shutdown` | exact | `SHUT_WR` sends a FIN and a later write fails `EPIPE`; `SHUT_RD` makes an empty read return 0 at once while bytes queued or arriving are still read; once both are shut, bytes arriving are answered with an RST; `ENOTCONN` on a socket not connected, refused, reset, or past both FINs; a listener's or a timer connection's shutdown is exit 3 | proc_gate sockapi shut |
| `dup`, `dup2`, `dup3` of a connection | exact | the new number shares the connection, its queue, `O_NONBLOCK` and options; `FD_CLOEXEC` is its own; the socket stays open until its last number closes; a dup of any other owned fd, or onto a fd that is open or below 500, is exit 3 | proc_gate sockapi dup, edges dup2, dup3 |
| a full send buffer (`short_writes`) | approximate | a non-blocking send with bytes of its connection still in flight may get `EAGAIN` or send 1, half, or all but one of its bytes; then sends get `EAGAIN` and `POLLOUT` is not reported until the peer reads; any buffer size can fill, and only those counts are offered; a blocking send is never cut short | proc_gate shortw |
| a network partition (`partitions`) | approximate | a budgeted CUT of a directed link holds back its bytes, FINs and handshakes, in order, until a HEAL; asymmetric cuts included; a blocking connect across a cut link waits for a HEAL or fails with `ETIMEDOUT` at a 127 s deadline; an RST across a cut link is exit 3; the client is never cut off | proc_gate partitions, cutconn |
| `close` | exact | FIN, or RST when bytes were left unread; bytes already sent stay readable | proc_gate rstwrite, rstignore, tcpsem rstdata |
| a reset connection | exact | the reset reported once (`ECONNRESET`, or `EPIPE` after a FIN), then reads 0 and writes `EPIPE` | proc_gate rstwrite, rstignore, reset, tcpsem finwrite |
| an exited listener's backlog | exact | each waiting dial is reset | proc_gate tcpsem backlog |
| `SIGPIPE` on a write into a reset connection | exact for ignored and default | default kills the node; ignored leaves `EPIPE` | proc_gate rstwrite, rstignore |
| `SIGPIPE` with a handler, or blocked | refused | exit 3 | proc_gate "a SIGPIPE handler is refused" |
| a crashed peer | approximate | no FIN; a reader may notice (EOF) once drained, standing for a timeout | proc_gate durlog, restart |
| a buffer the node does not own | exact | `EFAULT` for fixed-size arguments, and for `poll`, `epoll_wait`, the clock and `getrandom` into read-only memory; a partial transfer is exit 3 | proc_gate "a bad write buffer gets EFAULT", syscorner badbuf, robuf, edges partwrite through partpoll |

### Event loops

| construct | status | limit | gate |
|---|---|---|---|
| `poll`, `ppoll` with no or zero timeout | exact | readiness of a channel or listener is a search choice (WAKE), never automatic; `POLLOUT` is ready on a connection and an eventfd, never on the listener or an epoll instance, and with `POLLHUP` on a socket never dialled; a closed fd is `POLLNVAL` at once | proc_gate pollloop, noprogress, syscorner poll, pollclosed |
| a finite `poll`, `ppoll`, `epoll_wait` timeout | exact | a deadline on the node clock; EXPIRE returns 0 with nothing ready and moves the clock to the deadline | proc_gate sleeps poll, ppoll, epoll, "a finite poll timeout fires", leasedrift |
| `nanosleep`, `clock_nanosleep` (relative or `TIMER_ABSTIME`), `select` and `pselect` with no descriptor | exact | a deadline on the node clock; `select` and `pselect` return their timeout counted down to zero; select on descriptors, select with no timeout and a sleep on a CPU-time clock are exit 3 | proc_gate sleeps nanosleep, clock_nanosleep, abstime, select, pselect, and the three refusal rows |
| deadlines of several nodes | exact in order | EXPIRE fires a node's earliest deadline only when no other node passes a deadline first; time never advances one state per tick | proc_gate leasedrift |
| time inside a step, a late wake-up | not modelled | a node's clock does not move between two of its calls, and a deadline wakes exactly on time, so a bug that needs a process pause between a clock check and the act it guards is not reached | none (a documented boundary, cmc(7) Clocks) |
| clock drift between nodes (`drift = R`) | approximate | per fired deadline, one node's clock at the slowest or fastest rate R allows against the others; rates in between, two nodes off at once and message delay are not explored | proc_gate leasedrift (L(1 + rho) caught only with drift, the right margin fails at drift 200) |
| `setitimer`, `alarm`, `timer_create`, `timerfd_create` | refused | exit 3 (a timer on the real clock) | proc_gate sleeps setitimer, alarm, timer_create, timerfd |
| `epoll_create`, `epoll_ctl`, `epoll_wait`, `epoll_pwait` | exact for level-triggered `EPOLLIN`, `EPOLLOUT` | interest list up to 16 fds | proc_gate epollloop |
| `EPOLLET`, `EPOLLONESHOT`, other epoll flags | refused | exit 3 | proc_gate "EPOLLET is refused" |
| `eventfd2` | exact without `EFD_SEMAPHORE` | `EFD_NONBLOCK` honoured | proc_gate pollloop |
| `EFD_SEMAPHORE` | refused | exit 3 | proc_gate "EFD_SEMAPHORE is refused" |

### Time, randomness and memory

| construct | status | limit | gate |
|---|---|---|---|
| `clock_gettime`, `gettimeofday`, `time` | approximate | the node's own logical clock, every clock id alike: it moves only at EXPIRE and one second per budgeted TIMEOUT; `CLOCK_REALTIME` never steps | proc_gate idle, clocks, lease, sleeps |
| `getrandom` | approximate | a fixed pattern (zero bytes), not a search choice | proc_gate clocks |
| `malloc` and the aligned allocators | exact enough | a fixed-address arena (64 MiB by default); `free` is a no-op | proc_gate "the preload allocator checks sizes and copies exactly" |
| address layout | exact | nodes run without ASLR, so a replay lays memory out alike | proc_gate fingerprint |

### Threads

| construct | status | limit | gate |
|---|---|---|---|
| the order of threads' visible operations (`futex`, a call on a checker-owned fd, a state-hook write, `clone`, a thread's exit) | exact | every order is a search choice (RUN); one thread of a node runs at a time | proc_gate twothread, racewake, raceemit, raceborn, durack |
| a call on a kernel fd (a pipe, a file) in a node with more than one thread | refused | exit 3 before it runs; writes to stdout and stderr are allowed | proc_gate racepipe, kernelfd |
| `sendmsg` on a kernel fd (below 500) | exact in a node with one thread; refused in a node with more | a second seccomp filter stops it for the checker (`SECCOMP_RET_TRACE`), which sends it on to the kernel or ends the check with exit 3 | proc_gate sendmsg worker, alone |
| worlds with a node that has run more than one thread | exact (never merged) | searched as a tree over orders of visible operations; INCOMPLETE after `--max-states` such worlds, 1000000 by default (`CMC_TREE_CAP`) | proc_gate threadpos, tree bound (a build with `-DCMC_TREE_CAP=200`); docs_gate checks the default cmc(1) states; one run to the default in [the tree-bound measurement]({{ '/background/tree-cap/' | relative_url }}) |
| `clone`, `clone3` (threads), `futex` wait and wake, `exit`, `pthread_join` | exact | a newborn and its parent are both held at the clone's return until the search runs each | proc_gate twothread_join, raceborn |
| a lock acquisition on its uncontended path (pthread mutex, rwlock, spinlock, semaphore, barrier, once; C11 `mtx_*`, `call_once`) | exact (a choice point) | the preload's marker before the real call; releases are not choice points (an acquisition-only reduction, sound for programs free of data races) | proc_gate syncpoint |
| memory accesses between two choice points, with the race runtime (`-fsanitize=thread`, `libcmc-race.a`) | checked | a happens-before race is a violation with both code addresses; atomics are choice points; code outside the instrumentation (other libraries) is not checked, except its `memcpy`, `memmove` and `memset` calls; an unmodelled edge gives a false alarm, never a missed race | proc_gate memrace, raceemit |
| memory accesses between two choice points, without the race runtime | INCOMPLETE | a multi-threaded node cannot pass, and an unmet `eventually` is INCOMPLETE | proc_gate memrace, twothread |
| `memcpy`, `memmove`, `memset` and their fortified `__*_chk` forms, with the race runtime | checked | each records the ranges it reads and writes, from instrumented code and from other libraries | proc_gate memcopy, memcopy-chk, race_rt |
| the buffers of `read`, `write`, `pread`, `pwrite`, `recv`, `send`, `recvfrom`, `sendto`, `readv`, `writev`, `recvmsg`, `sendmsg`, `poll`, `epoll_wait`, with the race runtime | checked | the bytes each call moved are an access, recorded after the call; other system calls that touch memory (`ppoll`, `epoll_pwait`, `getsockopt`, ...) are not | proc_gate syncedge readbuf, race_rt |
| a thread's end by return, `pthread_exit` or `thrd_exit`, with the race runtime | exact | the join takes the thread's final clock | proc_gate threadexit |
| a try or timed acquisition, or a semaphore wait that leaves the count above zero, between threads, with the race runtime | INCOMPLETE | a failed try, or two holders at once, lies inside a section the search runs without a switch | proc_gate syncedge trymutex, trysem, tryrw |
| the happens-before edges: rwlock readers and writers, barrier rounds, condition variables, stores, a failed compare-exchange | exact | only the edges POSIX and C11 promise; a condition signal adds none | proc_gate syncedge rwread, casfail, casok, bargen, sem2; race_rt |
| a 16-byte atomic (`__int128` with `-mcx16`), with the race runtime | checked | like the smaller atomics: a choice point, ordered by its memory order; x86-64 only (cmpxchg16b) | proc_gate atomic128, race_rt |
| an atomic weaker than `seq_cst` | INCOMPLETE | the search runs a sequentially consistent machine; a pass that used one is INCOMPLETE | proc_gate memrace relaxedadd |
| `lseek`, `fsync`, `fdatasync`, `sync_file_range` on a kernel fd in a node with more than one thread | refused | exit 3 before it runs, like any call on a kernel fd | proc_gate files lseek |
| a timed futex wait (`pthread_cond_timedwait`) | exact | a deadline on the node clock, fired as `ETIMEDOUT`; with a second thread, the wake and the timeout each come first | proc_gate sleeps futex, futexabs, condtimed, timedwait |
| `fork`, `vfork`, `clone` without `CLONE_THREAD` | refused | exit 3, before the child exists | proc_gate "a fork is refused" |
| a `FUTEX_WAKE` of a strict subset of several waiters | refused | exit 1 | (code; no fixture yet) |

### The file system

| construct | status | limit | gate |
|---|---|---|---|
| `open`, `openat`, `openat2`, `creat`, `open_by_handle_at` read-only | exact | the kernel opens the file; the file system is read-only to a node, so what it reads is the same on every replay | proc_gate files config |
| an open for writing, or with `O_CREAT`, `O_TRUNC`, `O_APPEND` or `O_TMPFILE` | refused | exit 3, naming the path; `/dev/null` is allowed | proc_gate files persist, devnull, open_wronly through creat |
| a path the checker cannot read from the node's memory | refused | exit 3 | proc_gate files badpath |
| `unlink`, `rename`, `mkdir`, `rmdir`, `link`, `symlink`, `mknod`, `truncate`, `chmod`, `chown`, `utime`, `setxattr` and `removexattr` families, `file_setattr` | refused | exit 3 | proc_gate files, one row per call |
| `ftruncate`, `fallocate`, `flock` on a kernel fd; `io_uring_setup` | refused | exit 3 | proc_gate files ftruncate, fallocate, flock, io_uring_setup |
| a channel to another process: a second `execve`, SysV shared memory, message queues and semaphores, POSIX message queues, `process_vm_readv` and `process_vm_writev`, a signal or pidfd to another process | refused | exit 3; a signal to the node itself goes to the kernel | proc_gate edges exec, shm, mq, vmread, killparent, killgroup, pidfd, killself |
| `lseek`, `fsync`, `fdatasync`, `sync_file_range` on a kernel fd, one thread | exact | the kernel answers | proc_gate files config |
| `lseek`, `fsync`, `fdatasync`, `ftruncate`, `sync_file_range` on a checker-owned fd | exact | Linux's answer for a socket (`ESPIPE`, `EINVAL`); `fallocate` and `flock` there are refused | proc_gate files sockerr, against a native run |
| `fsync`, `fdatasync` of fd 193 | exact | succeed: the record is durable when written | proc_gate files durfsync |
| a modelled file system (`[files] dir`): `open`, `openat`, `creat`, `read`, `pread64`, `write`, `pwrite64`, `lseek`, `ftruncate`, `fsync`, `fdatasync`, `close`, `fstat`, `stat`, `newfstatat`, `statx`, `access`, `mkdir`, `unlink`, `rename`, `renameat` | approximate | contents in the world; a crash keeps each file's and directory's synced contents plus a prefix of its later changes (no reordering within a file, no torn writes); a create or rename is durable after an `fsync` of its directory; single-threaded nodes; 16 files, 4096 bytes a file, 16 unsynced changes a file | proc_gate fslog, fslog-bug, fslog-bug2, fsmodel |
| `eio`, `enospc` budgets on modelled files | exact | a write or truncation fails and is undone; a failed `fsync` forgets the unsynced changes | proc_gate fsfail rewrite, retry; fsmodel |
| on a modelled path or fd: `O_DIRECT`, `O_SYNC`, `mmap`, links, xattrs, `rmdir`, `chdir`, `getdents`, a path with `..`, a rename between directories or out of the model, a node past its `bytes` budget, a multi-threaded node | refused | exit 3 | proc_gate fsfail big, odirect, mmap, link, xattr, cross |

### State, search and verdicts

| construct | status | limit | gate |
|---|---|---|---|
| the order of nodes' steps between quiescent points | exact | at launch, after a RESTART, and after a call that wakes a parked node, which node runs first is a search choice (STEP); within one step a node's calls that reach no other node are not interleaved with another's | proc_gate dialexit (both outcomes of a dial to a node that exits), badptr and dialexit 300 runs each |
| the state hook (fd 3, fd 193) | exact | a record of up to 64 bytes, by `write` or `writev`; a longer or unfinished record is exit 3 | proc_gate longrec, partrec, ackfirst writev |
| deduplication | exact | the full world encoding is compared on a fingerprint match; a connection is named by its slot, not its peer | proc_gate fingerprint, fpfuzz, twoconn |
| crash and restart | approximate | a crash keeps the fd-193 record, and any prefix of each queue of messages the node sent that its peer has not read; a restart hands it back on the first read of fd 193; a crash can fall after any write, send, close, connect or durable write of a single-threaded node | proc_gate durlog, restart, durack-single, ackfirst, conncrash, sendlost |
| `eventually P` | exact for an exhaustive search | unmet after a full search is a violation; a bounded or simulated run is INCOMPLETE | proc_gate twothread reach |
| a run that never progressed | refused | INCOMPLETE without an `eventually` property | proc_gate noprogress |
| a fault signal (`abort`, a failed `assert`, `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`) | exact at the default disposition | the node is killed and the run ends with a violation (exit 1) and its trace; a handler is exit 3 | proc_gate fault |
| a thread blocked in, or busy outside, the calls the checker sees | refused | exit 1 after 10 s with no checked call | proc_gate kernelfd |
| `--refine` (C1) and `--refine-steps` (C2) | exact for the bound variables | C2 needs every spec variable bound; checks init, gated actions, exact states | proc_gate mono, spec_gate C2 rows |

## Abstract checker (`cmc --spec`)

| construct | status | limit | gate |
|---|---|---|---|
| safety verdicts, distinct states, depth | exact | agrees with TLC on every `.cml` model; the seen set compares canonical bytes, not hashes | spec_gate, emit_gate, apalache_gate |
| `eventually P`, `A ~> B` under `fairness weak` or none | exact | TLA+ semantics with stuttering; checked against TLC on each probe | spec_gate liveness rows, emit_gate |
| liveness under `symmetry` | refused | exit 3 | (code; no fixture yet) |
| paxos, fpaxos, caspaxos, zab, fastpaxos against a hand-written TLA+ twin | exact | same distinct states and depth, and the same shortest trace for the BUG; fastpaxos under a VIEW (the twin keeps promise senders the `.cml` counts) | `make check-spec-calibration` |
| Apalache on the emitted module | bounded | `--length 10` (fastpaxos 11): complete for counter, raft, hc, twophase, fastpaxos; a bounded pass for paxos, fpaxos, caspaxos, zab (longer runs: [Apalache past length 10]({{ '/background/apalache-length/' | relative_url }})) | apalache_gate |
| `and`, `or` | exact | left to right, short-circuit, as TLC; a skipped operand is parsed (names, syntax) but not evaluated | spec_gate guard and short-circuit rows, spec_calibrate guard |
| integers | exact | 64-bit; an overflowing literal or operation is exit 3; `/` and `mod` refuse negative operands | spec_gate overflow row |
| declared domains | exact | init and every successor must lie in each variable's domain | spec_gate boundpass, succbound |
| text that does not fit a buffer | refused | exit 3 | spec_gate truncate |
| a primed variable read, an init reading an unset variable, a variable assigned twice, a redeclared bound name | refused | exit 3 | spec_gate primed, init_order, duplicate, quantifier |
| memory safety | checked | ASan and UBSan over the spec gate | `make check-asan` |

## In-process engine (`cmc --model`, `libcmc(3)`)

| construct | status | limit | gate |
|---|---|---|---|
| the search on the HC calibration core | exact at the instances run | 22 rows against TLC: verdicts and distinct-state counts | `make check-calibration`, `check-hc-bugs` |
| determinism of a core | checked | `--det` runs every step twice | toy_gate |
| uninitialised reads in a core | checked | MemorySanitizer | `make check-msan` |
| crash, simulation, replay, the fair schedule | exact | a crash clears the node's self-channel; simulation and replay check every state; an armed timer is never starved (Phase 2 of the plan) | test_cmc |

## Examples

| example | status |
|---|---|
| paxos | pass and planted bug, with and without faults (proc_gate) |
| raftlog, zab | pass and planted bug (proc_gate) |
| raft | pass and planted bug, with and without a crash (proc_gate) |
| hc | pass with a progress property at four timeouts, planted bug by seeded simulation; both only in `make check-examples-slow`, about seven minutes; partitions only by simulation |
| paxos2 | refines `paxos.cml` with a progress property; its planted bug breaks the spec's agreement (proc_gate) |
