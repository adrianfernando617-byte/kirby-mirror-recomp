// mod_widescreen.cpp — trusted widescreen plugin for KirbyMirrorRecomp (v3).
//
// Registered with the gbarecomp mod runtime via GBA_MOD_CONSTRUCTOR
// (gbarecomp/docs/MOD_PACKAGES.md); package
// mods/packages/kirby-mirror.display/1.0.0/manifest.toml declares the
// "widescreen" feature and binds the "kirby-mirror.widescreen" plugin.
//
// v3 adds the `wide` margin policy ("carregar assets à frente"): the
// plugin reads the room's own decoded tilemap canvas out of guest RAM
// (struct LevelInfo.unk180 — struct RoomTiledBG, katam decomp
// include/data.h; gCurLevelInfo @ 0x02023530) and serves true world
// tiles for the margin columns via gba::g_ws_tilemap_provider. The room
// canvas is larger than one screen (e.g. the hub foreground is 62x46
// tiles at the time of writing), so the margins reveal real map content
// beyond the native 240px that the GBA could never display.
//
// Serving an entry requires the VRAM tile base the guest remaps canvas
// tile ids onto (each streaming layer lands its tileset at a different
// VRAM offset; the guest copy code only XORs flip bits). The plugin
// CALIBRATES live: per hardware BG layer, sample visible screenblock
// entries against candidate canvas tile deltas from {0,·0x100,·0x200,
// 0x300,0x400,0x600,0x800}; a consistent ≥75% match across samples arms
// that layer with the matched canvas + delta. Re-armed whenever the
// canvas tilemap pointer (room change) or canvas size changes, and
// whenever the layer was previously un-armed.
//
// Fail-closed: an un-calibrated layer falls back to the vanilla tile
// ring sampling (v1 look) via kWsTilemapKeepWrapped — never garbage.

#include <cstdlib>
#include <cstring>

#include "runtime_arm.h"  // g_runtime_fn_entry_hook, bus_read_u16/u32 (extern "C")
#include "gba_ppu.h"      // gba::g_ws_bg_x_provider, gba::g_ws_pillarbox,
                          // gba::g_ws_tilemap_provider (action codes 0/1/2)

#include "mod_runtime.h"

