// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

// What limits a project's samples: memory. A kit pad's sample is read whole into memory (at most kit_sample_seconds
// of it), its frames at 44.1 kHz and a copy at the device's rate; an instrument's files are streamed from the card,
// only each zone's start kept. The MPC has 2 GB and no swap, so a load that would leave less than memory_headroom
// free is refused (shell/sample_library.cpp, shell/sample_library_instruments.cpp). Pure but for memory_free().
namespace melodee {

constexpr unsigned kit_sample_seconds = 60;                // a kit pad's sample: what is read of its file
constexpr uint64_t memory_headroom = uint64_t(256) << 20;  // left free after a load: the app's own work and the system's

// What a kit pad's sample of `frames` frames at `rate` Hz holds once read, and while it is read: its frames at
// 44.1 kHz, left and right as floats; the same at the device's rate where that is another (a copy is made while
// the copies together stay under the library's budget); and its own frames while they are converted, where its
// rate is neither.
inline uint64_t kit_sample_bytes(uint64_t frames, unsigned rate, unsigned device_rate) {
    if (!rate) return 0;
    frames = std::min<uint64_t>(frames, uint64_t(kit_sample_seconds) * rate);
    const uint64_t side = 2 * sizeof(float);
    const uint64_t model = (frames * 44100 + rate - 1) / rate * side;
    const uint64_t device = device_rate && device_rate != 44100 ? (frames * device_rate + rate - 1) / rate * side : 0;
    const uint64_t own = rate != 44100 && rate != device_rate ? frames * side : 0;
    return model + device + own;
}

// Whether a load that takes `wanted` bytes leaves the headroom, with `free` bytes free now (0: the system does not
// say, a host: allowed).
inline bool memory_allows(uint64_t free, uint64_t wanted) {
    return !free || (free >= memory_headroom && wanted <= free - memory_headroom);
}

// The memory free to take now, as the system counts it (MemAvailable); 0 where it does not say (a host).
inline uint64_t memory_free() {
    std::ifstream info("/proc/meminfo");
    std::string name;
    uint64_t kb = 0;
    while (info >> name >> kb) {
        if (name == "MemAvailable:") return kb * 1024;
        std::string rest;
        std::getline(info, rest);
    }
    return 0;
}

} // namespace melodee
