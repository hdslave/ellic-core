// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/stems.h"
#include "melodee/engine/sample_data.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#include <sys/syscall.h>
#endif

extern char** environ;

namespace melodee {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// What the program names its files: the input's name with one of these after it, in stem_name()'s order.
const char* const made[stem_count] = {"_Drum.wav", "_Vocal.wav", "_Accompaniment.wav"};
// The program's arguments: threads, the frames of spectrum a pass of the model takes, the bins it tells apart
// (2048: up to 22 kHz) and the stems.
const char* const threads = "3";
const char* const time_step = "256";
const char* const bin_limit = "2048";
// How far the job is when the program is done (the rest: the three files written to the card), and how far the
// program is when its first pass of the model ends (it prints a line then).
constexpr float program_share = 0.94f;
constexpr double first_pass = 0.53;

double seconds_of(uint32_t frames) { return double(frames) / 44100.0; }
} // namespace

uint64_t stems_memory(uint32_t frames) {
    // The program: 400 MB and 4.9 MB a second cover what was measured. Its copy in RAM: 40 MB. Its input and three
    // outputs there: 1.4 MB a second together.
    return uint64_t((440.0 + 6.3 * seconds_of(frames)) * 1024.0 * 1024.0);
}

unsigned stems_seconds(uint32_t frames) {
    // 6 s and 0.72 s a second measured for the program; its copy from the card and the three files written after.
    return unsigned(std::ceil(9.0 + 0.72 * seconds_of(frames)));
}

StemSeparator::~StemSeparator() {
    cancel();
    if (worker.joinable()) worker.join();
}

bool StemSeparator::start(StemsJob job) {
    if (running.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (finished) return false;
    }
    if (worker.joinable()) worker.join();
    cancelled.store(false);
    fraction.store(0);
    seconds_left.store(stems_seconds(uint32_t(std::min(job.left.size(), job.right.size()))));
    running.store(true, std::memory_order_release);
    worker = std::thread([this, held = std::move(job)]() mutable { run(std::move(held)); });
    return true;
}

void StemSeparator::cancel() { cancelled.store(true); }

StemsProgress StemSeparator::progress() const {
    StemsProgress out;
    out.busy = running.load(std::memory_order_acquire);
    out.fraction = fraction.load(std::memory_order_relaxed);
    out.seconds_left = seconds_left.load(std::memory_order_relaxed);
    return out;
}

bool StemSeparator::take(StemsResult& out) {
    std::lock_guard<std::mutex> guard(lock);
    if (!finished) return false;
    out = std::move(result);
    result = StemsResult {};
    finished = false;
    return true;
}

void StemSeparator::run(StemsJob job) {
#if defined(__linux__)
    // Whatever the thread that asked runs at, this one waits and copies below the screen and the controls.
    sched_param normal {};
    sched_setscheduler(0, SCHED_OTHER, &normal);
    setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 5);
#endif
    StemsResult out;
    out.paths = job.paths;
    const std::string work = job.work;
    out.error = separate(job);
    std::error_code gone;
    if (!work.empty()) fs::remove_all(work, gone);
    if (!out.error.empty())
        for (const auto& path : out.paths)
            if (!path.empty()) fs::remove(path, gone); // no half of a job is left on the card
    {
        std::lock_guard<std::mutex> guard(lock);
        result = std::move(out);
        finished = true;
    }
    running.store(false, std::memory_order_release);
}

