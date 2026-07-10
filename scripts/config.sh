# Upstream target for the overlay build.
#
# Default: a pinned, known-good CE commit for reproducible LOCAL builds.
# For the "track latest CE" path (CI nightly), set UPSTREAM_REF=main.
# Every build records the resolved commit in build-manifest.txt (snapshot).
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/fallout2-ce/fallout2-ce.git}"
UPSTREAM_REF="${UPSTREAM_REF:-24199e916d5b270a0d2bcfd953eaeaef042918c0}"
