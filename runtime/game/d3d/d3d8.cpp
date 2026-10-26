// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "common/menu.h"
#include "common/menu_tap.h"
#include "common/options.h"
#include "core/diag.h"
#include "d3d/gpu_frame.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_LoadVertexShaderProgram
#define SYM_D3DDevice_LoadVertexShaderProgram FFFFFE06
#define HAS_D3DDevice_LoadVertexShaderProgram 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push;
constexpr uint32_t kDevThreshold = XDK_CDevice_PushThreshold;
constexpr uint32_t kDevBaseVertex = XDK_CDevice_BaseVertex;
constexpr uint32_t kDevIndexBuffer = XDK_CDevice_IndexBuffer;
constexpr uint32_t kDevTextures = XDK_CDevice_Textures;
constexpr uint32_t kDevRenderTarget = XDK_CDevice_RenderTarget;
constexpr uint32_t kDevDepth = XDK_CDevice_Depth;
constexpr uint32_t kDevSwapCount = XDK_CDevice_SwapCount;
constexpr uint32_t kDevBackBuffer = XDK_CDevice_BackBuffer;
constexpr uint32_t kDevFrontBuffer = XDK_CDevice_FrontBuffer;
constexpr uint32_t kDevZScale = XDK_CDevice_ZScale;
constexpr uint32_t kDevSuperX = XDK_CDevice_SuperSampleX;
constexpr uint32_t kDevSuperY = XDK_CDevice_SuperSampleY;
constexpr uint32_t kDevTransforms = XDK_CDevice_Transforms;
constexpr uint32_t kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage;
constexpr uint32_t kStreamSize = XDK_Stream_Size;

// D3DResource, D3DPixelContainer and D3DSurface fields
constexpr uint32_t kResCommon = 0;  // reference counts and type
constexpr uint32_t kResData = 4;
constexpr uint32_t kResLock = 8;
constexpr uint32_t kContainerFormat = 12;
constexpr uint32_t kContainerSize = 16;
constexpr uint32_t kSurfaceParent = 0x14;
constexpr uint32_t kSurfaceBytes = 24;
constexpr uint32_t kHeaderBytes = 20;  // a header without the surface parent

// Common: the device's references in bits 19 to 22, the type in bits 16 to 18
constexpr uint32_t kInternalRefs = 0x780000;
constexpr uint32_t kTitleRefs = 0xFFFF;  // the title's references
constexpr uint32_t kInternalRef = 0x80000;
constexpr uint32_t kTypeMask = 0x70000;
constexpr uint32_t kTypeTexture = 0x40000;
constexpr uint32_t kTypeSurface = 0x50000;

constexpr uint32_t kVsConstants = 192;  // hardware vertex constant registers
constexpr uint32_t kPsConstants = 16;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
bool g_buffers_title = false;  // g_buffers are the title's own surfaces, not the host's to free
bool g_buffers_fixed = false;   // the title's back buffer is not the front's shape (multisampled): no rotation
uint32_t g_depth = 0;
bool g_depth_title = false;  // g_depth is the title's own surface, not the host's to free
uint32_t g_scratch_push = 0;
uint32_t g_scratch_size = 0;
uint32_t g_program_start = 0;
bool g_program_from_memory = false;
bool g_program_dirty = false;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }

// verbose builds: what each render target took between swaps, logged every 120 frames
struct TargetStat {
    uint32_t size = 0, format = 0, depth = 0, draws = 0, programmable = 0, clears = 0, prims = 0;
};
std::map<uint32_t, TargetStat> g_target_stats;
bool g_trace = false;  // verbose builds: every D3D call of this frame is logged
#define TRACE(...)     do {         if (g_trace) LOGI("trace: " __VA_ARGS__);     } while (0)
std::map<uint32_t, uint32_t> g_walked;  // verbose builds: methods walked by first method, dwords
void CountTarget(bool clear, uint32_t prims = 0) {
    if (!logx::kVerbose) return;
    if (!clear)
        TRACE("draw %u vertices rt %08X zs %08X vp %u %u %u %u vs %08X tex0 %08X (data %08X fmt %08X size %08X) "
              "zenable %u stencil %u blend %u", prims, g_state.rt.data, g_state.zs.data, g_state.viewport.x, g_state.viewport.y,
              g_state.viewport.w, g_state.viewport.h, g_state.vs_handle, g_state.textures[0],
              g_state.textures[0] ? R32(g_state.textures[0] + kResData) : 0,
              g_state.textures[0] ? R32(g_state.textures[0] + kContainerFormat) : 0,
              g_state.textures[0] ? R32(g_state.textures[0] + kContainerSize) : 0,
              R32(kRenderState + gpu::kRsZEnable * 4), R32(kRenderState + gpu::kRsStencilEnable * 4), g_state.simple.size() > 0x304 / 4 ? g_state.simple[0x304 / 4] : 0);
    TargetStat& t = g_target_stats[g_state.rt.data];
    t.size = g_state.rt.size;
    t.format = g_state.rt.format;
    t.depth = g_state.zs.data;
    if (clear) {
        t.clears++;
        return;
    }
    t.draws++;
    t.prims += prims;
    if (g_state.vs_handle & 1) t.programmable++;
}
void LogTargets(uint32_t frame) {
    if (!logx::kVerbose || frame % 120) return;
    for (auto& [data, t] : g_target_stats)
        LOGI("frame %u: target %08X size %08X format %08X depth %08X: %u draws (%u shaded, %u vertices), %u clears",
             frame, data, t.size, t.format, t.depth, t.draws, t.programmable, t.prims, t.clears);
    g_target_stats.clear();
}
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res + kResCommon);
    bool surface = (common & kTypeMask) == kTypeSurface;
    if (!(common & kInternalRefs) && surface) {
        if (uint32_t parent = R32(res + kSurfaceParent)) W32(parent, R32(parent) + kInternalRef);
    }
    W32(res, R32(res) + kInternalRef);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - kInternalRef);
        bool unheld = !(R32(r) & (kInternalRefs | kTitleRefs));
        if (unheld) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res + kResCommon);
    bool surface = (common & kTypeMask) == kTypeSurface;
    if ((common & kInternalRefs) == kInternalRef && surface) {
        if (uint32_t parent = R32(res + kSurfaceParent)) drop(parent);
    }
    drop(res);
#endif
}

void Walk(uint32_t end);

// A link-time generated SwitchTexture call writes the NV2A texture methods directly instead of
// reaching the D3DDevice_SwitchTexture thunk. Keep the same stage-local texture header for both
// paths so tiled terrain and other streamed textures can replace the data/format of the bound image.
void SwitchStageTexture(uint32_t stage, uint32_t field, uint32_t value) {
    if (stage >= g_state.textures.size() || !field) return;
    static uint32_t headers[4];
    uint32_t cur = g_state.textures[stage];
    uint32_t& header = headers[stage];
    if (!header) header = mem::AllocSystem(kHeaderBytes);
    if (cur && cur != header) std::memcpy(HostPtr(header), HostPtr(cur), kHeaderBytes);
    W32(header + field, value);
    g_state.textures[stage] = header;
}

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + kResData);
        s.format = R32(header + kContainerFormat);
        s.size = R32(header + kContainerSize);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(kSurfaceBytes);
    W32(hdr + kResCommon, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + kResData, mem::PhysicalFromVirtual(data));
    W32(hdr + kResLock, 0);
    W32(hdr + kContainerFormat, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + kContainerSize, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + kSurfaceParent, 0);
    return hdr;
}

void SyncPixelShader();
// Most Wanted's 2x horizontal multisampling lays the back and depth buffers out 1280 wide, and writes that size back
// into them, while its viewport constants and textures of the back buffer are 640 wide: drawn here without the
// multisampling, both keep the front's size (left as they were, the depth buffer was dropped as a size off and the
// scene filled part of the frame)
void KeepTitleShapes() {
    if (!g_buffers_fixed) return;
    uint32_t size = R32(g_buffers[0] + kContainerSize);
    if (R32(g_buffers[1] + kContainerSize) != size) W32(g_buffers[1] + kContainerSize, size);
    if (g_depth_title && R32(g_depth + kContainerSize) != size) W32(g_depth + kContainerSize, size);
}


