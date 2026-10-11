#ifdef __PS4__

#include "platform/ps4/ps4_video.h"
#include "platform/ps4/ps4.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

namespace fallout {

#define PS4_FB_COUNT 2
#define PS4_FB_ALIGN 0x200000

namespace {

struct Ps4Video {
    int      video = -1;
    // Output = the registered flip-buffer resolution. This is the console's actual
    // video-out mode (720p stays 720p; 4K is capped to 1080p and the display
    // upscales).
    uint32_t outW = 1920;
    uint32_t outH = 1080;
    uint32_t pitch = 1920; // pixels
    int      srcW = 0;
    int      srcH = 0;

    // Viewport within the flip buffer:
    int      dstX = 0;
    int      dstY = 0;
    uint32_t dstW = 1920;
    uint32_t dstH = 1080;
    Ps4ScaleMode scaleMode = PS4_SCALE_LETTERBOX;

    void*    fb[PS4_FB_COUNT] = { nullptr, nullptr };
    off_t    directMemOff = 0;
    void*    directMemBase = nullptr;
    size_t   memSize = 0;
    OrbisKernelEqueue flipQueue = 0;
    int      activeIdx = 0;
    int64_t  frameID = 0;
    bool     ready = false;

    // Scaling lookup tables: colLut[ox] = source column for destination column ox (0 <= ox < dstW),
    // rowLut[oy] = source row for destination row oy (0 <= oy < dstH). Nearest sampling.
    int*     colLut = nullptr;
    int*     rowLut = nullptr;

