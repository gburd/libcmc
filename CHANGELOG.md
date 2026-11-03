# Changelog

## Unreleased

- 2026-11-03: extracted from pg_chain `test/mc/` (history kept via
  `git subtree split`).  Renamed the API prefix `mc_`/`Mc`/`MC_` to
  `cmc_`/`Cmc`/`CMC_`, the planted-bug switch `PGCHAIN_MC_BUG` to
  `CMC_BUG`, and the binary `chainmc` to `cmc`.  The command-line driver
  is now the library function `cmc_main()`, called with a model table by
  the program's own `main()`.  The library builds as `build/libcmc.a`.