namespace {

// ── guest addresses (katam decomp; gCurLevelInfo @ 0x02023530) ──────
constexpr uint32_t kLevelInfoBase = 0x02023530u;
constexpr uint32_t kIoBase = 0x04000000u;
constexpr uint32_t kRegBgcnt = kIoBase + 0x08u;       // BG0CNT..BG3CNT +2/l
constexpr uint32_t kRegBgScroll = 0x03003680u;        // gBgScrollRegs[4][2]
// struct RoomTiledBG (verified live): +0x18 = const u16 *tilemap
constexpr uint32_t kCanvasFg = kLevelInfoBase + 0x180u;          // [0]
constexpr uint32_t kCanvasNext = kLevelInfoBase + 0x180u + 0x24u; // [1]
constexpr uint32_t kBackgrounds = kLevelInfoBase + 0xC0u;        // [4] len 0x40
constexpr uint32_t kBgScrollXOff = 0x2Eu;
constexpr uint32_t kBgScrollYOff = 0x30u;

constexpr uint32_t kVBlankIntrPc = 0x08152388u;  // gf_VBlankIntr entry

constexpr int kDefaultWidth = 304;
constexpr int kMaxWidth = 896;  // engine ceiling: kMaxRenderWidth = 240 + 2*328
constexpr int kNativeWidth = 240;

// Candidate calibration deltas removed in v4 (verbatim DMA model).

enum class MarginMode { kWrap, kClamp, kBlack, kWide };

// ── struct-Binding model (v4) ───────────────────────────────────────
// sub_08153184 (the guest copier) fills the DISPLAYED screenblock ring
// verbatim (DMA, flip-XOR aside) from each struct Background's own
// source canvas. struct Background (gCurLevelInfo.unkC0[3], stride 0x40):
//   +0x10 source pointer (EWRAM decoded canvas or ROM matrix)
//   +0x14 source stride in tiles   +0x16 source height in tiles
//   +0x2E flags — flags&3 = hardware BG layer this struct feeds
//   +0x30 scrollX, +0x32 scrollY (pixels; the ring's page origin)
// Ring grid (row k, col c) == canvas (scrollY>>3 + k, scrollX>>3 + c),
// entries verbatim, no tile-id delta (verified live in-room: 31/31).
struct WideCanvas {
    uint16_t width;    // tiles
    uint16_t height;   // tiles
    uint32_t tilemap;  // guest address of the decoded canvas, row-major
};

struct Calib {
    bool armed;
    WideCanvas canvas;
};

// struct flags that switches the copier to a CHUNK-TABLE source layout
// (canvas cut into 32-col pages): flat slicing would be wrong there.
constexpr uint16_t kFlagChunked = 0x0040u;

static Calib g_calibs[4];   // per hardware BG layer
static MarginMode g_mode = MarginMode::kWrap;
static int g_left = 0;      // native X=0 within the extended canvas
bool g_ws_debug = false;    // diagnostics gate (env GBARECOMP_WS_DEBUG)

uint16_t canvas_tile_at(const WideCanvas& c, int col, int row) {
    if (c.width == 0u || c.tilemap == 0u) return 0u;
    const int clamp_col = (col < 0) ? 0 :
        (col >= static_cast<int>(c.width) ? static_cast<int>(c.width) - 1 : col);
    return bus_read_u16(c.tilemap +
        2u * static_cast<uint32_t>(row * c.width + clamp_col));
}

// Verify that the hardware layer's ring really holds this canvas at the
// page origin (fail closed: a wrong scene must not leak into margins).
// The struct preserves the last copy's placement: dest ring window starts
// at (unk24, unk22) with extent (unk26 cols?, unk28 rows), mapping
// ring rows [unk24+k] == canvas rows [(prevScrollY>>3) + k - unk24].
bool verify_ring_binding(uint8_t layer, const WideCanvas& canvas,
                         uint16_t scroll_x, uint16_t scroll_y,
                         uint16_t d_row, uint16_t d_col) {
    const uint16_t cnt = bus_read_u16(kRegBgcnt + 2u * layer);
    if (canvas.width == 0u || canvas.tilemap == 0u) return false;
    if (((cnt >> 14) & 3u) != 0u) return false;  // ring must be 32x32
    const uint32_t sb_addr =
        0x06000000u + ((cnt >> 8) & 31u) * 0x800u;
    const int row0 = (static_cast<int>(scroll_y) >> 3) - static_cast<int>(d_row);
    const int col0 = (static_cast<int>(scroll_x) >> 3) - static_cast<int>(d_col);
    int ok = 0, total = 0;
    for (uint32_t k = 0; k < 32 && total < 32; k += 3) {
        for (uint32_t c0 = 0; c0 < 32 && total < 32; c0 += 3) {
            const int crow = row0 + static_cast<int>(k);
            const int ccol = col0 + static_cast<int>(c0);
            if (crow < 0 || crow >= static_cast<int>(canvas.height)) continue;
            if (ccol < 0 || ccol >= static_cast<int>(canvas.width)) continue;
            const uint16_t cv = canvas_tile_at(canvas, ccol, crow) & 0x3FFu;
            if (cv == 0u) continue;  // transparent can't verify
            const uint16_t ring_entry = bus_read_u16(sb_addr +
                2u * static_cast<uint32_t>((((d_row + k) & 31) << 5) | ((d_col + c0) & 31))) & 0x3FFu;
            ++total;
            if (ring_entry == cv) ++ok;
        }
    }
    return total >= 8 && ok * 4u >= total * 3u;
}

// ── render-time cache ───────────────────────────────────────────────
// The PPU runs inside the CPU's MMIO catch-up: a provider bus_read while
// the PPU is mid-render re-enters render_scanline and overflows the stack
// (observed with 896-wide margins). So the provider reads ONLY cached
// state, all sampled once per frame in vblank_policy_hook.
//
// Band geometry: covers the canvas columns/rows the extended view can
// reach this frame (≤ ceil(896/8)+1 cols × ceil(160/8)+1 rows).
struct LayerFrame {
    uint16_t cnt;
    uint16_t scroll_x, scroll_y;
    bool band_valid;
    int16_t band_col0, band_cols;
    int16_t band_row0, band_rows;
    uint16_t entries[116][256];
    int hwx_min, hwx_max;  // diagnostics: provider call range per frame
    unsigned prov_calls;   // diagnostics: provider calls per frame
};
LayerFrame g_frame[4];
constexpr int kBandColsCap = 256;   // full canvas width (mirror coverage)
constexpr int kBandRowsCap = 116;   // full engine height: (240..864)/8 + slack

// Vertical reflection helper: negative rows reflect at the canvas top edge,
// rows beyond the bottom edge at the bottom — margin rows read like a wall.
int vmirror(int row, int h) {
    if (h <= 0) return 0;
    if (row < 0) {
        const int span = 2 * h;
        int r = (-row) % span;
        if (r >= h) r = span - 1 - r;  // fall back inside the top edge
        return r;
    }
    if (row >= h) {
        const int span = 2 * h;
        const int d = (row - h) % span;
        return d < h ? (h - 1 - d) : (d - h);
    }
    return row;
}

int band_entry(const LayerFrame& f, int col, int row) {
    const int ci = col - f.band_col0;
    const int ri = row - f.band_row0;
    if (ci < 0 || ci >= f.band_cols || ri < 0 || ri >= f.band_rows)
        return -1;  // outside cached band (should never happen)
    return f.entries[ri][ci];
}

