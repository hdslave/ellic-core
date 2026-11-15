// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// An MPC keygroup program written as an .xpm (the MPC 2 XML that MPC 2 and 3,
// Force and MPC Beats open, and Melodee reads with shell/mpc_program.h): one
// keygroup a key range, each with up to four velocity layers of WAV files
// beside the program. The auto sampler (shell/auto_sample.h) makes them. The
// layout follows the MPC's own: 128 instruments (those past the count unused),
// four layers each, the program's pads JSON, pad note and group maps; a loop
// is a slice loop, the slice ending where the loop does.
namespace melodee {

struct XpmLayer {
    std::string sample;    // the WAV's name beside the program, without ".wav"
    uint8_t root = 60;     // the MIDI note it sounds at its own pitch
    uint8_t low_velocity = 0, high_velocity = 127;
    uint32_t frames = 0;   // the WAV's
    // A loop while the key is held: from loop_end back to loop_start (0, 0: none).
    uint32_t loop_start = 0, loop_end = 0;
    bool looped() const { return loop_end > loop_start; }
};
struct XpmKeygroup {
    uint8_t low = 0, high = 127;
    std::vector<XpmLayer> layers; // four at most, lowest velocity first
    bool cycle = false;           // the layers are takes of one hit, played in turn (ZonePlay Cycle), not by velocity
    bool one_shot = false;        // plays out whole however short the press (a drum), else held while the key is
    // Its own release and level, for a fall of two stages (engine/release_fit.h): two keygroups over the same keys
    // and layers, this one's share of the level and how long it takes to fall once the key is let go.
    double release = -1;          // seconds; below 0 the program's
    double gain = 1;              // each layer's Volume
};
struct XpmProgram {
    std::string name;
    std::vector<XpmKeygroup> keygroups; // 128 at most, in key order
    double release = 0.05;              // seconds after the key is let go (the samples hold their own release)
    // Mono and Portamento (engine/keygroup.h mpc_portamento_value()): one note at a time, the pitch sliding to a new
    // key over `glide` seconds (0: none); with `legato` only a key pressed while another is held slides, no new attack.
    bool mono = false, legato = false;
    double glide = 0;
};

// The program as .xpm text.
std::string xpm_text(const XpmProgram& program);
// The program written to `path` (beside its WAVs). Empty on success.
std::string write_xpm(const std::string& path, const XpmProgram& program);

// keygroup_seconds()'s inverse (engine/keygroup.h): seconds as the MPC's 0..1 envelope value.
double xpm_envelope_value(double seconds);

} // namespace melodee
