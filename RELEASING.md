# Releasing libcmc

libcmc uses semantic versioning.  The one version string is
`CMC_VERSION` in `include/cmc.h`; the flake and the release workflows
read it from there.

## Steps

1. Bump `CMC_VERSION` in `include/cmc.h`.
2. Close the `## Unreleased` section in `CHANGELOG.md` with a
   `## X.Y.Z (YYYY-MM-DD)` heading (keep an empty `## Unreleased` above
   it).  `scripts/release-notes.sh X.Y.Z` must print that section; the
   release workflows fail without it.
3. Commit (`Release X.Y.Z`), push `main` to Codeberg (origin), and sync
   the GitHub mirror.
4. Wait for GitHub CI to pass on that commit.  CI runs the full
   `make check` (gcc and clang), msan, the calibration against TLC, and
   the nix build; a release should not ship on a red commit.
5. Create the annotated, signed tag and push it:

   ```sh
   git tag -a -s vX.Y.Z -m "libcmc X.Y.Z: <one-line summary>"
   git push origin vX.Y.Z
   ```

   Push the tag to Codeberg and sync the GitHub mirror.

6. The tag triggers `.github/workflows/release.yml`, which builds the
   source tarball and the man-page tarball, tests the unpacked tarball
   (`make check`, `make install`, the guide's model against the
   installed files), and publishes the **GitHub** release with those
   artifacts.  Wait for it to finish (the calibration job is the long
   pole, around nine minutes).

7. Publish the **Codeberg** release:

   ```sh
   make codeberg-release VER=X.Y.Z
   ```

   Codeberg Actions are disabled for this repository, so the forgejo
   release workflow does not run.  `scripts/codeberg-release.sh` creates
   the Codeberg release through the API and attaches the same tarballs
   the GitHub workflow built (fetched from the GitHub release).  It is
   idempotent, so it is safe to re-run.  It needs a Codeberg API token
   in `~/.config/tea/config.yml` (or `$CODEBERG_TOKEN`).

## Artifacts

Each release carries, on both forges:

- `libcmc-X.Y.Z.tar.gz` -- the source tarball (`git archive` of the tag).
- `libcmc-man-X.Y.Z.tar.gz` -- the generated man pages.

Both forges carry identical artifacts so a downstream consumer sees the
same release wherever it pulls from.
