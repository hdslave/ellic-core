// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// An MPC keygroup program written as an .xpm (the MPC 2 XML that MPC 2 and 3,
// Force and MPC Beats open, and Melodee reads with shell/mpc_program.h): one
// keygroup a key range, each with up to four velocity layers of WAV files
// beside the program. The auto sampler (shell/auto_sample.h) makes them.
namespace melodee {

struct XpmLayer {
    std::string sample;    // the WAV's name beside the program, without ".wav"
    uint8_t root = 60;     // the MIDI note it sounds at its own pitch
    uint8_t low_velocity = 1, high_velocity = 127;
    uint32_t frames = 0;   // the WAV's
};
struct XpmKeygroup {
    uint8_t low = 0, high = 127;
    std::vector<XpmLayer> layers; // four at most, lowest velocity first
};
struct XpmProgram {
    std::string name;
    std::vector<XpmKeygroup> keygroups; // 128 at most, in key order
    double release = 0.05;              // seconds after the key is let go (the samples hold their own release)
};

// The program as .xpm text.
std::string xpm_text(const XpmProgram& program);
// The program written to `path` (beside its WAVs). Empty on success.
std::string write_xpm(const std::string& path, const XpmProgram& program);

// keygroup_seconds()'s inverse (engine/keygroup.h): seconds as the MPC's 0..1 envelope value.
double xpm_envelope_value(double seconds);

} // namespace melodee