void SyncDeferred() {
    KeepTitleShapes();
    SyncPixelShader();
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++) {
        uint32_t stage = kTextureState + s * kStageStates * 4;
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(stage + i * 4);
    }
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

float g_view_scale = 1.0f;
bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
constexpr uint32_t kViewportScale = 58;
constexpr uint32_t kViewportOffset = 59;

void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f;
    float hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[kViewportScale] = {hw * g_view_scale, -hh * g_view_scale, (v.max_z - v.min_z) * g_state.zscale, 0};
        float x = v.x + g_state.ss_offset_x + hw;
        float y = v.y + g_state.ss_offset_y + hh;
        g_state.vsc[kViewportOffset] = {x, y, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for GetViewport and inlined readers
    uint32_t p = kDevice + XDK_CDevice_Viewport;
    W32(p, v.x);
    W32(p + 4, v.y);
    W32(p + 8, v.w);
    W32(p + 12, v.h);
    std::memcpy(HostPtr(p + 16), &v.min_z, 4);
    std::memcpy(HostPtr(p + 20), &v.max_z, 4);
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

}  // namespace

}  // namespace d3d

using namespace d3d;

// device

namespace {
// D3DPRESENT_PARAMETERS fields
constexpr uint32_t kPpWidth = 0;
constexpr uint32_t kPpHeight = 4;
constexpr uint32_t kPpFormat = 8;
constexpr uint32_t kPpCount = 12;
constexpr uint32_t kPpAutoDepth = 0x20;
constexpr uint32_t kPpDepthFormat = 0x24;
constexpr uint32_t kPpFlags = 0x28;
constexpr uint32_t kPpInterval = 0x30;
constexpr uint32_t kPpBufferSurfaces = 0x34;  // the Xbox's own fields: frame buffers and a depth buffer the title made
constexpr uint32_t kPpDepthSurface = 0x40;

struct PresentParams {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t count;
    uint32_t auto_depth;
    uint32_t depth_format;
    uint32_t interval;
    uint32_t buffer_surfaces[3];
    uint32_t depth_surface;
};

PresentParams ReadPresentParams(uint32_t pp) {
    PresentParams p;
    p.width = R32(pp + kPpWidth);
    p.height = R32(pp + kPpHeight);
    p.format = R32(pp + kPpFormat);
    p.count = std::max<uint32_t>(R32(pp + kPpCount), 1);
    p.auto_depth = R32(pp + kPpAutoDepth);
    p.depth_format = R32(pp + kPpDepthFormat);
    p.interval = R32(pp + kPpInterval);
    for (uint32_t i = 0; i < 3; i++) p.buffer_surfaces[i] = R32(pp + kPpBufferSurfaces + i * 4);
    p.depth_surface = R32(pp + kPpDepthSurface);
    if (!p.width) p.width = 640;
    if (!p.height) p.height = 480;
    return p;
}

PresentParams g_present{};  // the frame buffers' parameters

// a surface NewSurface made, its memory given back
void FreeSurface(uint32_t hdr) {
    if (!hdr) return;
    mem::FreeContiguous(mem::VirtualFromPhysical(R32(hdr + kResData)));
    mem::FreeSystem(hdr);
}

void CreateBuffers(const PresentParams& p) {
    g_present = p;
    uint32_t color_fmt = gpu::LinearColorFormat(p.format);
    g_buffer_count = std::min<uint32_t>(p.count + 1, 3);
    // Most Wanted draws into frame buffers it made itself and hands over here: presenting the host's own showed black
    g_buffers_title = p.buffer_surfaces[0] != 0;
    g_buffers_fixed = false;
    for (uint32_t i = 0; i < g_buffer_count; i++) {
        // the title's BufferSurfaces[0] is its back buffer (Most Wanted's is the 1280 wide multisampled one), [1]
        // the front; here [1] is the back buffer drawn to and [0] the front
        uint32_t title = p.buffer_surfaces[i == 0 ? 1 : i == 1 ? 0 : i];
        g_buffers[i] = g_buffers_title ? title : NewSurface(p.width, p.height, color_fmt, 4);
        if (g_buffers_title)
            LOGI("d3d: title frame buffer %u: %08X data %08X format %08X size %08X", i, g_buffers[i],
                 R32(g_buffers[i] + kResData), R32(g_buffers[i] + kContainerFormat), R32(g_buffers[i] + kContainerSize));
    }
    g_depth = 0;
    g_depth_title = false;
    if (p.auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(p.depth_format);
        g_depth = NewSurface(p.width, p.height, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    } else if (p.depth_surface) {
        // Digimon Rumble Arena 2 lays its depth buffer out in its own memory (a Z tile) and hands it over here: left
        // out, its 3D drew with no depth test, the scenery over the fighters
        g_depth = p.depth_surface;
        g_depth_title = true;
        LOGI("d3d: title depth surface %08X data %08X format %08X size %08X", g_depth, R32(g_depth + kResData),
             R32(g_depth + kContainerFormat), R32(g_depth + kContainerSize));
    }
    // Most Wanted asks for 2x horizontal multisampling: its back and depth buffers are laid out 1280 wide, while it
    // draws to the 640 wide one. Without that sampling here, all take the drawn one's size, or the depth buffer,
    // a size off, was left out of every draw (the car showed through its body)
    if (g_buffers_title) {
        // a back buffer of another shape than the front is one the console filters into the front at the swap:
        // the two do not trade places
        g_buffers_fixed = R32(g_buffers[0] + kContainerSize) != R32(g_buffers[1] + kContainerSize);
        LOGI("d3d: title back buffer %ux the front's", g_buffers_fixed ? 2 : 1);
        KeepTitleShapes();
    }
}

void EnsureBackend(uint32_t w, uint32_t h) {
    if (g_backend) return;
    g_backend = gpu::CreateBackend();
    if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
    if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
}

// the device fields the recompiled getters read: targets, super sampling and depth scale
void ApplyDeviceTargets() {
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f;
    uint32_t depth_format = g_depth ? R32(g_depth + kContainerFormat) >> 8 & 0xFF : 0;
    float zs = depth_format == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
}

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        // films pace on the count: it stands still through a focus pause
        while (game::g_focus_paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            next = clock::now();
        }
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

// fields the library's device init sets that inlined library code and the default states read, and the vertical
// blank the console's interrupt gave
void InitDeviceFields() {
    float one = 1.0f;
    SetDev(XDK_CDevice_AutoDepth, g_depth);
    std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
    std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));  // 60 Hz, widescreen
#if HAS_D3D_FvfShader
    SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    SetDev(XDK_CDevice_VBlankEvent, 0x00040000);  // notification event, signalled
    SetDev(XDK_CDevice_VBlankEvent + 4, 1);
    static bool started = false;
    if (!started) StartHostThread("vertical blank", VerticalBlank);
    started = true;
}
}  // namespace

namespace {
constexpr uint32_t kGammaRampBytes = 3 * 256;  // D3DGAMMARAMP: red, green, blue

// the ramps the library keeps in the device, both slots: a title's own GetGammaRamp copies one out (Digimon scales
// its brightness from it, and the zeros of a fresh device left the screen black)
void StoreGammaRamp(const uint8_t* ramp) {
    if constexpr (XDK_CDevice_GammaRamps != xdk::kUnknown)
        for (uint32_t i = 0; i < 2; i++)
            std::memcpy(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * kGammaRampBytes), ramp, kGammaRampBytes);
}
}  // namespace

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    PresentParams p = ReadPresentParams(pp);
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", p.width, p.height,
         p.format, p.count, p.auto_depth, p.depth_format, p.interval, R32(pp + kPpFlags));
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    // the library starts on the identity ramp
    uint8_t identity[kGammaRampBytes];
    for (uint32_t i = 0; i < kGammaRampBytes; i++) identity[i] = uint8_t(i);
    StoreGammaRamp(identity);
    ScratchPush();

    CreateBuffers(p);
    EnsureBackend(p.width, p.height);
    g_state = gpu::State{};
    g_state.viewport = {0, 0, p.width, p.height, 0.0f, 1.0f};
    ApplyDeviceTargets();
    InitDeviceFields();
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

