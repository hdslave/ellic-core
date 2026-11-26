// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/dsp.h"
#include "melodee/engine/multisample.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/model/print.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Freeze plan F2b: a frozen track plays its prints (model/print.h) instead of
// its engine and chain. A print's file is mapped from the card and read as
// memory, as a streamed instrument's zones are: the stream pager pins about a
// second ahead of each voice (engine/stream_pager.h), the first second of each
// print and a chunk at each of its bar lines stay pinned for as long as it is
// frozen, so a launch, a loop's wrap or a Locate to a bar line plays at once.
// The audio thread never reads a frame the pager has not pinned: past it, a
// voice plays silence (an underrun; or, just after a jump to where nothing is
// held, a wait).
namespace melodee {

// One print as the audio thread plays it: its frames interleaved float, read by the pager as one side of `stride`
// floats a frame: the print's two, then a pair for each bus its kit's pad sends ride along for (PrintRecord::sends).
// The shell (shell/print_engine.cpp) maps it; tests hold the frames in memory (resident).
struct PrintClip {
    PrintClip() = default;
    PrintClip(const PrintClip&) = delete;
    PrintClip& operator=(const PrintClip&) = delete;
    ZonePart part;          // left == right: stride * frames floats
    uint32_t frames = 0;    // device frames
    uint8_t stride = 2;     // floats a frame
    uint8_t sends = 0;      // a bit a bus with a pair of its own, in bus order after the print's
    uint32_t pass = 0;      // one pass of the pattern, device frames (a Ring print's tail follows it)
    uint32_t rate = 0;      // the device's rate it was printed at
    PrintTail tail = PrintTail::Loop;
    bool resident = false;  // every frame in memory: read without the pager
    struct Held { uint32_t from = 0, to = 0; }; // floats pinned while the clip lives, [from, to)
    std::vector<Held> held;
    std::vector<float> owned; // resident: the frames
    const float* data() const { return part.left; }
    // Floats safe to read from frame `frame` on without the pager's word (0: none held there).
    uint32_t safe_from(uint32_t frame) const;
    // Tests: interleaved frames held whole (`with_sends`: the pairs after the first two, as `sends` says).
    void hold_whole(std::vector<float> interleaved, uint32_t at_rate, uint32_t pass_frames, PrintTail how, uint8_t with_sends = 0);
    void set_sends(uint8_t bits); // `sends` and the stride they make
};

// A frozen track's prints, a clip a pattern (nullptr: that pattern plays live). The MIDI thread makes it, the audio
// thread plays it (Command::Type::Freeze) and hands it back to be freed once no voice reads it.
struct FreezePack {
    uint8_t track = 0;
    PrintTap tap = PrintTap::PostFx;
    // Freeze plan F2c: the level and pan its Post FX prints were made at (print_mix(), model/print.h): unity and
    // centred since the mixer's strip, the prints played before the track's strip, level and pan as its chain is.
    uint8_t level = 100;
    int8_t pan = 0;
    // Freeze plan F2d: a deep freeze. Its plug-ins are let go once the engine sleeps, so nothing wakes it: hands
    // (pads, keys, the arp) play nothing and a pattern without a print stays silent.
    bool deep = false;
    // Shallow Post FX freezes on plug-in instrument tracks only. The live instrument and its track FX keep
    // running alongside the print, including while it covers their output. Active strip processing also runs for
    // both paths, so keeping the live path does not save its CPU.
    bool keep_live = false;
    std::array<PrintClip*, pattern_count> clips {};
    std::vector<std::unique_ptr<PrintClip>> owned;
};

// A track's print voices, on the audio thread: the main voice plays the print of the pattern playing, where the
// pattern is; up to three more play out what it leaves behind (a Ring print's tail, or a fade).
class PrintPlayer {
public:
    static constexpr unsigned voice_count = 4;
    static constexpr float fade_seconds = 0.005f; // a voice let go mid-sound, or one starting mid-sound
    static constexpr uint32_t slack = 256;        // frames the main voice may be off its place before it moves there
    void set_pager(StreamPager* p) { pager = p; }

    // On each tick of the track's pattern: `clip` the print of the pattern playing (nullptr: none, it plays live),
    // `at` the tick in the pattern, `start` the frames from the pattern's top to that tick as a print counts them
    // (unrounded), `tick` this tick's length in frames.
    void follow(PrintClip* clip, uint32_t at, double start, double tick, float rate, bool replaces_live = false);
    void release(float rate); // the pattern stopped or changed: ordinary Ring tails ring, replacement voices fade
    void fade_all(float rate); // unfrozen: every voice fades out, marked as leaving
    void cut_all();            // every voice closed at once
    bool leaving() const;      // a voice from before fade_all() still sounds
    bool sounding() const;     // any voice
    bool playing() const { return voices[0].clip && !voices[0].releasing; }
    uint32_t position() const { return voices[0].at; }
    // Adds `frames` of every voice into `left` / `right`; a print with pad sends adds them into `send_l` / `send_r`
    // [bus] (nullptr: not heard), and sends_fed() then has a bit for each bus it added to.
    // Replacement voices write their combined audible gain per frame to coverage. An additive Ring tail instead
    // writes its own gain to ring_gain (the maximum when several tails ring), so it can be heard without covering live.
    void render(float* left, float* right, unsigned frames, float* const* send_l = nullptr, float* const* send_r = nullptr,
                float* coverage = nullptr, float* ring_gain = nullptr);
    unsigned sends_fed() const { return fed; }
    uint32_t underruns = 0; // runs that met a frame the pager had not pinned yet
    uint32_t waits = 0;     // the same just after a jump to where nothing is held (counted apart: not late, not yet)

private:
    struct Voice {
        PrintClip* clip = nullptr;
        uint32_t at = 0;
        uint32_t held_end = 0; // frames held at a replacement Loop's end while waiting for the next tick
        int stream = -1;
        bool seeking = false, leaving = false, releasing = false;
        bool replaces_live = false;
        bool ring_tail = false; // a printed tail additive to the next pass
        bool have_last = false;
        float last_l = 0, last_r = 0; // an exhausted replacement voice can fade from its last sample
        float gain = 1, step = 0; // a fade: step a frame
    };
    // One extra fade voice for a replacement launch or repeat when all the Ring voices are still busy.
    std::array<Voice, voice_count + 1> voices {}; // [0] the main voice
    // When another replacement arrives during that fade, mix the displaced voice's pinned audio into this short
    // buffer. Fades can overlap here without taking more pager streams or keeping their clips alive.
    struct FadeFrame { float left = 0, right = 0, coverage = 0, ring_gain = 0; };
    static constexpr unsigned fade_frames = unsigned(fade_seconds * dsp::max_rate) + 1;
    std::array<FadeFrame, fade_frames> recycled {};
    unsigned recycled_at = 0, recycled_count = 0;
    uint64_t expect = 0;  // the main voice's frame on the next tick
    double carry = 0;     // the next tick's place less `expect`, as render() counts ticks (-1..0]
    StreamPager* pager = nullptr;
    unsigned fed = 0;
    void begin(PrintClip* clip, uint32_t frame, bool fade_in, float rate, bool replaces_live);
    void rewind(Voice& v);
    void close(Voice& v);
    void recycle_fade(const Voice& v, float fade);
    void retire_main(bool ring, float rate); // the main voice to another, ringing on or fading
};

} // namespace melodee