    // Movie overlay (ARGB8888), staged from movie.cc; dst is in game/logical coords.
    uint32_t* movie = nullptr;
    int       movieCap = 0;
    int       movieW = 0;
    int       movieH = 0;
    SDL_Rect  movieDst = { 0, 0, 0, 0 };
    bool      movieActive = false;
};

Ps4Video g;

inline uint32_t toVout(uint32_t xrgb)
{
    // gSdlTextureSurface is SDL_PIXELFORMAT_RGB888 (0x00RRGGBB); sceVideoOut buffer
    // format 0x80000000 wants 0x80RRGGBB.
    return 0x80000000u | (xrgb & 0x00FFFFFFu);
}

static void ps4ClearFramebuffers()
{
    size_t fbPixels = (size_t)g.pitch * g.outH;
    for (int i = 0; i < PS4_FB_COUNT; i++) {
        if (g.fb[i] != nullptr) {
            uint64_t* p64 = (uint64_t*)g.fb[i];
            size_t count64 = fbPixels / 2;
            for (size_t px = 0; px < count64; px++) {
                p64[px] = 0x8000000080000000ull;
            }
        }
    }
}

static bool ps4ComputeViewport(int srcW, int srcH, Ps4ScaleMode mode)
{
    if (srcW <= 0 || srcH <= 0 || g.outW <= 0 || g.outH <= 0) {
        return false;
    }

    switch (mode) {
    case PS4_SCALE_FILL:
        g.dstX = 0;
        g.dstY = 0;
        g.dstW = g.outW;
        g.dstH = g.outH;
        break;

    case PS4_SCALE_INTEGER: {
        uint32_t scaleX = g.outW / (uint32_t)srcW;
        uint32_t scaleY = g.outH / (uint32_t)srcH;
        uint32_t scale = scaleX < scaleY ? scaleX : scaleY;
        if (scale < 1) scale = 1;
        g.dstW = (uint32_t)srcW * scale;
        g.dstH = (uint32_t)srcH * scale;
        g.dstX = (int)((g.outW - g.dstW) / 2);
        g.dstY = (int)((g.outH - g.dstH) / 2);
        break;
    }

    case PS4_SCALE_LETTERBOX:
    default: {
        uint64_t cross1 = (uint64_t)srcW * g.outH;
        uint64_t cross2 = (uint64_t)g.outW * srcH;
        if (cross1 > cross2) {
            // Source is wider than output -> letterbox top/bottom
            g.dstW = g.outW;
            g.dstH = (uint32_t)(((uint64_t)g.outW * srcH + (srcW / 2)) / srcW);
            if (g.dstH > g.outH) g.dstH = g.outH;
            g.dstX = 0;
            g.dstY = (int)((g.outH - g.dstH) / 2);
        } else if (cross1 < cross2) {
            // Output is wider than source -> pillarbox left/right (e.g. 4:3 on 16:9)
            g.dstH = g.outH;
            g.dstW = (uint32_t)(((uint64_t)g.outH * srcW + (srcH / 2)) / srcH);
            if (g.dstW > g.outW) g.dstW = g.outW;
            g.dstY = 0;
            g.dstX = (int)((g.outW - g.dstW) / 2);
        } else {
            // Aspect ratios match exactly (e.g. 16:9 on 16:9 output) -> 100% full screen
            g.dstX = 0;
            g.dstY = 0;
            g.dstW = g.outW;
            g.dstH = g.outH;
        }
        break;
    }
    }

    if (g.dstW == 0) g.dstW = 1;
    if (g.dstH == 0) g.dstH = 1;

    free(g.colLut); g.colLut = nullptr;
    free(g.rowLut); g.rowLut = nullptr;

    g.colLut = (int*)malloc(sizeof(int) * g.dstW);
    g.rowLut = (int*)malloc(sizeof(int) * g.dstH);
    if (g.colLut == nullptr || g.rowLut == nullptr) {
        free(g.colLut); g.colLut = nullptr;
        free(g.rowLut); g.rowLut = nullptr;
        return false;
    }

    for (uint32_t ox = 0; ox < g.dstW; ox++) {
        int sx = (int)((uint64_t)ox * srcW / g.dstW);
        g.colLut[ox] = sx < srcW ? sx : srcW - 1;
    }
    for (uint32_t oy = 0; oy < g.dstH; oy++) {
        int sy = (int)((uint64_t)oy * srcH / g.dstH);
        g.rowLut[oy] = sy < srcH ? sy : srcH - 1;
    }

    return true;
}

} // namespace

bool ps4VideoInit(int srcW, int srcH)
{
    if (srcW <= 0 || srcH <= 0) {
        return false;
    }

    // If sceVideoOut and direct memory are already initialized, we only need
    // to update the source resolution and rebuild the scaling LUTs.
    if (g.ready && g.video >= 0) {
        if (g.srcW == srcW && g.srcH == srcH && g.colLut != nullptr && g.rowLut != nullptr) {
            return true;
        }
        g.srcW = srcW;
        g.srcH = srcH;
        if (!ps4ComputeViewport(srcW, srcH, g.scaleMode)) {
            ps4Log("[ps4] LUT alloc FAILED in ps4VideoInit resize\n");
            return false;
        }
        ps4ClearFramebuffers();
        ps4Log("[ps4] ps4VideoInit source resized to %dx%d (output %ux%u viewport %ux%u at %d,%d)\n",
            srcW, srcH, g.outW, g.outH, g.dstW, g.dstH, g.dstX, g.dstY);
        return true;
    }

    if (g.ready) {
        ps4VideoShutdown();
    }

    g.srcW = srcW;
    g.srcH = srcH;

    g.video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g.video < 0) {
        ps4Log("[ps4] sceVideoOutOpen FAILED 0x%08x\n", (unsigned)g.video);
        ps4VideoShutdown();
        return false;
    }

    OrbisVideoOutResolutionStatus res;
    memset(&res, 0, sizeof(res));
    uint32_t reportedW = 1920, reportedH = 1080;
    if (sceVideoOutGetResolutionStatus(g.video, &res) == 0 && res.width >= 640 && res.width <= 3840) {
        reportedW = res.width;
        reportedH = res.height;
    }
    // Cap to 1080p: 720p and 1080p pass through unchanged (valid flip modes), but 4K
    // (3840x2160) is registered as 1080p and the display upscales — registering 4K
    // buffers needs the 4K privilege and otherwise fails (app exits to the menu).
    g.outW = reportedW > 1920 ? 1920 : reportedW;
    g.outH = reportedH > 1080 ? 1080 : reportedH;
    g.pitch = g.outW;
    ps4Log("[ps4] videoout out %ux%u (panel %ux%u), src %dx%d\n",
        g.outW, g.outH, reportedW, reportedH, srcW, srcH);

