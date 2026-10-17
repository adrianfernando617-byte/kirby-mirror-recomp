// main.cpp — KirbyMirrorRecomp entry point.
//
// Every gbarecomp game binary takes BOTH a BIOS and a ROM at launch
// (see ../gbarecomp/PRINCIPLES.md "BIOS is sacred"). The CLI accepts:
//
//   KirbyMirrorRecomp [--bios <path>] [--rom <path>] [game.toml]
//
// All three are optional on the command line; missing values are
// pulled from game.toml. Hashes are verified before any code runs.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"

void print_usage() {
    std::printf(
        "KirbyMirrorRecomp [--bios <path>] [--rom <path>] "
        "[game.toml]\n"
        "\n"
        "Both BIOS and ROM are required (either via flags or via the\n"
        "[bios] / [rom] sections of game.toml). The runtime refuses\n"
        "to start unless both hash-verify.\n"
        "\n"
        "Default BIOS path: ../gbarecomp/bios/gba_bios.bin\n"
        "Default game config: game.toml (in CWD)\n");
}

int main(int argc, char** argv) {
    std::printf("KirbyMirrorRecomp\n");

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    gbarecomp::RunOptions opts;
    opts.builtin_game_name  = "Kirby & The Amazing Mirror";
    opts.builtin_rom_sha1   = "274b102b6d940f46861a92b4e65f89a51815c12c";
    opts.builtin_rom_crc32  = 0x9F2A3048u;
    opts.mod_game_id        = "kirby-amazing-mirror-us";
    // The mod catalog owns the adaptive view: fail closed to native if the
    // catalog cannot load, and let feature state beat stale config/CLI.
    opts.mod_owns_adaptive_view = true;
    // Capability ceiling for trusted view plugins (see src/mod_widescreen.cpp).
    // 896 = engine storage limit (240 native + 2*328 margins); rooms with
    // wide canvases (up to 242 tiles = 1936px) supply real content.
    opts.max_view_width = 896;
    opts.launcher_region    = "USA";
    opts.launcher_game_config = "game.toml";

    return gbarecomp::run_game(argc, argv, opts);
}
