# Upstream target for the overlay build.
#
# Default: a pinned, known-good CE commit for reproducible LOCAL builds.
# For the "track latest CE" path, set UPSTREAM_REF=main.
# Every build records the resolved commit in build-manifest.txt (snapshot).
UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/fallout2-ce/fallout2-ce.git}"
UPSTREAM_REF="${UPSTREAM_REF:-296c77da92ecbb374d7067f5a9225020a8e6999f}"
