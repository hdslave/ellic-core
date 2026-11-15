// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// The auto sampler: a plug-in track's instrument played a key at a time by
// the print engine (shell/print_engine.h, a print host of its own, at SCHED_IDLE),
// each note held, let go and its release kept, written as 24-bit WAVs and an MPC
// keygroup program (shell/xpm_write.h) in Samples/Keygroups/<name>/, which a new
// KEYGROUP track then plays with the plug-in track's patterns and mix
// (shell/auto_sample.cpp). Taken before the track's chain (the keygroup
// track gets the chain) or after it (the keygroup track's chain empty).
namespace melodee {

// What a plug-in track is played over, a note at a time.
struct AutoSampleSettings {
    uint8_t low = 36, high = 96; // the lowest and highest root, MIDI notes (C2, C7)
    uint8_t step = 3;            // semitones between roots
    uint8_t layers = 1;          // velocity layers, 1-4
    float hold = 2.0f;           // seconds each key is held
    float tail = 8.0f;           // seconds of release kept at most
    bool post_fx = false;        // taken after the track's chain (its effects in the samples), else before it
    bool loop = false;           // each sample a loop where the key was held (engine/loop_find.h)
    bool normalize = true;       // one gain for the whole program, its loudest sample at -1 dB (layers keep their balance)
    // Each layer's velocity (the loudest of its range): 127 alone; 63, 127; 42, 84, 127; 31, 63, 95, 127.
    uint8_t velocity(unsigned layer) const {
        const unsigned n = std::clamp<unsigned>(layers, 1, 4);
        return uint8_t(std::clamp<unsigned>((std::min(layer, n - 1) + 1) * 127 / n, 1, 127));
    }
    std::vector<uint8_t> roots() const {
        std::vector<uint8_t> out;
        for (int k = low; k <= high && k <= 127; k += std::max<int>(step, 1)) out.push_back(uint8_t(k));
        return out;
    }
};

// What the auto sampler made: "<folder>/<name>.xpm" and its WAVs beside it, or why not.
struct AutoSampleResult {
    std::string project_path;
    unsigned track = 0;
    std::string program; // the .xpm's path
    std::string name;
    std::string error;   // empty: made
    unsigned samples = 0;
    unsigned loops = 0;  // samples given a loop
    float gain_db = 0;   // the program's gain (normalize)
    bool post_fx = false;
    double seconds = 0;
};

} // namespace melodee