// The library's Reset frees the frame buffers through the title's XMemFree, which never allocated these host
// surfaces (Shaolin Monks has a heap of its own there): new ones are made as CreateDevice makes them
HLE_STDCALL(D3DDevice_Reset, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    PresentParams p = ReadPresentParams(a[0]);
    const PresentParams& o = g_present;
    bool same = p.width == o.width && p.height == o.height && p.format == o.format && p.count == o.count &&
                p.auto_depth == o.auto_depth && p.depth_format == o.depth_format && p.depth_surface == o.depth_surface &&
                p.buffer_surfaces[0] == o.buffer_surfaces[0];
    LOGI("d3d: Reset%s %ux%u format %X buffers %u depth %u format %X interval %X", same ? " (same buffers)" : "", p.width, p.height, p.format, p.count,
         p.auto_depth, p.depth_format, p.interval);
    if (!same) {
        // the renderer may still read them: it finishes first
        if (g_backend) g_backend->Flush();
        for (uint32_t i = 0; i < g_buffer_count; i++) {
            uint32_t buffer = std::exchange(g_buffers[i], 0);
            if (!g_buffers_title) FreeSurface(buffer);
        }
        uint32_t depth = std::exchange(g_depth, 0);
        if (!g_depth_title) FreeSurface(depth);
        CreateBuffers(p);
    }
    ApplyDeviceTargets();
    SetDev(XDK_CDevice_AutoDepth, g_depth);
    g_state.viewport = {0, 0, p.width, p.height, 0.0f, 1.0f};
    g_state.scissor_count = 0;
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

namespace {
// perf diagnostic: who blocks on which surface, once each
void LogBlockOnce(X86Context& ctx, uint32_t data) {
    static std::unordered_set<uint32_t> seen;
    if (!diag::Perf() || !seen.insert(data).second) return;
    uint32_t code[4] = {};
    uint32_t n = 0;
    for (uint32_t i = 0; i < 32 && n < 4; i++) {
        uint32_t v = R32(ctx.esp + i * 4);
        if (v >= X86CodeBase && v < X86CodeEnd) code[n++] = v;
    }
    LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
}
}  // namespace

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res + kResCommon) & kTypeMask;
        if (type == kTypeTexture || type == kTypeSurface) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + kResData));
            LogBlockOnce(ctx, data);
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + kResData)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return a[0] && g_backend && g_backend->Busy(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }
// the library compares the GPU's get and put pointers and its status register, which nothing here moves: Most
// Wanted spun on it for good after its language screen. Idle once the renderer has drawn what was sent
#if HAS_D3DDevice_IsBusy
HLE_STDCALL(D3DDevice_IsBusy, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
#endif

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0];
    uint32_t size = R32(front + kContainerSize);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64;
    uint32_t height = (size >> 12 & 0xFFF) + 1;
    uint32_t bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + kResData));
    if (g_backend) g_backend->Sync(data, bytes);
    uint32_t common = R32(front + kResCommon);
    kernel::SavePersistedDisplay(common, R32(front + kContainerFormat), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[kGammaRampBytes];
    std::memcpy(ramp, HostPtr(a[1]), kGammaRampBytes);
    StoreGammaRamp(ramp);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    KeepTitleShapes();
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget);
    uint32_t zs = a[1];
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget);
    uint32_t old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    TRACE("set render target %08X data %08X size %08X fmt %08X, depth %08X", rt, g_state.rt.data, g_state.rt.size,
          g_state.rt.format, g_state.zs.data);
    // SetRenderTarget resets the viewport and the scissors to the whole target, as the library's does: Shaolin Monks
    // sets a 160x120 scissor for its glow and draws its HUD after the switch back to the back buffer
    uint32_t w;
    uint32_t h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    if (g_state.scissor_count) TRACE("scissors %u reset by SetRenderTarget", g_state.scissor_count);
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    // -1 the front buffer, 0 and up the back buffers
    uint32_t back = std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[back];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
uint64_t g_backend_presents = 0;

constexpr bool kGammaKnown = XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown;

// the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
void ApplyPendingGamma() {
    if (!g_backend || !kGammaKnown) return;
    uint32_t i = Dev(kDevSwapCount) & 1;
    uint32_t pending = XDK_CDevice_GammaPending + i * 4;
    if (Dev(pending) != 1) return;
    SetDev(pending, 0);
    const void* ramp = HostPtr(kDevice + XDK_CDevice_GammaRamps + i * kGammaRampBytes);
    g_backend->SetGamma(static_cast<const uint8_t*>(ramp));
}

// like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
void RotateBuffers() {
    uint32_t last = g_buffers[g_buffer_count - 1];
    uint32_t data = R32(last + kResData);
    uint32_t lock = R32(last + kResLock);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + kResData, R32(g_buffers[i - 1] + kResData));
        W32(g_buffers[i] + kResLock, R32(g_buffers[i - 1] + kResLock));
    }
    W32(g_buffers[0] + kResData, data);
    W32(g_buffers[0] + kResLock, lock);
}
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    game::WaitWhileUnfocused();
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    Walk(Dev(kDevPush));
    game::ApplyCheats();
    game::MenuFrame();
    game::PromptFrame();
    LogTargets(g_presents);
    if (g_trace) LOGI("trace: swap");
    g_trace = logx::kVerbose && g_presents % 600 == 0;
    if (logx::kVerbose && g_presents % 120 == 0 && !g_walked.empty()) {
        std::string methods;
        for (auto& [m, n] : g_walked) methods += " " + std::to_string(n) + "x" + std::to_string(m);
        LOGI("frame %u: pushed methods (dwords x method, decimal):%s", uint32_t(g_presents), methods.c_str());
        g_walked.clear();
    }
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    ApplyPendingGamma();
    if (g_backend) {
        KeepTitleShapes();
        g_backend->Present(SurfaceOf(g_buffers[1]));
        // the title records one frame while the GPU draws the one before it: a GPU bound frame rate then paces the
        // game like a frame rate cap, and the frame times the game measures match the frames shown
        if (g_backend_presents) gpu::WaitGpuFrame(g_backend_presents - 1);
        g_backend_presents++;
    }
    app::PaceFrame();
    if (!g_buffers_fixed) RotateBuffers();
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    CountTarget(true);
    TRACE("clear %u rects flags %X color %08X rt %08X zs %08X vp %u %u %u %u rect0 %d %d %d %d", a[0], a[2], a[3],
          g_state.rt.data, g_state.zs.data, g_state.viewport.x, g_state.viewport.y, g_state.viewport.w, g_state.viewport.h, rects ? int32_t(R32(a[1])) : -1, rects ? int32_t(R32(a[1] + 4)) : -1,
          rects ? int32_t(R32(a[1] + 8)) : -1, rects ? int32_t(R32(a[1] + 12)) : -1);
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    TRACE("copy rects src %08X (%08X) count %u dst %08X (%08X)", a[0], R32(a[0] + 4), a[2], a[3], R32(a[3] + 4));
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t v = a[0];
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t method = a[0] & 0x1FFC;
    uint32_t value = a[1];
    g_state.simple[method >> 2] = value;
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0];
    uint32_t v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3;
    uint32_t type = a[1];
    uint32_t v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

namespace {
// the XDK's transform types: view 0, projection 1, texture 2 to 5, world 6 to 9; the renderer keeps Direct3D 8's
// numbers (view 2, projection 3, texture 16, world 256)
uint32_t D3D8TransformType(uint32_t t) {
    if (t < 2) return t + 2;
    if (t < 6) return t + 14;
    return t + 250;
}
}  // namespace

