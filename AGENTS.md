# Project instructions

libcmc: an explicit-state model checker for C protocol code, in C11
with libc only.  The code it checks is the code that ships: cores are
pure step functions linked both into a product and into `cmc`.

**Read `docs/steering/project.md` before any change.**  It holds the
rules that keep the checker honest, the evidence standard and the
repository hygiene.

## Read first

- `docs/DESIGN.md`: the engine as built, with its roadmap.
- `docs/WHY.md`: why this exists rather than an existing tool.
- `docs/research/`: the survey (01) and the adversarial review (02).
- `docs/measurements/`: calibration against TLC.

## Skills (`.agents/skills/`)

- `libcmc-change`: any change to `src/`, `include/cmc.h`, the gates, or
  the calibration.

See .agent-steering-domains.md for domain-specific steering (local).