    if (sceKernelCreateEqueue(&g.flipQueue, "ps4 vout flip") < 0) {
        ps4Log("[ps4] CreateEqueue FAILED\n");
        ps4VideoShutdown();
        return false;
    }
    sceVideoOutAddFlipEvent(g.flipQueue, g.video, 0);

    size_t fbSize = (size_t)g.pitch * g.outH * 4;
    size_t total = fbSize * PS4_FB_COUNT;
    g.memSize = (total + PS4_FB_ALIGN - 1) / PS4_FB_ALIGN * PS4_FB_ALIGN;
    g.directMemOff = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), g.memSize,
            PS4_FB_ALIGN, 3 /* WC_GARLIC */, &g.directMemOff) < 0) {
        ps4Log("[ps4] AllocateDirectMemory FAILED (%zu)\n", g.memSize);
        ps4VideoShutdown();
        return false;
    }
    g.directMemBase = nullptr;
    if (sceKernelMapDirectMemory(&g.directMemBase, g.memSize, 0x33 /* CPU_RW|GPU_RW */, 0, g.directMemOff, PS4_FB_ALIGN) < 0) {
        ps4Log("[ps4] MapDirectMemory FAILED\n");
        ps4VideoShutdown();
        return false;
    }
    for (int i = 0; i < PS4_FB_COUNT; i++) {
        g.fb[i] = (void*)((uintptr_t)g.directMemBase + (size_t)i * fbSize);
    }

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1 /* LINEAR */, 0 /* 16:9 */,
        g.outW, g.outH, g.pitch);
    if (sceVideoOutRegisterBuffers(g.video, 0, g.fb, PS4_FB_COUNT, &attr) < 0) {
        ps4Log("[ps4] RegisterBuffers FAILED (%ux%u)\n", g.outW, g.outH);
        ps4VideoShutdown();
        return false;
    }
    sceVideoOutSetFlipRate(g.video, 0 /* 60Hz */);

    if (!ps4ComputeViewport(srcW, srcH, g.scaleMode)) {
        ps4Log("[ps4] LUT alloc FAILED\n");
        ps4VideoShutdown();
        return false;
    }
    ps4ClearFramebuffers();

    g.activeIdx = 0;
    g.frameID = 0;
    g.ready = true;
    ps4Log("[ps4] ps4VideoInit OK (native sceVideoOut, viewport %ux%u at %d,%d, mode=%d)\n",
        g.dstW, g.dstH, g.dstX, g.dstY, (int)g.scaleMode);
    return true;
}

void ps4VideoShutdown()
{
    if (g.video >= 0) {
        if (g.frameID > 0 && g.flipQueue > 0) {
            int64_t lastFlip = g.frameID - 1;
            OrbisKernelEvent evt;
            int cnt;
            for (int attempts = 0; attempts < 100; attempts++) {
                OrbisVideoOutFlipStatus st;
                if (sceVideoOutGetFlipStatus(g.video, &st) == 0 && st.flipArg >= lastFlip) {
                    break;
                }
                if (sceKernelWaitEqueue(g.flipQueue, &evt, 1, &cnt, 0) != 0) {
                    break;
                }
            }
        }
        sceVideoOutClose(g.video);
        g.video = -1;
    }

    if (g.directMemBase != nullptr && g.memSize > 0) {
        sceKernelMunmap(g.directMemBase, g.memSize);
        g.directMemBase = nullptr;
    }
    if (g.directMemOff != 0 && g.memSize > 0) {
        sceKernelReleaseDirectMemory(g.directMemOff, g.memSize);
        g.directMemOff = 0;
    }
    g.memSize = 0;

    if (g.flipQueue > 0) {
        sceKernelDeleteEqueue(g.flipQueue);
        g.flipQueue = 0;
    }

    for (int i = 0; i < PS4_FB_COUNT; i++) {
        g.fb[i] = nullptr;
    }

    free(g.colLut); g.colLut = nullptr;
    free(g.rowLut); g.rowLut = nullptr;
    free(g.movie);  g.movie = nullptr;
    g.movieCap = 0;
    g.movieActive = false;
    g.ready = false;
    g.activeIdx = 0;
    g.frameID = 0;
}