// the device copy is what GetTransform reads
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = D3D8TransformType(t);
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0];
    uint32_t l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0];
    uint32_t on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t stage = a[0] & 3;
    uint32_t tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    // bound again: the AddRef and Release would cancel out
    bool same = tex == old;
    if (!same) AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (!same) Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3;
    uint32_t pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t method = a[0] & 0x1FFC;
    if (method < 0x1B00 || method >= 0x1B00 + 4 * 0x40) return 0;
    uint32_t stage = (method - 0x1B00) >> 6;
    SwitchStageTexture(stage, kResData, a[1]);
    SwitchStageTexture(stage, kContainerFormat, a[2]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t n = a[0] & 15;
    uint32_t vb = a[1];
    uint32_t stride = a[2];
    uint32_t stream = kStreams + n * kStreamSize;
    uint32_t old = R32(stream + XDK_Stream_Buffer);
    bool same = vb == old;
    if (!same) AddRef(ctx, vb);
    W32(stream + XDK_Stream_Stride, stride);
    W32(stream + XDK_Stream_Buffer, vb);
    if (!same) Release(ctx, old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    bool same = ib == old;
    if (!same) AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + kResData) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    if (!same) Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + kResData) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0];
    uint32_t func = a[1];
    uint32_t out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    g_program_from_memory = false;
    g_program_dirty = false;
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants) {
                if (reg < kVsConstants) g_state.vsc[reg] = v;
            }
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0];
    if (reg < kVsConstants) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0];
    if (reg < kVsConstants) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0];
    uint32_t src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < kVsConstants; i++) {
        std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0];
    uint32_t src = a[1];
    uint32_t dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < kVsConstants; i++) {
        std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    }
    return 0;
}

// Xbox vertex programs are not always bound as a complete D3D shader. Titles commonly load the
// program into NV2A program memory with push-buffer methods, then select a start slot. Keep that
// memory and combine the selected program with the bound shader's input declaration.
namespace {
constexpr uint32_t kProgramSlots = 136;
constexpr uint32_t kInstructionDwords = 4;
constexpr uint32_t kLastInstruction = 1;
constexpr uint32_t kVertexProgramType = 0x2078;
constexpr uint32_t kInputRegisters = 16;
constexpr uint32_t kShaderInputs = 0x14;
constexpr uint32_t kShaderInputBytes = 16;
constexpr uint32_t kTypeNone = 0x02;

std::array<uint32_t, kProgramSlots * kInstructionDwords> g_program_memory{};
std::map<uint64_t, std::shared_ptr<gpu::VertexShader>> g_selected;

uint32_t ElementBytes(uint32_t type) {
    uint32_t kind = type & 15;
    uint32_t count = (type >> 4) & 15;
    if (kind == 0 || kind == 6) return 4;
    if (kind == 1 || kind == 5) return 2 * count;
    if (kind == 4) return count;
    return 4 * count;
}

void StoreProgram(uint32_t slot, const std::vector<uint32_t>& words) {
    if (slot >= kProgramSlots) return;
    size_t room = g_program_memory.size() - slot * kInstructionDwords;
    std::copy_n(words.begin(), std::min(words.size(), room), g_program_memory.begin() + slot * kInstructionDwords);
}

std::vector<uint32_t> FunctionWords(uint32_t function) {
    if (!function) return {};
    uint32_t count = R32(function) >> 16;
    std::vector<uint32_t> words(count * kInstructionDwords);
    for (uint32_t i = 0; i < words.size(); i++) words[i] = R32(function + 4 + i * 4);
    return words;
}

std::vector<uint32_t> ProgramAt(uint32_t slot) {
    std::vector<uint32_t> program;
    for (; slot < kProgramSlots; slot++) {
        const uint32_t* insn = &g_program_memory[slot * kInstructionDwords];
        program.insert(program.end(), insn, insn + kInstructionDwords);
        if (insn[3] & kLastInstruction) break;
    }
    return program;
}

void ApplyGuestInputs(uint32_t handle, gpu::VertexShader& vs) {
    uint32_t inputs = (handle & ~1u) + kShaderInputs;
    for (uint32_t reg = 0; reg < kInputRegisters; reg++) {
        uint32_t entry = inputs + reg * kShaderInputBytes;
        uint32_t stream = R32(entry);
        uint32_t offset = R32(entry + 4);
        uint32_t type = R32(entry + 8);
        if (type == kTypeNone || stream >= vs.stream_stride.size()) continue;
        vs.inputs[reg] = {int(stream), offset, type};
        vs.stream_stride[stream] = std::max(vs.stream_stride[stream], offset + ElementBytes(type));
    }
}

void ApplyShaderInputs(uint32_t handle, gpu::VertexShader& vs) {
    auto it = g_vs.find(handle);
    if (it == g_vs.end()) {
        ApplyGuestInputs(handle, vs);
        return;
    }
    vs.inputs = it->second->inputs;
    vs.stream_stride = it->second->stream_stride;
}

std::shared_ptr<gpu::VertexShader> SelectedShader(uint32_t handle, uint32_t slot) {
    gpu::VertexShader selected;
    selected.program = ProgramAt(slot);
    selected.program_type = kVertexProgramType;
    if (handle) ApplyShaderInputs(handle, selected);
    else {
        selected.inputs = g_state.vs->inputs;
        selected.stream_stride = g_state.vs->stream_stride;
    }
    uint64_t key = gpu::Hash(selected.program.data(), selected.program.size() * 4);
    key = gpu::Hash(selected.inputs.data(), sizeof(selected.inputs), key);
    key = gpu::Hash(selected.stream_stride.data(), sizeof(selected.stream_stride), key);
    auto& result = g_selected[key];
    if (!result) result = std::make_shared<gpu::VertexShader>(std::move(selected));
    return result;
}

void SelectProgram(uint32_t handle, uint32_t slot) {
    g_program_start = slot;
    g_state.vs = SelectedShader(handle, slot);
    if (handle) g_state.vs_handle = handle;
    g_program_from_memory = true;
    g_program_dirty = false;
}

void SyncProgram() {
    if (!g_program_dirty || !(g_state.vs_handle & 1) || !g_state.vs) return;
    SelectProgram(0, g_program_start);
}
}  // namespace

HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end()) {
        LOG_FIRST(8, "d3d: LoadVertexShader with unknown handle %08X", a[0]);
        return 0;
    }
    StoreProgram(a[1], it->second->program);
    return 0;
}

HLE_STDCALL(D3DDevice_LoadVertexShaderProgram, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    StoreProgram(a[1], FunctionWords(a[0]));
    return 0;
}

HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t handle = a[0];
    bool bound = (g_state.vs_handle & 1) && g_state.vs;
    if (!handle && !bound) {
        LOG_FIRST(8, "d3d: SelectVertexShader at slot %u without a bound vertex shader", a[1]);
        return 0;
    }
    SelectProgram(handle, a[1]);
    return 0;
}
// D3DSCM_NORESERVEDCONSTANTS (0x10): the title's own constants in c-38 and c-37, the viewport no longer writes them
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    bool own = a[0] & 0x10;
    if (own != g_own_constants) LOG_FIRST(8, "d3d: shader constant mode %X", a[0]);
    g_own_constants = own;
    if (!own) UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    // laid out as the library's: references, flags, then the definition it points at, which titles read and change
    // through the object (Most Wanted's fixed screens drew black from the empty record this was)
    constexpr uint32_t kPsObjectDef = 0x0C;
    uint32_t rec = mem::AllocSystem(kPsObjectDef + sizeof(ps->def), 16);
    W32(rec, 1);
    W32(rec + 4, 1);
    W32(rec + 8, rec + kPsObjectDef);
    std::memcpy(HostPtr(rec + kPsObjectDef), ps->def, sizeof(ps->def));
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

