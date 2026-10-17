// mod_widescreen.cpp — trusted widescreen plugin for KirbyMirrorRecomp.
//
// Registered with the gbarecomp mod runtime via GBA_MOD_CONSTRUCTOR
// (see ../gbarecomp/docs/MOD_PACKAGES.md). The built-in package
// mods/packages/kirby-mirror.display/1.0.0/manifest.toml declares the
// "widescreen" feature and binds it to the "kirby-mirror.widescreen"
// plugin id below.
//
// Activation happens on the runtime's commit pass (mods_packages
// "Play" flow): the callback requests a fixed extended logical width
// through gba_mod_set_view_width(). RunOptions::max_view_width, set in
// src/main.cpp, remains the capability ceiling — the plugin request is
// clamped to it. Reset callbacks restore the native 240px view.
//
// Margin *content* is still owned by the guest: with a wider view the
// guest PPU keeps sampling the tile ring beyond column 239, so until
// the authored-margin stage lands the extra columns show whatever the
// ring carries (see docs note in the manifest).

#include <cstdlib>

#include "mod_runtime.h"

namespace {

// 240 native + 32 px per side = 4 extra 8px tile-columns per side.
constexpr int kDefaultWidth = 304;
constexpr int kMaxWidth = 304;  // mirrors RunOptions::max_view_width

int requested_width() {
    const char* value = gba_mod_option_value(
        "kirby-mirror.display", "widescreen", "width");
    if (!value) return kDefaultWidth;
    const int parsed = std::atoi(value);
    if (parsed <= 0 || parsed > kMaxWidth) return kDefaultWidth;
    return parsed;
}

void activate_widescreen(void) {
    gba_mod_set_view_width(requested_width());
}

void reset_widescreen(void) {
    gba_mod_set_view_width(0);
    gba_mod_set_adaptive_view_enabled(0);
}

}  // namespace

GBA_MOD_CONSTRUCTOR(register_kirby_display_mods) {
    gba_mod_register_reset_callback(reset_widescreen);
    gba_mod_register_activation_plugin("kirby-mirror.widescreen",
                                       activate_widescreen);
}
