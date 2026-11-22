// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

// Jam capture, after Melodee FM-1's jam to song. While the transport plays outside Song, each bar is logged: the
// pattern each used track plays (or nothing), which tracks are not heard (Mute, or another track's Solo), the tempo,
// and a sequence that took over on that bar. TAKE JAM on Song lays the log after the song's end: a clip for each run
// of the same pattern on a lane (muted where the track was not heard), a marker where a sequence took over, and the
// tempo lane where the tempo changed. The log is one run: PLAY outside Song starts a new one.
namespace melodee {

constexpr uint8_t jam_silent = 255; // JamBar::pattern: the track played nothing that bar

struct JamBar {
    std::array<uint8_t, track_count> pattern; // jam_silent: nothing
    uint16_t muted = 0;  // a bit a track: not heard
    uint16_t tempo = 0;  // tenths of a BPM; 0: not known
    int16_t scene = -1;  // a sequence that took over on this bar
    JamBar() { pattern.fill(jam_silent); }
};

struct JamLog {
    std::vector<JamBar> bars;
    bool full() const { return bars.size() >= song_bars; }
};

struct JamTake {
    unsigned at = 0;       // the song's bar the jam starts on
    unsigned bars = 0;     // bars of it laid out
    unsigned clips = 0;
    unsigned left_out = 0; // bars past Song's room (999 bars, 256 clips)
};

// The log after the song's end (bar 1 when the song is empty). A run of a pattern with no notes leaves no clip.
inline JamTake jam_to_song(Project& project, const PatternBank& bank, const JamLog& log) {
    auto& song = project.song;
    JamTake take;
    take.at = song.length();
    const unsigned logged = unsigned(log.bars.size());
    unsigned limit = take.at < song_bars ? std::min(logged, song_bars - take.at) : 0;

    // Each lane's runs: the same pattern, heard or not, bar after bar.
    std::vector<Clip> clips;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!project.tracks[t].used()) continue;
        for (unsigned b = 0; b < limit;) {
            const uint8_t p = log.bars[b].pattern[t];
            const bool muted = log.bars[b].muted >> t & 1;
            unsigned e = b + 1;
            while (e < limit && log.bars[e].pattern[t] == p && bool(log.bars[e].muted >> t & 1) == muted) ++e;
            if (p != jam_silent && !bank.at[t][p % pattern_count].empty())
                clips.push_back({uint8_t(t), uint8_t(p % pattern_count), uint16_t(take.at + b), uint16_t(e - b), muted});
            b = e;
        }
    }
    // Song's clips run out: the jam ends where the first clip with no room would begin.
    std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.bar < b.bar; });
    const unsigned room = song_clips - std::min<unsigned>(song.count, song_clips);
    if (clips.size() > room) {
        limit = std::min(limit, unsigned(clips[room].bar) - take.at);
        clips.resize(room);
    }
    for (auto clip : clips) {
        if (clip.bar >= take.at + limit) continue;
        clip.bars = uint16_t(std::min<unsigned>(clip.end(), take.at + limit) - clip.bar);
        if (song.place(clip)) ++take.clips;
    }
    for (unsigned b = 0; b < limit; ++b) {
        const int n = log.bars[b].scene;
        const unsigned bar = take.at + b;
        if (n < 0 || song.marker_count == song.markers.size() || song.marker_at(bar) >= 0) continue;
        Marker marker;
        marker.bar = uint16_t(bar);
        const auto& scene = project.scenes[unsigned(n) % pattern_count];
        if (scene.name[0]) std::copy(std::begin(scene.name), std::end(scene.name), std::begin(marker.name));
        else std::snprintf(marker.name, sizeof(marker.name), "SEQ %u", unsigned(n) % pattern_count + 1);
        song.markers[song.marker_count++] = marker;
    }
    // The tempo lane: a step where the jam's tempo differs from what the song would play there.
    unsigned before = unsigned(std::lround(song.tempo.at(double(take.at), project.tempo)));
    for (unsigned b = 0; b < limit; ++b) {
        const unsigned tempo = log.bars[b].tempo;
        if (!tempo || tempo == before) continue;
        song.tempo.set(take.at + b, tempo, false);
        before = tempo;
    }
    take.bars = limit;
    take.left_out = logged - limit;
    return take;
}

} // namespace melodee