namespace d3d {
namespace {
// D3DRS_PSALPHAINPUTS0 to D3DRS_PSINPUTTEXTURE shadow the bound shader's definition, the same layout; texture modes
// (54) became a complex state of their own, D3DRS_PSTEXTUREMODES
constexpr uint32_t kPsRenderStates = 57;
constexpr uint32_t kPsTextureModesIndex = 54;

// the pixel shader object's definition as the combiner state, like the XDK pushes it
void PutPixelShader(const uint32_t* d);
void LoadPixelShader(uint32_t handle) {
    g_state.ps = nullptr;
    if (XDK_CDevice_PixelShader != xdk::kUnknown) SetDev(XDK_CDevice_PixelShader, handle);
    if (!handle) return;
    auto it = g_ps.find(handle);
    if (it != g_ps.end() && XDK_PixelShader_Definition == xdk::kUnknown) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(handle + XDK_PixelShader_Definition));
        auto& ps = made[handle];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = handle;
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", handle);
        return;
    }
    // as the library's SetPixelShader, the render states take the definition for titles to change
    for (uint32_t i = 0; i < kPsRenderStates; i++)
        if (i != kPsTextureModesIndex) W32(kRenderState + i * 4, g_state.ps->def[i]);
    W32(kRenderState + gpu::kRsPsTextureModes * 4, g_state.ps->def[kPsTextureModesIndex]);
    PutPixelShader(g_state.ps->def);
}

void PutPixelShader(const uint32_t* d) {
    auto put = [&](uint32_t method, uint32_t v) { g_state.simple[method >> 2] = v; };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
}

