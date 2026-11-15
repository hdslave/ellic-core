// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/engine.h"
#include "melodee/engine/multisample.h"
#include "melodee/engine/stream_pager.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Freeze plan F3: an audio track's regions as the audio thread plays them
// (model/audio_track.h). Each take is a WAV file read in place by the stream
// pager (engine/stream_pager.h), its frames at its own rate: a region's first
// chunk stays pinned while it is in the song, so it plays at once there; the
// pager pins about a second ahead of a voice. Past what is pinned a voice plays
// silence (an underrun; just after a jump, a wait) and keeps its place.
//
// The song leads: on each tick the engine host names the region sounding there
// and how far into it the song is. A voice started plays on at its own pace
// (a tempo change does not move it); only a jump (Play, the loop coming round,
// a locate) puts it where the song is, fading across.
namespace melodee {

// One region: frames [offset, offset + frames) of `take`, at song ticks [start, end).
struct RegionClip {
    ZonePart* take = nullptr;
    uint32_t start = 0, end = 0;
    uint32_t offset = 0, frames = 0;    // the take's frames, at its own rate
    uint32_t fade_in = 0, fade_out = 0; // the take's frames
    float gain = 1;
    bool reverse = false;
    bool resident = false; // tests: every frame in memory, read without the pager
    uint32_t held = 0;     // frames from where it starts playing pinned for as long as the pack lives
};

// An audio track's regions, by start, and the takes they play. The MIDI thread makes it (shell/audio_tracks.cpp),
// the audio thread plays it (Command::AudioPack) and hands it back to be freed once no voice reads it.
struct AudioPack {
    uint8_t track = 0;
    std::vector<RegionClip> regions;
    std::vector<std::unique_ptr<ZonePart>> takes;
    std::vector<std::vector<float>> owned; // tests: frames held whole (left, then right)
    // The region sounding at song tick `tick`, or nullptr.
    const RegionClip* at(uint32_t tick) const;
    // Tests: a take of `frames` frames at `rate`, `left` and `right` held whole; its index in `takes`.
    unsigned hold_take(std::vector<float> left, std::vector<float> right, float rate);
};

class RegionPlayer {
public:
    static constexpr unsigned voice_count = 4;
    static constexpr float fade_seconds = 0.005f;
    static constexpr double slack = 256; // device frames a voice may be off the song after a jump before it moves
    void set_pager(StreamPager* p) { pager = p; }
    // On a tick of the song: `clip` the region sounding there (nullptr: none), `into` the device frames from its
    // start to this tick (unrounded), `jumped` when the song did not come here from the tick before.
    void follow(const RegionClip* clip, double into, bool jumped, float rate);
    void release(float rate);  // the region left: its voice fades out
    void fade_all(float rate); // its pack replaced: every voice fades out, marked as leaving
    void cut_all();            // every voice closed at once
    bool leaving() const;      // a voice from before fade_all() still sounds
    bool sounding() const;
    const RegionClip* playing() const { return voices[0].clip; }
    void render(float* left, float* right, unsigned frames);
    uint32_t underruns = 0, waits = 0;
    bool quiet = false; // its regions not heard (Monitor In; Auto while it records): the voices go on where they are

private:
    struct Voice {
        const RegionClip* clip = nullptr;
        double pos = 0, step = 1; // frames into the region, and a device frame's worth of them
        int stream = -1;
        bool seeking = false, leaving = false;
        float gain = 1, fade = 0; // a fade: `fade` a frame
    };
    std::array<Voice, voice_count> voices {}; // [0] the main voice
    StreamPager* pager = nullptr;
    void begin(const RegionClip* clip, double pos, bool fade_in, float rate);
    void retire_main(float rate);
    void close(Voice& v);
};

// The engine of an AUDIO track: its regions' voices, in stereo; notes do nothing (its pads play nothing, as on the MPC).
// The engine host renders the player itself, run by run between the song's ticks (as a frozen track's prints), so a
// region starts on its frame whether the tracks render once a block or step by step; the engine's own render adds
// nothing to the track's buffers.
class AudioTrackEngine : public Engine {
public:
    RegionPlayer player;
    void note_on(int, int) override {}
    void note_off(int) override {}
    void all_notes_off() override {}
    void silence() override { player.cut_all(); }
    void set_param(int, int) override {}
    void render(float*, unsigned) override {}
    bool stereo() const override { return true; }
    void render_stereo(float*, float*, unsigned) override {}
    bool sounding() const override { return player.sounding(); }
};

} // namespace melodee