static void ps4CompositeMovie(uint32_t* fb)
{
    if (!g.movieActive || g.movie == nullptr || g.movieW <= 0 || g.movieH <= 0
        || g.srcW <= 0 || g.srcH <= 0 || g.dstW <= 0 || g.dstH <= 0) {
        return;
    }
    // The engine dst rect is in game/logical (src) coords; map it into output screen coords.
    int dx0 = (int)g.dstX + (int)((int64_t)g.movieDst.x * g.dstW / g.srcW);
    int dy0 = (int)g.dstY + (int)((int64_t)g.movieDst.y * g.dstH / g.srcH);
    int dw = (int)((int64_t)g.movieDst.w * g.dstW / g.srcW);
    int dh = (int)((int64_t)g.movieDst.h * g.dstH / g.srcH);
    if (dw <= 0 || dh <= 0) return;

    // Fast-path: precompute horizontal mapping on stack to eliminate divisions in inner loop.
    int movieColLut[1920];
    int lutCount = dw <= 1920 ? dw : 1920;
    for (int x = 0; x < lutCount; x++) {
        int sx = (int)((int64_t)x * g.movieW / dw);
        movieColLut[x] = sx < g.movieW ? sx : g.movieW - 1;
    }

    int startX = dx0 < 0 ? 0 : dx0;
    int endX = dx0 + dw > (int)g.outW ? (int)g.outW : dx0 + dw;
    if (endX <= startX) return;

    // NOTE: s_movieRow resides in cached RAM to avoid uncached reads from WC framebuffer.
    static uint32_t s_movieRow[1920];
    int prevSrcY = -1;
    for (int y = 0; y < dh; y++) {
        int py = dy0 + y;
        if (py < 0 || py >= (int)g.outH) continue;

        int srcY = (int)((int64_t)y * g.movieH / dh);
        if (srcY >= g.movieH) srcY = g.movieH - 1;

        if (srcY != prevSrcY) {
            const uint32_t* srow = g.movie + (size_t)srcY * g.movieW;
            for (int px = startX; px < endX; px++) {
                int x = px - dx0;
                int srcX = x < 1920 ? movieColLut[x] : (int)((int64_t)x * g.movieW / dw);
                s_movieRow[px - startX] = toVout(srow[srcX]);
            }
            prevSrcY = srcY;
        }
        memcpy(fb + (size_t)py * g.pitch + startX, s_movieRow, (size_t)(endX - startX) * sizeof(uint32_t));
    }
}

