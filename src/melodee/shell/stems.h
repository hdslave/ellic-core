// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Stems: a sample separated into its drums, its vocals and its music (the
// bass and every other instrument), each a file of its own.
//
// The separating is done by a program of its own, SpleeterRT's offline one
// with Deezer's Spleeter model inside it (tools/stems/build.py makes it,
// tools/stems/stems_main.c is its entry). It is kept on the card, not in
// Melodee's binary: 40 MB that would otherwise sit in RAM the whole session.
// The card is mounted noexec, so a job copies it into RAM, runs it there at
// the lowest priority and removes the copy when it ends. The program reads
// one WAV file and writes three beside it (44.1 kHz, 32-bit float, stereo,
// the input's length to the frame); those are written to the card as
// write_wav() keeps a sample.
//
// Measured on the MPC with three threads (four are slower, the audio and
// render threads being on the same cores): 13 s of sound in 15 to 18 s with
// 366 MB at its fullest, 60 s in 48 to 50 s with 682 MB.
namespace melodee {

constexpr unsigned stem_count = 3;
// The stems in the order they land: the word a file's name ends in, and the screen's.
inline const char* stem_name(unsigned n) {
    static const char* const names[stem_count] = {"DRUMS", "VOCALS", "MUSIC"};
    return names[n % stem_count];
}

// What a job takes for `frames` 44.1 kHz frames: memory at its fullest, the
// program and its files in RAM with it, in bytes; and seconds on the MPC.
uint64_t stems_memory(uint32_t frames);
unsigned stems_seconds(uint32_t frames);
// The shortest sample the program is given: under a second there is nothing for it to tell apart.
constexpr uint32_t stems_least_frames = 44100;

struct StemsProgress {
    bool busy = false;
    float fraction = 0;        // 0..1
    unsigned seconds_left = 0;
};

struct StemsJob {
    std::string engine;        // the program, on the card
    std::string work;          // a folder in RAM for its copy and its files: made here, removed after
    std::vector<float> left, right;             // the sample's 44.1 kHz frames
    std::array<std::string, stem_count> paths;  // the files to write, in stem_name()'s order
};

struct StemsResult {
    std::string error;         // empty: the three files are written; "stopped": it was cancelled
    std::array<std::string, stem_count> paths;
};

// One job at a time, on a thread of its own. start() and take() belong to
// one thread (the library's service thread); cancel(), busy() and progress() to any.
class StemSeparator {
public:
    StemSeparator() = default;
    ~StemSeparator(); // a job still running is stopped first
    StemSeparator(const StemSeparator&) = delete;
    StemSeparator& operator=(const StemSeparator&) = delete;

    bool start(StemsJob job); // false while a job runs or its result waits to be taken
    void cancel();
    bool busy() const { return running.load(std::memory_order_acquire); }
    StemsProgress progress() const;
    bool take(StemsResult& out);

private:
    void run(StemsJob job);
    std::string separate(StemsJob& job);
    std::thread worker;
    std::atomic<bool> running {false}, cancelled {false};
    std::atomic<float> fraction {0};
    std::atomic<unsigned> seconds_left {0};
    std::atomic<int> child {0}; // the program's process while it runs
    mutable std::mutex lock;
    StemsResult result;         // under `lock`
    bool finished = false;      // under `lock`: `result` waits to be taken
};

} // namespace melodee