        // Sample per-layer register state and the armed canvas band once per frame
// (VBlank context: bus reads are safe there — no render is in progress).
void refresh_frame_cache(void) {
    // (re)bind hardware BG layers from the three struct Backgrounds; scroll
    // and canvas come from the STRUCT, not from layer arithmetic.
    static unsigned s_grind_frames = 0;
    for (int s = 0; s < 3; ++s) {
        const uint32_t sbase = kBackgrounds + 0x40u * s;
        const uint16_t flags = bus_read_u16(sbase + 0x2Eu);
        const uint32_t src = bus_read_u32(sbase + 0x10u);
        const uint16_t cw = bus_read_u16(sbase + 0x14u);
        const uint16_t ch = bus_read_u16(sbase + 0x16u);
        const int hwL = flags & 3;
        Calib& calib = g_calibs[hwL];
        const bool valid = src != 0u && src != 0xFFFFFFFFu &&
                           cw != 0u && cw <= 256u && ch != 0u && ch <= 256u &&
                           (flags & kFlagChunked) == 0u;
        const bool changed = !calib.armed ||
            calib.canvas.tilemap != src ||
            calib.canvas.width != cw || calib.canvas.height != ch;
        if (!valid) {
            if (calib.armed && g_ws_debug)
                std::fprintf(stderr, "[ws-debug] BG%d UNARMED (src invalid)\n", hwL);
            calib.armed = false;
            continue;
        }
        calib.canvas = WideCanvas{cw, ch, src};
        uint16_t scx = bus_read_u16(sbase + 0x30u);
        uint16_t scy = bus_read_u16(sbase + 0x32u);
        if (changed || !calib.armed) {
            const uint16_t d_row = bus_read_u16(sbase + 0x24u);
            const uint16_t d_col = bus_read_u16(sbase + 0x22u);
            if (verify_ring_binding(static_cast<uint8_t>(hwL), calib.canvas,
                                    scx, scy, d_row, d_col)) {
                calib.armed = true;
                if (g_ws_debug)
                    std::fprintf(stderr,
                        "[ws-debug] BG%d ARMED canvas %ux%u scr 0x%08x ring %ux%u\n",
                        hwL, cw, ch, src, cw, ch);
            } else {
                if (g_ws_debug && (++s_grind_frames % 50u) == 1u)
                    std::fprintf(stderr,
                        "[ws-debug] BG%d bind struct=%d flags=0x%04x src=0x%08x %ux%u "
                        "scroll=(%u,%u) verify FAIL\n",
                        hwL, s, flags, src, cw, ch, scx, scy);
                calib.armed = false;
            }
        }
        // kill the [i]-slot frame scroll until the band fill below
        (void)scx; (void)scy;
    }

    static unsigned s_dbg_frames = 0;
    if (g_ws_debug && (++s_dbg_frames % 60u) == 0u) {
        for (int l = 0; l < 4; ++l) {
            std::fprintf(stderr,
                "[ws-frame] f=%u authored=0x%x BG%d armed=%d cnt=0x%04x scroll=(%u,%u) "
                "band_c=%d..%d hwx=%d..%d calls=%u\n",
                s_dbg_frames, gba::g_ws_authored_margin_layers, l,
                g_calibs[l].armed ? 1 : 0, g_frame[l].cnt,
                g_frame[l].scroll_x, g_frame[l].scroll_y,
                g_frame[l].band_col0, g_frame[l].band_cols,
                g_frame[l].hwx_min, g_frame[l].hwx_max,
                g_frame[l].prov_calls);
        }
    }
    for (int l = 0; l < 4; ++l) {
        LayerFrame& f = g_frame[l];
        f.hwx_min = 0x7FFF; f.hwx_max = -0x8000; f.prov_calls = 0;
        f.cnt = bus_read_u16(kRegBgcnt + 2u * l);
        const Calib& c = g_calibs[l];
        if (!c.armed || c.canvas.width == 0u || c.canvas.tilemap == 0u) {
            f.band_valid = false;
            continue;
        }
        // scroll from the struct slot that feeds this hardware layer
        // (re-read the three bindings and remember their hwL assignment).
        uint16_t scx = 0, scy = 0;
        for (int s = 0; s < 3; ++s) {
            const uint32_t sbase = kBackgrounds + 0x40u * s;
            if ((bus_read_u16(sbase + 0x2Eu) & 3u) == l) {
                scx = bus_read_u16(sbase + 0x30u);
                scy = bus_read_u16(sbase + 0x32u);
                break;
            }
        }
        f.scroll_x = scx;
        f.scroll_y = scy;
        // Band covers the FULL canvas columns (mirror needs both edges) and
        // the rows the vertical margins can reach (v5 fill: host top/bottom
        // extras). Negative/extra rows are pre-mirrored into the band so the
        // provider only slices it — no reflection math at render time.
        const int extra_top = static_cast<int>(gba::g_ws_extra_top);
        const int extra_bottom = static_cast<int>(gba::g_ws_extra_bottom);
        const int col0 = 0;
        const int col1 = static_cast<int>(c.canvas.width) - 1;
        const int row0 = static_cast<int>(scy) >> 3;
        f.band_row0 = static_cast<int16_t>(row0 - (extra_top + 7) / 8);
        f.band_rows = static_cast<int16_t>(
            ((extra_top + 160 + extra_bottom) / 8 + 3));
        f.band_col0 = static_cast<int16_t>(col0);
        f.band_cols = static_cast<int16_t>(col1 - col0 + 1);
        f.band_valid = true;
        for (int r = 0; r < f.band_rows; ++r) {
            const int mrow = vmirror(f.band_row0 + r,
                                     static_cast<int>(c.canvas.height));
            uint32_t addr = c.canvas.tilemap +
                2u * static_cast<uint32_t>(mrow * c.canvas.width + f.band_col0);
            for (int cc = 0; cc < f.band_cols; ++cc, addr += 2u)
                f.entries[r][cc] = bus_read_u16(addr);
        }
    }
}

// PPU tilemap provider: build a GBA entry for a margin column.
// Action codes: 1 = replace (returns *out_entry), 2 = keep vanilla wrap,
// 0 = unavailable (pixel skipped).
int wide_tilemap_provider(int bg, int hw_x, int screen_y, uint16_t* out_entry) {
    const int layer = bg & 3;
    const Calib& calib = g_calibs[layer];
    LayerFrame& f = g_frame[layer];  // cached; NO bus reads here
    if (hw_x < f.hwx_min) f.hwx_min = hw_x;
    if (hw_x > f.hwx_max) f.hwx_max = hw_x;
    ++f.prov_calls;
    if (!calib.armed) {
        // Layers without a verified canvas binding: foreground-ish (higher
        // priority than any armed layer) would repeat their ring 256px-wide
        // across the margins — skip the pixel so armed layers fill; anything
        // behind keeps vanilla wrap (mostly hidden below armed layers).
        const unsigned my_prio = f.cnt & 3u;
        for (int other = 0; other < 4; ++other) {
            if (other == layer || !g_calibs[other].armed) continue;
            if (my_prio < (g_frame[other].cnt & 3u))
                return 0;  // margins: let armed layer fill
        }
        return 2;  // vanilla wrap fallback
    }

    // Signed math: hw_x may be negative (left margin). Coordinates beyond
    // the canvas edges mirror the edge region (last tiles reflected), so
    // the margin reads like a wall/curtain, not a repeated copy of the map.
    const int col = (static_cast<int>(f.scroll_x) + hw_x) >> 3;
    const int row = (static_cast<int>(f.scroll_y) + screen_y) >> 3;
    int cc = col;
    const int w = static_cast<int>(calib.canvas.width);
    if (w > 0) {
        if (cc < 0) {
            // left mirror: -1 → 0, -2 → 1 … (reflect at the left border)
            const int span = 2 * w;
            cc = (-cc) % span;                // 1..span-1 …
            if (cc >= w) cc = span - 1 - cc;  // fall back inside left part
            if (cc < 0) cc = 0;
        } else if (cc >= w) {
            // right mirror: W → W-1, W+1 → W-2 … cyclic when very wide
            const int span = 2 * w;
            const int d = (cc - w) % span;    // 0..2w-1
            cc = d < w ? (w - 1 - d) : (d - w);
        }
    }
    const int entry = band_entry(f, cc, row);
    if (entry < 0) {
        // Mirror coords that fall outside the cached band (band clamps to
        // the canvas edge columns) — degrade to the exact edge column.
        const int ec = (col < 0) ? 0 : (w > 0 ? w - 1 : 0);
        const int e2 = band_entry(f, ec, row);
        if (e2 < 0) return 2;  // cache miss → vanilla behaviour
        if (e2 == 0) return 0; // canvas transparent → skip (BG behind)
        *out_entry = static_cast<uint16_t>(e2);
        return 1;
    }
    if (entry == 0) return 0;   // canvas transparent → skip (BG behind)

    // Verbatim DMA model: the copier stores the source entry as-is, so the
    // returned entry keeps its own palette bank + flip bits.
    *out_entry = static_cast<uint16_t>(entry);
    return 1;
}

// ── per-frame sync (hooked at gf_VBlankIntr entry) ──────────────────
// g_ws_debug moved to top-level globals
void vblank_policy_hook(uint32_t entry_pc) {
    if (entry_pc != kVBlankIntrPc) return;
    gba::g_ws_pillarbox = (g_mode == MarginMode::kBlack) ? 1 : 0;
    if (g_mode != MarginMode::kWide) return;

    // Refresh the render-time cache BEFORE the PPU draws this frame's
    // margins (providers must not touch the bus mid-render). Binding,
    // verification and the canvas band all happen here.
    refresh_frame_cache();
}

int requested_width() {
    const char* value = gba_mod_option_value(
        "kirby-mirror.display", "widescreen", "width");
    if (!value) return kDefaultWidth;
    const int parsed = std::atoi(value);
    if (parsed <= 0 || parsed > kMaxWidth) return kDefaultWidth;
    return parsed;
}

MarginMode parse_margin_mode() {
    const char* value = gba_mod_option_value(
        "kirby-mirror.display", "widescreen", "margins");
    if (!value) return MarginMode::kWide;  // v3 default: real map content
    if (std::strcmp(value, "clamp") == 0) return MarginMode::kClamp;
    if (std::strcmp(value, "black") == 0) return MarginMode::kBlack;
    if (std::strcmp(value, "wrap") == 0) return MarginMode::kWrap;
    return MarginMode::kWide;
}

int clamp_bg_x_provider(int /*layer*/, int x, int /*screen_y*/, int* out_hw_x) {
    if (x >= g_left && x < g_left + kNativeWidth) return 0;
    *out_hw_x = (x < g_left) ? g_left : (g_left + kNativeWidth - 1);
    return 1;
}

void activate_widescreen(void) {
    const int width = requested_width();
    g_left = (width - kNativeWidth) / 2;
    g_mode = parse_margin_mode();
    // Declare all regular BG layers as provider-authored in the margins:
    // without this the guest's WIN0/WIN1 window masks gate the margin
    // pixels BEFORE the tilemap provider runs (armed layers would never be
    // asked for extended columns). Providers still fail closed per layer.
    gba::g_ws_authored_margin_layers = 0xF;
    g_ws_debug = (std::getenv("GBARECOMP_WS_DEBUG") != nullptr);

    gba_mod_set_view_width(width);
    // v5 fill: the adaptive (resize-driven) view owns the live geometry —
    // window aspect drives both the width and the height (engine vertical
    // extension, opt-in via opts.max_resize_view_height). The fixed width
    // above remains only the boot-time base.
    gba_mod_set_adaptive_view_enabled(1);
    gba::g_ws_bg_x_provider = nullptr;
    gba::g_ws_tilemap_provider = nullptr;
    switch (g_mode) {
        case MarginMode::kClamp:
            gba::g_ws_bg_x_provider = &clamp_bg_x_provider;
            gba::g_ws_bg_x_provider_layers = 0xFu;
            gba::g_ws_pillarbox = 0;
            break;
        case MarginMode::kBlack:
            gba::g_ws_pillarbox = 1;
            break;
        case MarginMode::kWide:
            gba::g_ws_tilemap_provider = &wide_tilemap_provider;
            gba::g_ws_pillarbox = 0;
            break;
        case MarginMode::kWrap:
        default:
            gba::g_ws_pillarbox = 0;
            break;
    }
    g_runtime_fn_entry_hook = &vblank_policy_hook;
}

void reset_widescreen(void) {
    g_runtime_fn_entry_hook = nullptr;
    gba::g_ws_bg_x_provider = nullptr;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_tilemap_provider = nullptr;
    gba::g_ws_authored_margin_layers = 0;
    for (int l = 0; l < 4; ++l) g_calibs[l].armed = false;
    gba_mod_set_view_width(0);
    gba_mod_set_adaptive_view_enabled(0);
}

}  // namespace

GBA_MOD_CONSTRUCTOR(register_kirby_display_mods) {
    gba_mod_register_reset_callback(reset_widescreen);
    gba_mod_register_activation_plugin("kirby-mirror.widescreen",
                                       activate_widescreen);
}
