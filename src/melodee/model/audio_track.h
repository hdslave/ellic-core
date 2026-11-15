// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/input_config.h"
#include "melodee/model/limits.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

// Freeze plan F3, audio tracks: an AUDIO track, as on the MPC 3, records its
// input into the song and plays it back from there. What it recorded sits on
// its Song lane as regions (Song::regions, model/project.h): each one plays a
// part of one take, a WAV file in "<project> Audio/" beside the project. Its
// pads play nothing and it has no patterns, as on the MPC.
//
// A take is recorded once through, between the loop's bars or from the cursor
// until Stop; recording again over a region replaces that part of it (the rest
// stays). Each take gets a file of its own, so Undo brings an old one back.
// Frames are counted at 44.1 kHz, as the project counts everything; a take's
// file is at the device's rate as it was recorded.
namespace melodee {

// What the track hears of its input, as the MPC's audio track Monitor: Off never, In always, Auto while it is armed
// or recording (its regions otherwise), Merge its input and its regions together.
enum class TrackMonitor : uint8_t { Off, In, Auto, Merge, Count };
inline const char* track_monitor_name(TrackMonitor m) {
    static const char* const names[] {"Off", "In", "Auto", "Merge"};
    return names[unsigned(m) % unsigned(TrackMonitor::Count)];
}

// The sources an audio track records from, in the order K1 turns through them: the inputs and USB-C (Melodee's own
// mix and tracks are Resample's and the Track source's, on Sample Record).
constexpr InputSource audio_sources[] {InputSource::Rear, InputSource::RearL, InputSource::RearR, InputSource::Mic,
                                       InputSource::Usb, InputSource::UsbL, InputSource::UsbR};
constexpr unsigned audio_source_count = sizeof(audio_sources) / sizeof(audio_sources[0]);
inline unsigned audio_source_index(InputSource s) {
    for (unsigned n = 0; n < audio_source_count; ++n) if (audio_sources[n] == s) return n;
    return 0;
}

// A source as the audio track's cards and chips name it.
inline const char* audio_source_label(InputSource s) {
    switch (s) {
    case InputSource::Rear: return "Rear L+R";
    case InputSource::Usb: return "USB L+R";
    default: return input_source_name(s);
    }
}

// An audio track's own settings (AUDT): where it records from and what it monitors.
struct AudioTrackSettings {
    InputSource source = InputSource::Rear;
    TrackMonitor monitor = TrackMonitor::Auto;
    bool operator==(const AudioTrackSettings& o) const { return source == o.source && monitor == o.monitor; }
};

// Warp's algorithms: Stretch keeps the pitch (Tones, the sampler's), Repitch plays it faster or slower.
enum class WarpMode : uint8_t { Stretch, Repitch, Count };
inline const char* warp_mode_name(WarpMode m) { return m == WarpMode::Repitch ? "Repitch" : "Stretch"; }

// One region on an audio track's lane: frames [offset, offset + frames) of take `take`, from `tick` ticks into bar
// `bar` of the song. Its length on the lane is those frames at `tempo`, the tempo it was recorded at (Warp's BPM):
// with Warp on it is stretched to keep that length at any tempo; off, it plays as recorded.
struct AudioRegion {
    uint8_t track = 0;
    uint16_t take = 0;        // its file: audio_take_name(take)
    uint16_t bar = 0, tick = 0;
    uint32_t offset = 0;      // Edit Start: frames of the take skipped
    uint32_t frames = 0;      // what it plays from there (Edit End - Edit Start)
    uint32_t take_frames = 0; // the whole take
    uint16_t tempo = 1200;    // tenths of a BPM
    int16_t gain = 0;         // tenths of a dB, region_gain_min..region_gain_max
    uint32_t fade_in = 0, fade_out = 0; // frames
    bool reverse = false, muted = false;
    bool warp = false;
    WarpMode mode = WarpMode::Stretch;
    int8_t semi = 0;          // -24..24
    int8_t fine = 0;          // cents, -50..50
    bool used() const { return frames != 0; }
    bool operator==(const AudioRegion& o) const {
        return track == o.track && take == o.take && bar == o.bar && tick == o.tick && offset == o.offset && frames == o.frames &&
               take_frames == o.take_frames && tempo == o.tempo && gain == o.gain && fade_in == o.fade_in && fade_out == o.fade_out &&
               reverse == o.reverse && muted == o.muted && warp == o.warp && mode == o.mode && semi == o.semi && fine == o.fine;
    }
};
constexpr int region_gain_min = -600, region_gain_max = 120; // -60 .. +12 dB

// Frames (44.1 kHz) and song ticks at a tempo (tenths of a BPM): a quarter is 600 / tempo seconds.
inline uint64_t frames_to_ticks(uint64_t frames, unsigned tempo) {
    return (frames * std::max(tempo, 1u) * ticks_per_quarter + 300 * 44100) / (600 * 44100);
}
inline uint64_t ticks_to_frames(uint64_t ticks, unsigned tempo) {
    return (ticks * 600 * 44100 + std::max(tempo, 1u) * ticks_per_quarter / 2) / (std::max(tempo, 1u) * ticks_per_quarter);
}
// Where a region starts and how long it is on its lane, in song ticks.
inline uint32_t region_start(const AudioRegion& r, unsigned bar_ticks) { return uint32_t(r.bar) * bar_ticks + r.tick; }
inline uint32_t region_ticks(const AudioRegion& r) { return uint32_t(std::max<uint64_t>(1, frames_to_ticks(r.frames, r.tempo))); }
inline uint32_t region_end(const AudioRegion& r, unsigned bar_ticks) { return region_start(r, bar_ticks) + region_ticks(r); }
// A region placed at song tick `at`.
inline void set_region_start(AudioRegion& r, uint32_t at, unsigned bar_ticks) {
    bar_ticks = std::max(bar_ticks, 1u);
    r.bar = uint16_t(std::min<uint32_t>(at / bar_ticks, song_bars));
    r.tick = uint16_t(at % bar_ticks);
}

// The folder beside a project file that holds its takes ("" without a file), and a take's file name in it.
inline std::string audio_folder(const std::string& project_path) {
    if (project_path.empty()) return {};
    auto slash = project_path.find_last_of('/');
    auto dot = project_path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) dot = project_path.size();
    return project_path.substr(0, dot) + " Audio";
}
// Take numbers run through the project, whatever track recorded them, so a region copied or moved to another track
// plays the same file (files are never written again once recorded).
inline std::string audio_take_name(unsigned take) {
    char name[24];
    std::snprintf(name, sizeof(name), "Take %03u.wav", take);
    return name;
}
// The take a file name holds ("Take 012.wav": 12), or 0.
inline unsigned audio_take_of(const std::string& name) {
    unsigned take = 0;
    char tail[8] {};
    if (std::sscanf(name.c_str(), "Take %u%7s", &take, tail) == 2 && std::string(tail) == ".wav") return take;
    return 0;
}

} // namespace melodee