// the bound pixel shader as the library pushes it when it draws: the device's shader, then its render states
void SyncPixelShader() {
    // inlined library code (state blocks) sets the device's pixel shader without SetPixelShader
    if (XDK_CDevice_PixelShader != xdk::kUnknown) {
        uint32_t bound = Dev(XDK_CDevice_PixelShader);
        if (bound != (g_state.ps ? g_state.ps->handle : 0)) {
            LOG_FIRST(8, "d3d: device pixel shader %08X set without SetPixelShader", bound);
            LoadPixelShader(bound);
        }
    }
    if (!g_state.ps) return;
    // Most Wanted sets the combiners through the D3DRS_PS render states, written inline between draws: they are what
    // the library pushes, the definition only their starting point (its screens drew black from the definition's)
    uint32_t d[std::size(gpu::PixelShader{}.def)];
    std::memcpy(d, g_state.ps->def, sizeof(d));
    for (uint32_t i = 0; i < kPsRenderStates; i++)
        if (i != kPsTextureModesIndex) d[i] = R32(kRenderState + i * 4);
    d[kPsTextureModesIndex] = R32(kRenderState + gpu::kRsPsTextureModes * 4);
    if (std::memcmp(d, g_state.ps->def, sizeof(d)) == 0) return;
    static std::map<std::vector<uint32_t>, std::shared_ptr<gpu::PixelShader>> edited;
    if (edited.size() > 4096) edited.clear();  // constants a title fades through
    auto& ps = edited[std::vector<uint32_t>(d, d + std::size(d))];
    if (!ps) {
        LOG_FIRST(4, "d3d: pixel shader %08X changed through its render states", g_state.ps->handle);
        ps = std::make_shared<gpu::PixelShader>();
        ps->handle = g_state.ps->handle;
        std::memcpy(ps->def, d, sizeof(d));
    }
    g_state.ps = ps;
    PutPixelShader(ps->def);
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    LoadPixelShader(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0];
    uint32_t src = a[1];
    uint32_t n = a[2];
    for (uint32_t i = 0; i < n && reg + i < kPsConstants; i++) {
        std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    }
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncProgram();
    SyncDeferred();
    CountTarget(false, a[2]);
    if (g_trace && (g_state.vs_handle & 1) && g_state.vs && g_state.streams[0].vb && a[2] <= 8) {
        static int dumps = 0;
        if (dumps++ < 6) {
            auto& vs = *g_state.vs;
            std::string in;
            for (int r = 0; r < 16; r++)
                if (vs.inputs[r].stream >= 0)
                    in += " v" + std::to_string(r) + "@" + std::to_string(vs.inputs[r].offset) + "t" +
                          std::to_string(vs.inputs[r].type);
            uint32_t stride = g_state.streams[0].stride;
            uint32_t base = mem::VirtualFromPhysical(R32(g_state.streams[0].vb + kResData)) + a[1] * stride;
            LOGI("trace:   vs inputs%s stride %u", in.c_str(), stride);
            for (uint32_t v = 0; v < a[2] && v < 4; v++) {
                float f[6];
                std::memcpy(f, HostPtr(base + v * stride), 24);
                LOGI("trace:   v%u %.2f %.2f %.2f %.2f %.2f %.2f", v, f[0], f[1], f[2], f[3], f[4], f[5]);
            }
            for (int c : {96 - 38, 96 - 37, 0, 1, 2, 3, 4, 5, 6, 7})
                LOGI("trace:   c[%d] %.4f %.4f %.4f %.4f", c, g_state.vsc[c][0], g_state.vsc[c][1], g_state.vsc[c][2],
                     g_state.vsc[c][3]);
        }
    }
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncProgram();
    SyncDeferred();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    CountTarget(false, a[1]);
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20;
constexpr uint32_t kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0;
uint32_t g_up_pos = 0;
uint32_t g_up_header = 0;
uint32_t g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(kHeaderBytes);
        std::memset(HostPtr(g_up_header), 0, kHeaderBytes);
        W32(g_up_header + kResCommon, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    // the halves the last copy ended in and this one ends in
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1;
    uint32_t to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + kResData, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

// The game's 2D shader (its HUD, letterboxes and pages): position float3 at 0, colour at 12, texture coordinates as a
// packed normal at 16. Its screen position is c102..c105 times the position, scaled by c58, over w, plus c59, the y row
// of the negated position; its texture coordinates the packed normal plus c114. A quad of it goes to the prompts.
void HudQuad(const void* vertices, uint32_t stride) {
    const auto* vs = g_state.vs.get();
    if (!(g_state.vs_handle & 1) || !vs || stride != 20 || vs->inputs[0].type != 0x32 || vs->inputs[3].type != 0x16 ||
        vs->inputs[3].offset != 16)
        return;
    const auto& c = g_state.vsc;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t* p = static_cast<const uint8_t*>(vertices) + i * stride;
        float pos[4] = {0, 0, 0, 1};
        std::memcpy(pos, p, 12);
        auto dot = [&](int r, float sign) {
            return sign * (c[r][0] * pos[0] + c[r][1] * pos[1] + c[r][2] * pos[2] + c[r][3] * pos[3]);
        };
        float w = dot(105, 1);
        if (std::fabs(w) < 1e-6f) return;
        float sx = dot(102, 1) * c[58][0] / w + c[59][0];
        float sy = dot(103, -1) * c[58][1] / w + c[59][1];
        uint32_t packed;
        std::memcpy(&packed, p + 16, 4);
        float u = std::max(float(int32_t(packed << 21) >> 21) / 1023.0f, -1.0f) + c[114][0];
        float v = std::max(float(int32_t(packed << 10) >> 21) / 1023.0f, -1.0f) + c[114][1];
        x0 = std::min(x0, sx), y0 = std::min(y0, sy), x1 = std::max(x1, sx), y1 = std::max(y1, sy);
        u0 = std::min(u0, u), v0 = std::min(v0, v), u1 = std::max(u1, u), v1 = std::max(v1, v);
    }
    game::PromptQuad(g_state.textures[0], x0, y0, x1, y1, u0, v0, u1, v1);
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    auto stream = g_state.streams[0];
    if (g_trace && (g_state.vs_handle & 1) && g_state.vs && count <= 8) {
        static int dumps = 0;
        if (dumps++ < 8) {
            const uint8_t* b = static_cast<const uint8_t*>(vertices);
            std::string in;
            for (int r = 0; r < 16; r++)
                if (g_state.vs->inputs[r].stream >= 0)
                    in += " v" + std::to_string(r) + "@" + std::to_string(g_state.vs->inputs[r].offset) + "t" +
                          std::to_string(g_state.vs->inputs[r].type);
            LOGI("trace:   up vs %08X inputs%s stride %u", g_state.vs_handle, in.c_str(), stride);
            for (uint32_t v = 0; v < std::min(count, 4u); v++) {
                float f[6] = {};
                std::memcpy(f, b + v * stride, std::min<uint32_t>(stride, 24));
                LOGI("trace:   up v%u %.2f %.2f %.2f %.2f %.2f %.2f", v, f[0], f[1], f[2], f[3], f[4], f[5]);
            }
            for (int c : {58, 59, 102, 103, 104, 105, 114, 115})
                LOGI("trace:   c%d %.4f %.4f %.4f %.4f", c, g_state.vsc[c][0], g_state.vsc[c][1], g_state.vsc[c][2],
                     g_state.vsc[c][3]);
        }
    }
    if (g_trace && !(g_state.vs_handle & 1) && (g_state.vs_handle & 0xE) == 4) {
        const uint8_t* b = static_cast<const uint8_t*>(vertices);
        for (uint32_t v = 0; v < std::min(count, 4u); v++) {
            float f[6] = {};
            std::memcpy(f, b + v * stride, std::min<uint32_t>(stride, 24));
            uint32_t uv_at = stride - 8;  // the last texture coordinate pair
            float uv[2];
            std::memcpy(uv, b + v * stride + uv_at, 8);
            LOGI("trace:   v%u xyzw %.1f %.1f %.2f %.2f uv %.3f %.3f (stride %u)", v, f[0], f[1], f[2], f[3], uv[0], uv[1],
                 stride);
        }
    }
    if (count == 4) HudQuad(vertices, stride);
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    CountTarget(false, count);
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the
// scratch buffer.
namespace d3d {
namespace {
// push buffer methods: begin/end, inline vertices, vertex data of 2 floats, 4 bytes and 4 floats per slot
constexpr uint32_t kMBeginEnd = 0x17FC;
constexpr uint32_t kMInlineArray = 0x1818;
constexpr uint32_t kMVertexData2F = 0x1880;
constexpr uint32_t kMVertexData2FEnd = 0x1900;
constexpr uint32_t kMVertexData4UB = 0x1940;
constexpr uint32_t kMVertexData4UBEnd = 0x1980;
constexpr uint32_t kMVertexData4F = 0x1A00;
constexpr uint32_t kMVertexData4FEnd = 0x1B00;

constexpr uint32_t kImSlots = 16;
constexpr uint32_t kImFloats = kImSlots * 4;

uint32_t g_im_prim = 0;           // open primitive
uint32_t g_im_mask = 0;           // slots it wrote
float g_im_attr[kImFloats];       // 16 slots of x y z w
std::vector<float> g_im_vertices;  // every slot per vertex
std::vector<uint16_t> g_im_indices;
std::vector<uint32_t> g_im_inline;  // INLINE_ARRAY: whole vertices of the bound format

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
// Immediate vertices with an FVF of D3DFVF_XYZRHW: packed as a vertex buffer of that FVF and drawn as one, the
// fixed function path taking the screen positions as they are. The slots are the NV2A's: position 0, diffuse 3,
// specular 4, texture coordinates 9 on
void DrawPretransformed(uint32_t prim, uint32_t count, uint32_t fvf) {
    static std::vector<uint8_t> packed;
    auto color = [](const float* c) {  // r g b a to a D3DCOLOR
        auto byte = [](float f) { return uint32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
        return byte(c[3]) << 24 | byte(c[0]) << 16 | byte(c[1]) << 8 | byte(c[2]);
    };
    uint32_t textures = (fvf >> 8) & 0xF;
    if (logx::kVerbose) {
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        static std::set<std::array<uint32_t, 9>> views;
        const gpu::Viewport& vp = g_state.viewport;
        std::array<uint32_t, 9> key = {vp.x, vp.y, vp.w, vp.h, g_state.scissor_count, g_state.scissors[0],
                                       g_state.scissors[2], g_state.scissors[3], g_state.rt.size};
        if (views.insert(key).second && views.size() < 16)
            LOGI("d3d: immediate XYZRHW viewport %u %u %u %u scissors %u (%u..%u, ..%u) rt %08X size %08X", vp.x, vp.y,
                 vp.w, vp.h, g_state.scissor_count, g_state.scissors[0], g_state.scissors[2], g_state.scissors[3],
                 g_state.rt.data, g_state.rt.size);
        if (seen.insert({fvf, g_im_mask}).second)
            LOGI("d3d: immediate XYZRHW fvf %X slots %X prim %u count %u tex0 %08X", fvf, g_im_mask, prim, count,
                 g_state.textures[0]);
    }
    packed.clear();
    if (logx::kVerbose) {
        static int dumped = 0;
        for (uint32_t v = 0; v < count && dumped < 24; v++, dumped++) {
            const float* q = &g_im_vertices[v * kImFloats];
            LOGI("d3d: imm v%u pos %.1f %.1f %.3f %.3f d %.2f %.2f %.2f %.2f t %.3f %.3f tex %08X", v, q[0], q[1],
                 q[2], q[3], q[12], q[13], q[14], q[15], q[36], q[37], g_state.textures[0]);
        }
    }
    {
        // the box the primitive covers, for the menus the touch controls tap
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t v = 0; v < count; v++) {
            const float* q = &g_im_vertices[v * kImFloats];
            x0 = std::min(x0, q[0]), y0 = std::min(y0, q[1]), x1 = std::max(x1, q[0]), y1 = std::max(y1, q[1]);
        }
        const float* d = &g_im_vertices[12];
        uint32_t t = g_state.textures[0];
        if (fvf & 0x40) game::MenuQuad(t, x0, y0, x1, y1, d[0], d[1], d[2], d[3]);
        if (count == 3 || count == 4) {
            float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
            for (uint32_t v = 0; v < count; v++) {
                const float* q = &g_im_vertices[v * kImFloats + 9 * 4];
                u0 = std::min(u0, q[0]), v0 = std::min(v0, q[1]), u1 = std::max(u1, q[0]), v1 = std::max(v1, q[1]);
            }
            game::PromptQuad(t, x0, y0, x1, y1, u0, v0, u1, v1);
        }
        if (g_trace) LOGI("trace:   imm %u verts box %.0f %.0f %.0f %.0f d %.2f %.2f %.2f %.2f tex %08X fmt %08X", count, x0, y0, x1,
             y1, d[0], d[1], d[2], d[3], t, t ? R32(t + kContainerFormat) : 0);
    }
    for (uint32_t v = 0; v < count; v++) {
        const float* slots = &g_im_vertices[v * kImFloats];
        auto put = [&](const void* p, size_t n) {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            packed.insert(packed.end(), b, b + n);
        };
        // x y z rhw; an rhw of 0 is 1, as the GPU's pass through takes it (Shaolin Monks' menus leave it 0)
        float pos[4] = {slots[0], slots[1], slots[2], slots[3] != 0.0f ? slots[3] : 1.0f};
        put(pos, 16);
        if (fvf & 0x40) {  // D3DFVF_DIFFUSE
            uint32_t c = color(slots + 3 * 4);
            put(&c, 4);
        }
        if (fvf & 0x80) {  // D3DFVF_SPECULAR
            uint32_t c = color(slots + 4 * 4);
            put(&c, 4);
        }
        for (uint32_t t = 0; t < textures; t++) {
            // D3DFVF_TEXCOORDSIZEn in bits 16 + 2t: 0 two floats, 1 three, 2 four, 3 one
            static constexpr uint32_t kFloats[] = {2, 3, 4, 1};
            put(slots + (9 + t) * 4, kFloats[(fvf >> (16 + t * 2)) & 3] * 4);
        }
    }
    DrawUp(prim, count, packed.data(), uint32_t(packed.size() / count));
}

void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from;
        std::shared_ptr<gpu::VertexShader> vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim;
    uint32_t count = uint32_t(g_im_vertices.size() / kImFloats);
    uint32_t handle = g_state.vs_handle;
    g_im_prim = 0;
    // A program upload and its draw can be in the same push buffer. Waiting
    // until Walk returns would draw with the previous program.
    if (prim) SyncProgram();
    // Most Wanted pushes its car bodies' vertices inline in the push buffer: left out, the cars showed their insides
    if (prim && !g_im_inline.empty()) {
        uint32_t stride = VertexBytes(), bytes = uint32_t(g_im_inline.size()) * 4;
        if (stride && bytes % stride == 0) {
            DrawUp(prim, bytes / stride, g_im_inline.data(), stride);
        } else {
            LOG_FIRST(8, "d3d: inline primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
        }
        g_im_inline.clear();
    }
    if (prim && !g_im_indices.empty()) {
        SyncDeferred();
        CountTarget(false, uint32_t(g_im_indices.size()));
        LOG_FIRST(8, "d3d: pushed indexed draw prim %u indices %zu vs %08X vb %08X stride %u", prim,
                  g_im_indices.size(), handle, g_state.streams[0].vb, g_state.streams[0].stride);
        LOG_FIRST(4, "d3d: raw draw shader ps %08X combiner %08X final %08X/%08X program start %u memory %u dirty %u",
                  g_state.ps ? g_state.ps->handle : 0, g_state.simple[gpu::kMPsCombinerCount >> 2],
                  g_state.simple[gpu::kMPsFinalAbcd >> 2], g_state.simple[gpu::kMPsFinalEfgh >> 2],
                  g_program_start, g_program_from_memory, g_program_dirty);
        if (g_backend) g_backend->Draw(g_state, prim, 0, uint32_t(g_im_indices.size()),
                                      reinterpret_cast<const uint8_t*>(g_im_indices.data()));
        g_im_indices.clear();
    }
    if (!prim || !count) return;
    bool pretransformed = !(handle & 1) && (handle & 0xE) == 4;  // D3DFVF_XYZRHW
    if (pretransformed) {
        DrawPretransformed(prim, count, handle);
        return;
    }
    auto bound = g_state.vs;
    auto from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < kImSlots; s++) {
            if (!(g_im_mask >> s & 1)) continue;
            m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};  // FLOAT4
            m.vs->stream_stride[0] += 16;
        }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++) {
        for (uint32_t s = 0; s < kImSlots; s++) {
            const float* slot = &g_im_vertices[v * kImFloats + s * 4];
            if (g_im_mask >> s & 1) packed.insert(packed.end(), slot, slot + 4);
        }
    }
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// one value of a method: begin/end opens or draws the primitive, vertex data fills a slot
void ApplyImmediateMethod(uint32_t m, uint32_t v) {
    float f;
    float* a = g_im_attr;
    bool last = true;  // the slot's last component
    std::memcpy(&f, &v, 4);
    if (m == kMBeginEnd) {  // 0 ends
        if (v) {
            g_im_prim = v;
            g_im_mask = 0;
            g_im_vertices.clear();
            g_im_indices.clear();
            g_im_inline.clear();
        } else {
            EndPrimitive();
        }
        return;
    }
    if (m == 0x1800 || m == 0x1804) {  // two packed 16-bit array elements
        if (g_im_prim) {
            g_im_indices.push_back(uint16_t(v));
            g_im_indices.push_back(uint16_t(v >> 16));
        }
        return;
    }
    if (m == 0x1808) {  // one 32-bit array element: an odd count's last index
        if (g_im_prim) g_im_indices.push_back(uint16_t(v));
        return;
    }
    if (m == kMInlineArray) {
        if (g_im_prim) g_im_inline.push_back(v);
        return;
    }
    if (m >= kMVertexData2F && m < kMVertexData2FEnd) {  // 2 floats, z = 0 and w = 1
        a += (m - kMVertexData2F) / 8 * 4;
        last = (m - kMVertexData2F) / 4 & 1;
        a[last] = f;
        a[2] = 0;
        a[3] = 1;
    } else if (m >= 0x1900 && m < 0x1940) {  // 2 signed shorts
        a += (m - 0x1900) / 4 * 4;
        a[0] = float(int16_t(v));
        a[1] = float(int16_t(v >> 16));
        a[2] = 0;
        a[3] = 1;
    } else if (m >= kMVertexData4UB && m < kMVertexData4UBEnd) {  // 4 bytes
        a += (m - kMVertexData4UB) / 4 * 4;
        for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
    } else if (m >= 0x1980 && m < 0x1A00) {  // 4 signed shorts, two to a dword
        a += (m - 0x1980) / 8 * 4;
        last = (m - 0x1980) / 4 & 1;
        a[last * 2] = float(int16_t(v));
        a[last * 2 + 1] = float(int16_t(v >> 16));
    } else if (m >= kMVertexData4F && m < kMVertexData4FEnd) {  // 4 floats
        uint32_t component = (m - kMVertexData4F) / 4 & 3;
        a += (m - kMVertexData4F) / 16 * 4;
        last = component == 3;
        a[component] = f;
    } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
        last = m == 0x1508;
        a[(m - 0x1500) / 4] = f;
        a[3] = 1;
    } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
        last = m == 0x1524;
        a[(m - 0x1518) / 4] = f;
    } else {
        return;
    }
    if (!g_im_prim) return;
    g_im_mask |= 1u << (a - g_im_attr) / 4;
    // slot 0, the position, completes a vertex
    if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + kImFloats);
}

