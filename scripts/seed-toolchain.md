# One-time: seed the OpenOrbis toolchain for CI

CI builds with the **same** OpenOrbis toolchain (pacbrew) you use locally. Package
it once, attach it to a **private release** of this repo, and CI downloads it (then
caches it in Actions — so it's fetched only on the first run / cache miss). The
toolchain never lives in the repo; releases don't count toward repo size.

Why this instead of bootstrapping pacbrew fresh in CI: it's deterministic and uses
the exact toolchain that already builds here — no dependency on pacbrew's server or
bootstrap changing. (OpenOrbis + pacbrew are open toolchains, not game content; a
private repo just keeps things tidy.)

## Steps

1. **Package the toolchain** on your build machine (WSL), from the pacbrew root:
   ```sh
   tar -czf openorbis-toolchain.tar.gz -C /opt/pacbrew .
   ```
   (Extracts back to `/opt/pacbrew`, matching `$OPENORBIS=/opt/pacbrew/ps4/openorbis`,
   so the toolchain's absolute paths line up on the runner.)

2. **Create a release** in this repo with tag **`toolchain-v1`** and attach
   `openorbis-toolchain.tar.gz` as an asset. (CLI: `gh release create toolchain-v1
   openorbis-toolchain.tar.gz -t "OpenOrbis toolchain" -n "pacbrew snapshot"`.)
   That's it — no URL or asset id to configure; the workflow finds it by tag via
   `gh release download` (auth'd with the built-in `GITHUB_TOKEN`). To use a
   different tag, set repo variable `TOOLCHAIN_TAG`.

3. **Run** the `build-ps4` workflow (Actions → build-ps4 → Run workflow). The first
   run downloads + caches the toolchain; later runs reuse the Actions cache.

## Refreshing the toolchain

When you update pacbrew locally, bump the cache key (`openorbis-toolchain-v1` →
`-v2` in `build.yml`), re-package, and update the release asset + `TOOLCHAIN_URL`.
