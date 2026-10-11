#ifndef FALLOUT_PLATFORM_PS4_PS4_PATH_H_
#define FALLOUT_PLATFORM_PS4_PS4_PATH_H_

// Pure, dependency-free path helpers used by the PS4 write guard
// (ps4IsPathWritable). Kept header-only and free of SDK includes so the logic
// can be unit-tested on the host (see docs/ps4-port-log.md).

#include <stddef.h>
#include <string.h>

namespace fallout {

// Lexically normalizes [in] into [out]: collapses repeated '/', drops "." and
// resolves ".." components. Only '/' is treated as a separator, mirroring the
// console's filesystem (a '\' is an ordinary filename character there).
//
// Symlinks are NOT resolved (the target may not exist yet when we are about to
// create it, which rules out realpath()).
//
// Returns false if [in] is empty, does not fit [out], or is a relative path
// that climbs above its starting point. ".." at the root of an absolute path
// stays at the root, as POSIX does.
inline bool ps4PathNormalize(const char* in, char* out, size_t outSize)
{
    if (in == nullptr || out == nullptr || outSize < 2 || in[0] == '\0') {
        return false;
    }

    const bool absolute = in[0] == '/';
    const size_t root = absolute ? 1 : 0;
    size_t len = 0;
    if (absolute) {
        out[len++] = '/';
    }

    const char* p = in;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        const char* start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        const size_t n = static_cast<size_t>(p - start);

        if (n == 0 || (n == 1 && start[0] == '.')) {
            continue;
        }

        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (len <= root) {
                if (absolute) {
                    continue;
                }
                return false;
            }
            size_t i = len;
            while (i > root && out[i - 1] != '/') {
                i--;
            }
            len = (i > root) ? i - 1 : root;
            continue;
        }

        const size_t need = len + (len > root ? 1 : 0) + n;
        if (need + 1 > outSize) {
            return false;
        }
        if (len > root) {
            out[len++] = '/';
        }
        memcpy(out + len, start, n);
        len += n;
    }

    out[len] = '\0';
    return len > 0;
}

// True if [normPath] equals [normBase] or lies below it. Both must already be
// normalized absolute paths. Matches on a path-component boundary, so
// "/data/fallout2x" is not under "/data/fallout2". The filesystem root is never
// accepted as a base.
inline bool ps4PathIsUnder(const char* normPath, const char* normBase)
{
    const size_t baseLen = strlen(normBase);
    if (baseLen <= 1) {
        return false;
    }
    if (strncmp(normPath, normBase, baseLen) != 0) {
        return false;
    }
    return normPath[baseLen] == '/' || normPath[baseLen] == '\0';
}

} // namespace fallout

#endif // FALLOUT_PLATFORM_PS4_PS4_PATH_H_