constexpr uint32_t kConstantLoad = 0x1EA4;
constexpr uint32_t kConstantData = 0x0B80;
constexpr uint32_t kConstantDataEnd = 0x0C00;
uint32_t g_constant_register = 0;
uint32_t g_constant_component = 0;

bool ApplyConstant(uint32_t m, uint32_t v) {
    if (m == kConstantLoad) {
        g_constant_register = v;
        g_constant_component = 0;
        return true;
    }
    if (m < kConstantData || m >= kConstantDataEnd) return false;
    if (g_constant_register < kVsConstants)
        std::memcpy(&g_state.vsc[g_constant_register][g_constant_component], &v, 4);
    if (++g_constant_component == 4) {
        g_constant_component = 0;
        g_constant_register++;
    }
    return true;
}

constexpr uint32_t kProgramLoad = 0x1E9C;
constexpr uint32_t kProgramData = 0x0B00;
constexpr uint32_t kProgramDataEnd = 0x0B80;
constexpr uint32_t kProgramStart = 0x1EA0;
uint32_t g_program_load = 0;

bool ApplyProgram(uint32_t m, uint32_t v) {
    if (m == kProgramLoad) {
        g_program_load = v * kInstructionDwords;
        return true;
    }
    if (m == kProgramStart) {
        g_program_start = v;
        g_program_dirty = true;
        return true;
    }
    if (m < kProgramData || m >= kProgramDataEnd) return false;
    if (g_program_load < g_program_memory.size()) g_program_memory[g_program_load++] = v;
    g_program_dirty |= g_program_from_memory;
    return true;
}

// Titles can inline the XDK's visibility, palette and texture methods in the push buffer. These
// never call the corresponding host API entry points, so consume them while walking raw commands.
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable && m >> 2 < g_state.simple.size()) g_state.simple[m >> 2] = v;
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

constexpr uint32_t kTextureOffsetMethod = 0x1B00;
constexpr uint32_t kTextureFormatMethod = 0x1B04;
constexpr uint32_t kTextureStageStride = 0x40;

void ApplyTextureSwitch(uint32_t m, uint32_t v) {
    if (m < kTextureOffsetMethod || m >= kTextureOffsetMethod + 4 * kTextureStageStride) return;
    uint32_t stage = (m - kTextureOffsetMethod) / kTextureStageStride;
    uint32_t reg = (m - kTextureOffsetMethod) % kTextureStageStride;
    uint32_t field = reg == 0 ? kResData : reg == kTextureFormatMethod - kTextureOffsetMethod ? kContainerFormat : 0;
    uint32_t current = g_state.textures[stage];
    if (!field || !current || R32(current + field) == v) return;
    SwitchStageTexture(stage, field, v);
}

