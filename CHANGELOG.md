# Changelog

## Unreleased

- 2026-11-03: man pages `cmc(1)`, `libcmc(3)` and `cmc(7)`; a
  documentation site built for GitHub Pages and Codeberg Pages; `make
  install` (library, header, man pages) and `make check-docs`, which is
  now part of `make check`; a Nix flake; CI on both forges.  No API
  change.

- 2026-11-03: extracted from pg_chain `test/mc/` (history kept via
  `git subtree split`).  Renamed the API prefix `mc_`/`Mc`/`MC_` to
  `cmc_`/`Cmc`/`CMC_`, the planted-bug switch `PGCHAIN_MC_BUG` to
  `CMC_BUG`, and the binary `chainmc` to `cmc`.  The command-line driver
  is now the library function `cmc_main()`, called with a model table by
  the program's own `main()`.  The library builds as `build/libcmc.a`.