void ps4VideoPresent(SDL_Surface* frame)
{
    if (!g.ready || frame == nullptr || g.colLut == nullptr || g.rowLut == nullptr) {
        return;
    }

    if (frame->w != g.srcW || frame->h != g.srcH) {
        static bool s_warnedMismatch = false;
        if (!s_warnedMismatch) {
            s_warnedMismatch = true;
            ps4Log("[ps4] video: frame surface size mismatch (%dx%d, expected %dx%d)\n",
                   frame->w, frame->h, g.srcW, g.srcH);
        }
        return;
    }

    // Pipelined wait: wait for the buffer we are about to write into (activeIdx)
    // to no longer be displayed. For double-buffering, activeIdx is free once
    // the previous frame (frameID - 1) has been flipped.
    if (g.frameID >= PS4_FB_COUNT) {
        int64_t targetFlip = g.frameID - (PS4_FB_COUNT - 1);
        OrbisKernelEvent evt;
        int cnt;
        for (;;) {
            OrbisVideoOutFlipStatus st;
            if (sceVideoOutGetFlipStatus(g.video, &st) == 0) {
                if (st.flipArg >= targetFlip) break;
            }
            if (sceKernelWaitEqueue(g.flipQueue, &evt, 1, &cnt, 0) != 0) break;
        }
    }

    uint64_t t0 = sceKernelGetProcessTime();
    static uint64_t s_lastPresentTime = 0;
    uint64_t frameDelta = s_lastPresentTime > 0 ? (t0 - s_lastPresentTime) : 0;
    s_lastPresentTime = t0;

    uint32_t* fb = (uint32_t*)g.fb[g.activeIdx];
    const uint32_t* src = (const uint32_t*)frame->pixels;
    int srcPitch = frame->pitch / 4;

    bool isFullscreenMovie = g.movieActive && g.movie != nullptr &&
                             g.movieDst.x <= 0 && g.movieDst.y <= 0 &&
                             g.movieDst.w >= g.srcW && g.movieDst.h >= g.srcH;

    if (!isFullscreenMovie) {
        // NOTE: s_rowBuf resides in normal cached RAM. The destination fb is allocated
        // as WC (Write-Combined) direct GPU memory. Never read from fb (e.g. duplicating
        // previous lines via memcpy from drow - pitch) as uncached WC reads stall the CPU pipeline
        // and degrade framerate from 60 FPS down to ~10 FPS.
        static uint32_t s_rowBuf[1920];
        for (uint32_t oy = 0; oy < g.dstH; oy++) {
            uint32_t* drow = fb + (size_t)(g.dstY + oy) * g.pitch + g.dstX;
            if (oy == 0 || g.rowLut[oy] != g.rowLut[oy - 1]) {
                const uint32_t* srow = src + (size_t)g.rowLut[oy] * srcPitch;
                const int* col = g.colLut;
                for (uint32_t ox = 0; ox < g.dstW; ox++) {
                    s_rowBuf[ox] = toVout(srow[col[ox]]);
                }
            }
            memcpy(drow, s_rowBuf, (size_t)g.dstW * sizeof(uint32_t));
        }
    }

    ps4CompositeMovie(fb);

#ifdef PS4_LOG_FRAME_PACING
    uint64_t t1 = sceKernelGetProcessTime();
    static uint32_t s_frameLogCounter = 0;
    if (++s_frameLogCounter >= 120) {
        s_frameLogCounter = 0;
        ps4Log("[ps4] frame delta: %lu us (upscale: %lu us)\n", (unsigned long)frameDelta, (unsigned long)(t1 - t0));
    }
#endif

    sceVideoOutSubmitFlip(g.video, g.activeIdx, ORBIS_VIDEO_OUT_FLIP_VSYNC, g.frameID);

    g.activeIdx = (g.activeIdx + 1) % PS4_FB_COUNT;
    g.frameID++;
}

void ps4VideoSetScaleMode(Ps4ScaleMode mode)
{
    if (g.scaleMode == mode) return;
    g.scaleMode = mode;
    if (g.ready && g.srcW > 0 && g.srcH > 0) {
        ps4ComputeViewport(g.srcW, g.srcH, mode);
        ps4ClearFramebuffers();
    }
}

Ps4ScaleMode ps4VideoGetScaleMode()
{
    return g.scaleMode;
}

void ps4VideoMovieOverlaySet(const void* pixels, int srcW, int srcH, SDL_Rect dst)
{
    if (pixels == nullptr || srcW <= 0 || srcH <= 0) {
        g.movieActive = false;
        return;
    }
    int need = srcW * srcH;
    if (need > g.movieCap) {
        uint32_t* n = (uint32_t*)realloc(g.movie, (size_t)need * 4);
        if (n == nullptr) {
            g.movieActive = false;
            return;
        }
        g.movie = n;
        g.movieCap = need;
    }
    memcpy(g.movie, pixels, (size_t)need * 4);
    g.movieW = srcW;
    g.movieH = srcH;
    g.movieDst = dst;
    g.movieActive = true;
}

void ps4VideoMovieOverlayClear()
{
    g.movieActive = false;
}

} // namespace fallout

#endif // __PS4__