constexpr uint32_t kTexturePalette = 0x1B20;

bool ApplyPalette(uint32_t m, uint32_t v) {
    if (m < kTexturePalette || m >= kTexturePalette + 4 * kTextureStageStride ||
        (m - kTexturePalette) % kTextureStageStride)
        return false;
    static uint32_t headers[4];
    uint32_t stage = (m - kTexturePalette) / kTextureStageStride;
    if (!headers[stage]) headers[stage] = mem::AllocSystem(12);
    W32(headers[stage], (v >> 2 & 3) << 30 | 0x30001);
    W32(headers[stage] + 4, v & ~0x3Fu);
    W32(headers[stage] + 8, 0);
    g_state.palettes[stage] = headers[stage];
    return true;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, the rest is skipped
void ApplyWalkedMethod(uint32_t m, uint32_t v) {
    ApplyVisibilityMethod(m, v);
    ApplyPalette(m, v);
    ApplyTextureSwitch(m, v);
    if (!ApplyConstant(m, v) && !ApplyProgram(m, v)) {
        ApplyImmediateMethod(m, v);
        if (m >> 2 < g_state.simple.size()) g_state.simple[m >> 2] = v;
    }
}

constexpr uint32_t kPushReturn = 0x00020000;
constexpr uint32_t kPushSegmentMax = 4u << 20;  // bytes a called or jumped to segment is read for at most

// a jump or call's target: new jump addr|1, call addr|2, old jump 0x20000000|addr; 0 for a method header
uint32_t PushTarget(uint32_t head, bool& call) {
    call = (head & 3) == 2;
    if ((head & 3) == 1 || call) return mem::VirtualFromPhysical(head & ~3u);
    if ((head & 0xE0000003) == 0x20000000) return mem::VirtualFromPhysical(head & 0x1FFFFFFC);
    return 0;
}

// A pushed segment the main buffer calls or jumps to, as a pushbuffer the library runs (Most Wanted's car bodies):
// read until its return, or its jump back to resume, the main buffer's next command
void WalkSegment(uint32_t p, uint32_t resume, int depth, uint32_t& budget) {
    uint32_t end = p + kPushSegmentMax;
    while (p + 4 <= end && budget) {
        uint32_t head = R32(p);
        p += 4;
        budget--;
        if (head == kPushReturn) return;
        bool call;
        if (uint32_t target = PushTarget(head, call)) {
            if (call) {
                if (depth < 4) WalkSegment(target, p, depth + 1, budget);
                continue;
            }
            if (target == resume) return;
            p = target;
            end = target + kPushSegmentMax;
            continue;
        }
        uint32_t method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        if (head & 0xA0030003) {
            LOG_FIRST(8, "d3d: pushed segment stops at %08X: %08X", p - 4, head);
            return;
        }
        if (logx::kVerbose) g_walked[method] += n;
        for (uint32_t i = 0; i < n && budget; i++, p += 4, budget--)
            ApplyWalkedMethod(head & 0x40000000 ? method : method + i * 4, R32(p));
    }
}

void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p);
        uint32_t method = head & 0x1FFC;
        uint32_t n = (head >> 18) & 0x7FF;
        p += 4;
        bool call;
        if (uint32_t target = PushTarget(head, call)) {
            LOG_FIRST(8, "d3d: push buffer %s to %08X from %08X", call ? "call" : "jump", target, p - 4);
            if (target != p) {
                uint32_t budget = kPushSegmentMax;
                WalkSegment(target, p, 0, budget);
            }
            continue;
        }
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: push buffer walk stops at %08X: %08X", p - 4, head);
            break;
        }
        if (logx::kVerbose) g_walked[method] += n;
        for (uint32_t i = 0; i < n; i++, p += 4) ApplyWalkedMethod(head & 0x40000000 ? method : method + i * 4, R32(p));
    }
    g_walk = end;
    SyncProgram();
}
}  // namespace
}  // namespace d3d

// (register, x, y, z, w), register -1 the position
#if HAS_D3DDevice_SetVertexData4f
HLE_STDCALL(D3DDevice_SetVertexData4f, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // the other attributes pushed before this one apply first, as on the GPU
    int32_t reg = int32_t(a[0]);
    if (g_im_prim || reg < 0) {
        uint32_t base = reg < 0 ? kMVertexData4F : kMVertexData4F + uint32_t(reg) * 16;
        for (uint32_t i = 0; i < 4; i++) ApplyImmediateMethod(base + i * 4, a[1 + i]);
        return 0;
    }
    if (reg < 16)
        for (int i = 0; i < 4; i++) g_state.vattr[reg][i] = ArgF(a, 1 + i);
    return 0;
}
#endif
// (register, D3DCOLOR): r, g, b, a as a D3DCOLOR stream element reads it
#if HAS_D3DDevice_SetVertexData4ub
HLE_STDCALL(D3DDevice_SetVertexData4ub, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t reg = a[0], c = a[1];
    // the NV2A's byte order, as the library swizzles it: r g b a from the low byte up
    uint32_t rgba = (c >> 16 & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16 | (c & 0xFF000000);
    if (g_im_prim) {
        ApplyImmediateMethod(kMVertexData4UB + reg * 4, rgba);
        return 0;
    }
    if (reg < 16)
        for (int i = 0; i < 4; i++) g_state.vattr[reg][i] = float(rgba >> (i * 8) & 0xFF) / 255.0f;
    return 0;
}
#endif

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncProgram();
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

// DrawIndexedVerticesUP(prim, index count, 16 bit indices, vertices, stride): the library pushes the vertices inline;
// here both go through the ring, the indices first, the vertices up to the highest index after them
#if HAS_D3DDevice_DrawIndexedVerticesUP
HLE_STDCALL(D3DDevice_DrawIndexedVerticesUP, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncProgram();
    uint32_t prim = a[0], count = a[1], stride = a[4];
    if (!g_backend || !count || !stride || !a[2] || !a[3]) return 0;
    const uint16_t* indices = static_cast<const uint16_t*>(HostPtr(a[2]));
    uint32_t vertices = uint32_t(*std::max_element(indices, indices + count)) + 1;
    SyncDeferred();
    uint32_t copied = mem::VirtualFromPhysical(R32(UpVertices(indices, count * 2) + kResData));
    auto stream = g_state.streams[0];
    uint32_t base_vertex = g_state.base_vertex;
    g_state.base_vertex = 0;  // UP indices address the supplied array, not the bound index buffer.
    g_state.streams[0] = {UpVertices(HostPtr(a[3]), vertices * stride), stride};
    CountTarget(false, count);
    g_backend->Draw(g_state, prim, 0, count, static_cast<const uint8_t*>(HostPtr(copied)));
    g_state.streams[0] = stream;
    g_state.base_vertex = base_vertex;
    return 0;
}
#endif

static void DrawPushedPrimitive(uint32_t prim, const std::vector<uint32_t>& vertices) {
    uint32_t stride = VertexBytes();
    uint32_t bytes = uint32_t(vertices.size()) * 4;
    if (stride && bytes % stride == 0) {
        DrawUp(prim, bytes / stride, vertices.data(), stride);
    } else {
        LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
    }
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    uint32_t end = a[0];
    for (uint32_t p = g_push_begin; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p);
        uint32_t method = head & 0x1FFC;
        uint32_t n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4;
            uint32_t v = R32(p);
            if (m == kMInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kMBeginEnd) {
                if (m >> 2 < g_state.simple.size()) g_state.simple[m >> 2] = v;
            } else if (v) {
                prim = v;
                vertices.clear();
            } else if (prim) {
                DrawPushedPrimitive(prim, vertices);
                prim = 0;
            }
        }
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// Begin(prim): opens an immediate primitive, as the library's push of the begin does; the SetVertexData calls and
// the commands pushed until End give its vertices
HLE_STDCALL(D3DDevice_Begin, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    ApplyImmediateMethod(kMBeginEnd, a[0]);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
