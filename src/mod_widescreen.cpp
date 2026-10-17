// mod_widescreen.cpp — trusted widescreen plugin for KirbyMirrorRecomp (v2).
//
// Registered with the gbarecomp mod runtime via GBA_MOD_CONSTRUCTOR
// (see ../gbarecomp/docs/MOD_PACKAGES.md). The built-in package
// mods/packages/kirby-mirror.display/1.1.0/manifest.toml declares the
// "widescreen" feature and binds it to the "kirby-mirror.widescreen"
// plugin id below.
//
// Activation happens on the runtime's commit pass (mods_packages "Play"
// flow): the callback requests a fixed extended logical width through
// gba_mod_set_view_width(). RunOptions::max_view_width, set in
// src/main.cpp, remains the capability ceiling — the plugin request is
// clamped to it. Reset callbacks restore the native 240px view.
//
// Fase 2 adds a margin *presentation policy*, chosen by the new
// "margins" feature option and owned entirely by this trusted plugin
// (the guest ROM stays byte-identical):
//
//   wrap  (default) — vanilla behavior: the guest PPU keeps sampling its
//          own 32x32 tile ring beyond column 239. KAM rooms are one
//          screen wide with a locked camera, so this repeats the room's
//          far edge; audited against title/file-select/game-select/
//          cutscene/field rooms and reads as scenery continuation.
//
//   clamp — margins continue the nearest native column exactly, via
//          gba::g_ws_bg_x_provider (host-side, read-only). Kills the
//          wrap seam on rooms whose left/right borders disagree.
//
//   black — gba::g_ws_pillarbox = 1: solid margins (menus, screenshots).
//
// The policy is reasserted once per guest frame from a function-entry
// hook on the game's own VBlank routine (gf_VBlankIntr, 0x08152388 per
// generated/symbol_map.cpp), so a savestate load's fail-closed pillarbox
// default cannot strand the wide view. Uses the single-slot
// g_runtime_fn_entry_hook debug hook, which nothing else enables in
// shipped runs (ws_provenance only arms it under its own env flag).

#include <cstdlib>
#include <cstring>

#include "runtime_arm.h"  // g_runtime_fn_entry_hook, bus_read_u16 (extern "C")
#include "gba_ppu.h"      // gba::g_ws_bg_x_provider, gba::g_ws_pillarbox

#include "mod_runtime.h"

namespace {

// 240 native + 32 px per side = 4 extra 8px tile-columns per side.
constexpr int kDefaultWidth = 304;
constexpr int kMaxWidth = 304;  // mirrors RunOptions::max_view_width
// gf_VBlankIntr entry, from KirbyMirrorRecomp/generated/symbol_map.cpp.
constexpr uint32_t kVBlankIntrPc = 0x08152388u;

enum class MarginMode { kWrap, kClamp, kBlack };

MarginMode g_margin_mode = MarginMode::kWrap;
int g_left = 0;  // native X=0 within the extended canvas ((W-240)/2)

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
    if (!value) return MarginMode::kWrap;
    if (std::strcmp(value, "clamp") == 0) return MarginMode::kClamp;
    if (std::strcmp(value, "black") == 0) return MarginMode::kBlack;
    return MarginMode::kWrap;
}

// bg_x provider: continue the nearest native column into the margins.
// Returning 0 (action none) for native columns keeps the vanilla path.
// The clamp result is independent of hofs/scanline, so no per-frame
// table refresh is required.
int clamp_bg_x_provider(int /*layer*/, int x, int /*screen_y*/, int* out_hw_x) {
    if (x >= g_left && x < g_left + 240) return 0;
    *out_hw_x = (x < g_left) ? g_left : (g_left + 239);
    return 1;
}

// Per-frame hook: fires on every recompiled function entry; the PC filter
// keeps the cost to one compare on the non-VBlank path. Reasserts the
// margin policy so savestate loads (which reset g_ws_pillarbox to the
// fail-closed default) cannot strand the wide view.
void vblank_policy_hook(uint32_t entry_pc) {
    if (entry_pc != kVBlankIntrPc) return;
    gba::g_ws_pillarbox = (g_margin_mode == MarginMode::kBlack) ? 1 : 0;
}

void activate_widescreen(void) {
    const int width = requested_width();
    g_left = (width - 240) / 2;  // runtime centers the extended canvas
    g_margin_mode = parse_margin_mode();

    gba_mod_set_view_width(width);

    switch (g_margin_mode) {
        case MarginMode::kClamp:
            gba::g_ws_bg_x_provider = &clamp_bg_x_provider;
            gba::g_ws_bg_x_provider_layers = 0xFu;  // all BG layers
            gba::g_ws_pillarbox = 0;
            break;
        case MarginMode::kBlack:
            gba::g_ws_bg_x_provider = nullptr;
            gba::g_ws_pillarbox = 1;
            break;
        case MarginMode::kWrap:
        default:
            gba::g_ws_bg_x_provider = nullptr;
            gba::g_ws_pillarbox = 0;
            break;
    }
    g_runtime_fn_entry_hook = &vblank_policy_hook;
}

void reset_widescreen(void) {
    g_runtime_fn_entry_hook = nullptr;
    gba::g_ws_bg_x_provider = nullptr;
    gba::g_ws_pillarbox = 0;
    gba_mod_set_view_width(0);
    gba_mod_set_adaptive_view_enabled(0);
}

}  // namespace

GBA_MOD_CONSTRUCTOR(register_kirby_display_mods) {
    gba_mod_register_reset_callback(reset_widescreen);
    gba_mod_register_activation_plugin("kirby-mirror.widescreen",
                                       activate_widescreen);
}