std::string StemSeparator::separate(StemsJob& job) {
    const uint32_t frames = uint32_t(std::min(job.left.size(), job.right.size()));
    if (frames < stems_least_frames) return "too short";
    if (job.work.empty()) return "no work folder";
    std::error_code error;
    if (!fs::is_regular_file(job.engine, error)) return "no program on the card";
    fs::remove_all(job.work, error); // what a job that was cut off left
    fs::create_directories(job.work, error);
    if (error) return "no work folder";
    const std::string program = job.work + "/melodee-stems", input = job.work + "/in.wav";
    fs::copy_file(job.engine, program, fs::copy_options::overwrite_existing, error);
    if (error) return "program not copied";
    fs::permissions(program, fs::perms::owner_all, error);
    if (cancelled.load()) return "stopped";
    {
        SampleData sample;
        job.left.resize(frames);
        job.right.resize(frames);
        sample.left = std::move(job.left);
        sample.right = std::move(job.right);
        sample.frames = frames;
        const auto failed = write_wav(input, sample);
        if (!failed.empty()) return failed;
    }

    int ends[2];
    if (pipe(ends) != 0) return "no pipe";
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], 1); // what it prints, and its errors, come here
    posix_spawn_file_actions_adddup2(&actions, ends[1], 2);
    posix_spawn_file_actions_addclose(&actions, ends[0]);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    const char* const args[] = {program.c_str(), threads, time_step, bin_limit, "3", input.c_str(), nullptr};
    pid_t pid = 0;
    // posix_spawn, not fork(): no copy of Melodee's page tables is made (plugin/vst2_bridge_instance.cpp).
    const int spawned = posix_spawn(&pid, args[0], &actions, nullptr, const_cast<char* const*>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(ends[1]);
    if (spawned != 0) { close(ends[0]); return "program did not start"; }
    child.store(pid);

    // It prints a line as each of its steps ends. The first pass of the model ending tells how fast this job goes;
    // until then the time measured on the MPC is the guess.
    const auto began = Clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - began).count(); };
    double whole = std::max(1.0, double(stems_seconds(frames)));
    bool paced = false, killed = false, reaped = false;
    int status = 0;
    std::string line;
    for (;;) {
        if (cancelled.load() && !killed) { kill(pid, SIGKILL); killed = true; }
        pollfd waiting {ends[0], POLLIN, 0};
        if (poll(&waiting, 1, 100) > 0) {
            char bytes[512];
            const ssize_t got = read(ends[0], bytes, sizeof bytes);
            if (got == 0) break; // it has ended
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
            for (ssize_t n = 0; n < got; ++n) {
                if (bytes[n] != '\n') {
                    if (line.size() < 256) line += bytes[n];
                    continue;
                }
                if (!paced && line.find("Inference") != std::string::npos) { whole = std::max(1.0, elapsed() / first_pass); paced = true; }
                line.clear();
            }
        }
        // It is over when the program is, not when the pipe closes: something it started may still hold the pipe.
        if (waitpid(pid, &status, WNOHANG) == pid) { reaped = true; break; }
        const double at = elapsed();
        fraction.store(program_share * float(std::min(1.0, at / whole)), std::memory_order_relaxed);
        seconds_left.store(unsigned(std::ceil(std::max(1.0, whole - at))), std::memory_order_relaxed);
    }
    close(ends[0]);
    while (!reaped && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    child.store(0);
    if (cancelled.load()) return "stopped";
    // The program asks the kernel to end it first when memory runs out (tools/stems/stems_main.c).
    if (WIFSIGNALED(status)) return WTERMSIG(status) == SIGKILL ? "out of memory" : "program stopped";
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "program failed";

    for (unsigned n = 0; n < stem_count; ++n) {
        if (cancelled.load()) return "stopped";
        SampleData stem;
        const std::string from = input + made[n];
        auto failed = read_wav(from, stem, 3600);
        if (!failed.empty()) return std::string("no ") + stem_name(n) + " made";
        stem.left.resize(frames, 0.0f); // the sample's length to the frame, so its trim and slices fit each stem
        stem.right.resize(frames, 0.0f);
        stem.frames = frames;
        fs::create_directories(fs::path(job.paths[n]).parent_path(), error);
        failed = write_wav(job.paths[n], stem);
        if (!failed.empty()) return failed;
        fs::remove(from, error); // its RAM back before the next is read
        fraction.store(program_share + (1.0f - program_share) * float(n + 1) / float(stem_count), std::memory_order_relaxed);
        seconds_left.store(1, std::memory_order_relaxed);
    }
    return {};
}

} // namespace melodee
