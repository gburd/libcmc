# libcmc project steering

## The product

libcmc is a model checker that other projects trust as a safety gate.
A bug in libcmc reads as "no bug found" in their code.  Every change is
judged by one question: could this make cmc report PASS where a real
bug exists?

## Language and build

- C11 and libc only, built with plain `make`.  gcc and clang must both
  be clean with `-Wall -Wextra -Werror`.  Scripts are POSIX shell plus
  bash.
- TLA+ appears only as the calibration oracle in `calib/hc/`, run through
  `calib/hc/tlc.sh`.  No other language is used to compute an expected
  value.
- Scratch, logs and replay files go under `$CMC_SCRATCH` (default
  `~/tmp/cmc-build`), never `/tmp`.

## The rules that keep the checker honest

1. **No omniscient oracle.**  In actor mode, an action may be enabled
   only by something a core emitted (send, timer, persist, wake) or by
   an environment budget.  `cmc_enabled()` never reads core state.
   Properties may read core state; enabled-action code never calls them.
2. **The world is the state.**  Everything that affects future behaviour
   lives in the world bytes: network, timers, persists, wakes, resets,
   budgets, the ghost log.  Nothing else does, including padding and
   uninitialised bytes.  New environment state goes into the world
   encoding and the fingerprint, and is kept canonical.
3. **The engine owns durability.**  Only `apply_persist` changes durable
   bytes.  A crash may keep any prefix of the pending persists.
4. **Limits are errors.**  Exceeding a cap is reported as an engine
   violation, never truncated.
5. **TLC semantics.**  BFS gives shortest counterexamples, depth counts
   the initial state as 1, and counts are exact.  Calibration rows must
   still match after any change to search, encoding or the HC core.

## Every fix starts as a failing test

For a soundness bug, first write the probe or test that makes cmc give
the wrong verdict, and show it is red on the current engine.  Then fix
the engine, and show the test is green.  The test asserts the right
verdict, not just that something changed.  Then rerun every gate that
the change touches.

## Evidence

Label claims: [measured], [source: file:line], [web: URL],
[estimate], [unverified].  A measurement records the command, host,
load, wall time, and the cmc and TLC versions; raw output sits next to
its write-up in `docs/measurements/`.  A run that hit its time or state
limit is "no violation found in N states", never "passes".

## Repository hygiene

- Commit as Greg Burd <greg@burd.me>, in PostgreSQL commit style: a short
  imperative subject, then a body that explains why.  Use ASCII in code
  and commits.  Add files by name, never with `git add -A`.
- Build trees (`build*/`) are gitignored.  Agent notes go in `.agent/`,
  which is local and never committed.
- A change to `include/cmc.h` is an API change: note it in
  `CHANGELOG.md`, and update `man/man3/libcmc.3`.

## Documentation

- The man pages (`man/`, mdoc) are the reference: `cmc(1)` for every
  `cmc_main()` option, `libcmc(3)` for every function and macro in
  `cmc.h`, `cmc(7)` for the model and its rules.  `make check-docs`
  (part of `make check`) lints them with mandoc and fails on an option,
  function or property macro they do not mention.
- The site (`docs/`, Jekyll with just-the-docs) is published to GitHub
  Pages and Codeberg Pages by `.github/workflows/pages.yml` and
  `.forgejo/workflows/pages.yml`.  Every page passes a link and anchor
  check before it deploys.
- Code shown in the guide is a region of a file under
  `docs/_includes/snippets/`, which `make check-docs` builds, runs, and
  checks against what the guide says.  Never paste code into a page that
  no snippet backs; a snippet must not contain `{{` or `{%` (Liquid).

## Releases

Semantic versioning, tags `vX.Y.Z`.  To cut one:

1. Set `CMC_VERSION` in `include/cmc.h`, and give `CHANGELOG.md` a
   `## X.Y.Z (date)` section (it becomes the release notes).
2. Run `make check-all` with gcc and clang, commit, push, and wait for
   CI to pass on both forges.
3. Create a signed annotated tag `vX.Y.Z` on that commit and push it to
   origin (Codeberg); the push mirror carries it to GitHub.
4. `.github/workflows/release.yml` runs every CI job at the tag, the
   full calibration included, tests the source tarball, and publishes
   the GitHub release.  `.forgejo/workflows/release.yml` publishes the
   Codeberg release from the same tag.  Both refuse a tag that is not
   `v` + `CMC_VERSION`, or a version with no `CHANGELOG.md` section.

## CI

- GitHub Actions carries the CI: `make check` with gcc and clang, the
  MemorySanitizer gate, `nix flake check`, and weekly the full
  calibration with its planted bugs.
- Codeberg runs one `make check` job on `codeberg-small` and the Pages
  deploy.  Its hosted runners are shared and asked to be used sparingly
  (<https://codeberg.org/actions/meta>); Codeberg turned Actions off on
  another of these projects for running too many pipelines.  Do not add
  jobs or matrices there, and batch fixes into few pushes.
