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
constexpr int kMaxWidth = 304;
constexpr int kNativeWidth = 240;

// Candidate calibration deltas: canvas tile id + delta = VRAM tile id.
constexpr uint16_t kTileDeltas[] = {0x000u, 0x100u, 0x200u, 0x300u,
                                    0x400u, 0x600u, 0x800u};

enum class MarginMode { kWrap, kClamp, kBlack, kWide };

// ── per-layer wide state ────────────────────────────────────────────
struct WideCanvas {
    uint16_t width;    // tiles
    uint16_t height;   // tiles
    uint32_t tilemap;  // guest address of the decoded tilemap, row-major
};

struct Calib {
    bool armed;
    WideCanvas canvas;
    uint16_t tile_delta;  // canvas tile id + delta = VRAM tile id
};

static Calib g_calibs[4];   // per hardware BG layer
static MarginMode g_mode = MarginMode::kWrap;
static int g_left = 0;      // native X=0 within the extended canvas

WideCanvas read_canvas(uint32_t base) {
    WideCanvas c{};
    c.width = static_cast<uint16_t>(bus_read_u16(base));
    c.height = static_cast<uint16_t>(bus_read_u16(base + 2u));
    c.tilemap = bus_read_u32(base + 0x18u);
    if (c.width == 0u || c.width > 256u || c.height > 256u || c.height == 0u)
        c.width = c.height = 0;  // invalid / unset canvas
    return c;
}

uint16_t canvas_tile_at(const WideCanvas& c, int col, int row) {
    if (c.width == 0u || c.tilemap == 0u) return 0u;
    const int wrap_col = ((col % static_cast<int>(c.width)) +
                          static_cast<int>(c.width)) % static_cast<int>(c.width);
    const int wrap_row = ((row % static_cast<int>(c.height)) +
                          static_cast<int>(c.height)) % static_cast<int>(c.height);
    return bus_read_u16(c.tilemap +
                        2u * static_cast<uint32_t>(wrap_row * c.width + wrap_col));
}

// Attempt to arm one hardware layer against one canvas. Returns the tile
// delta via *out_delta on success.
bool calibrate_layer(uint8_t layer, const WideCanvas& canvas,
                     uint16_t* out_delta) {
    if (canvas.width == 0u || canvas.height == 0u || canvas.tilemap == 0u)
        return false;
    const uint16_t cnt = bus_read_u16(kRegBgcnt + 2u * layer);
    if (((cnt >> 14) & 3u) != 0u) return false;  // BG size 0 (32x32) only
    const uint32_t sb_addr =
        0x06000000u + ((cnt >> 8) & 31u) * 0x800u;  // screenblock base
    const uint16_t scroll_x =
        bus_read_u16(kBackgrounds + 0x40u * layer + kBgScrollXOff);
    const uint16_t scroll_y =
        bus_read_u16(kBackgrounds + 0x40u * layer + kBgScrollYOff);

    for (const uint16_t delta : kTileDeltas) {
        int ok = 0, total = 0;
        for (int sy = 0; sy < 160 && total < 32; sy += 56) {
            for (int sx = 8; sx < 240; sx += 48) {
                const int mc = static_cast<int>(
                    (scroll_x + static_cast<uint16_t>(sx)) >> 3);
                const int mr = static_cast<int>(
                    (scroll_y + static_cast<uint16_t>(sy)) >> 3);
                const uint16_t canvas_tile = canvas_tile_at(canvas, mc, mr);
                const uint32_t ring_off =
                    2u * static_cast<uint32_t>(((mr & 31) << 5) | (mc & 31));
                const uint16_t ring_entry = bus_read_u16(sb_addr + ring_off);
                const uint16_t want =
                    static_cast<uint16_t>(((canvas_tile & 0x3FFu) + delta) & 0x3FFu);
                if ((ring_entry & 0x3FFu) == want) ++ok;
                ++total;
            }
        }
        if (ok * 4u >= total * 3u) {  // ≥75% consistent
            *out_delta = delta;
            return true;
        }
    }
    return false;
}

// PPU tilemap provider: build a GBA entry for a margin column.
// Action codes: 1 = replace (returns *out_entry), 2 = keep vanilla wrap,
// 0 = unavailable (pixel skipped).
int wide_tilemap_provider(int bg, int hw_x, int screen_y, uint16_t* out_entry) {
    const int layer = bg & 3;
    const Calib& calib = g_calibs[layer];
    if (!calib.armed) return 2;  // vanilla wrap fallback

    const uint16_t scroll_x =
        bus_read_u16(kBackgrounds + 0x40u * layer + kBgScrollXOff);
    const uint16_t scroll_y =
        bus_read_u16(kBackgrounds + 0x40u * layer + kBgScrollYOff);
    // Signed math: hw_x may be negative (left margin). Canvas wrap is
    // handled by canvas_tile_at's modulo.
    const int col = (static_cast<int>(scroll_x) + hw_x) >> 3;
    const int row = (static_cast<int>(scroll_y) + screen_y) >> 3;
    const uint16_t entry = canvas_tile_at(calib.canvas, col, row);
    if (entry == 0u) return 0;  // canvas says transparent → skip (BG behind).

    const uint16_t tile = static_cast<uint16_t>(
        ((entry & 0x3FFu) + calib.tile_delta) & 0x3FFu);
    *out_entry = static_cast<uint16_t>((entry & 0xFC00u) | tile);
    return 1;
}

// ── per-frame sync (hooked at gf_VBlankIntr entry) ──────────────────
WideCanvas g_last_canvas{0, 0, 0};
void vblank_policy_hook(uint32_t entry_pc) {
    if (entry_pc != kVBlankIntrPc) return;
    gba::g_ws_pillarbox = (g_mode == MarginMode::kBlack) ? 1 : 0;
    if (g_mode != MarginMode::kWide) return;

    const WideCanvas fg = read_canvas(kCanvasFg);
    const bool room_changed =
        (fg.tilemap != g_last_canvas.tilemap ||
         fg.width != g_last_canvas.width ||
         fg.height != g_last_canvas.height);
    if (room_changed) for (int l = 0; l < 4; ++l) g_calibs[l].armed = false;
    g_last_canvas = fg;

    for (int l = 0; l < 4; ++l) {
        Calib& calib = g_calibs[l];
        if (calib.armed) {
            // Keep the canvas reference fresh (layer may re-point).
            calib.canvas = fg;
            continue;
        }
        uint16_t delta = 0;
        if (calibrate_layer(static_cast<uint8_t>(l), fg, &delta)) {
            calib.armed = true;
            calib.canvas = fg;
            calib.tile_delta = delta;
        }
    }
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

    gba_mod_set_view_width(width);
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
