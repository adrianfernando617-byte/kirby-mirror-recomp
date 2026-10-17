# recomp_master_misses.toml.frag — AUTO-GENERATED proposal.
# These guest PCs were reached by runtime_dispatch with no generated
# function and were bridged through the interpreter this session.
# A HUMAN reviews these and merges the genuine ones into the binary's
# config; this file is NEVER auto-merged (PRINCIPLES.md "Never
# auto-write game.toml").
#
# Proposal shapes:
#   [[extra_func]]  — a standalone missed function entry.
#   [[jump_table]]  — flagged in a comment when a tight run of
#       consecutive same-mode misses looks like a computed-jump
#       switch's case targets. Sizing the table covers the whole
#       switch in ONE entry; prefer it over the per-case [[extra_func]].
# BIOS PCs (< 0x4000) belong in bios/gba_bios.toml; cart PCs in the
# game's game.toml.
#
# game:  AGB KIRBY AM
# code:  B8KE
# sha1:  274b102b6d940f46861a92b4e65f89a51815c12c

[[extra_func]]
addr = 0x03000060
mode = "thumb"
note = "proposed from self-heal miss-log; bridged x551"

[[extra_func]]
addr = 0x030017F0
mode = "arm"
note = "proposed from self-heal miss-log; bridged x550"

[[extra_func]]
addr = 0x03007D4C
mode = "thumb"
note = "proposed from self-heal miss-log; bridged x24"

[[extra_func]]
addr = 0x03007DA0
mode = "thumb"
note = "proposed from self-heal miss-log; bridged x27"

