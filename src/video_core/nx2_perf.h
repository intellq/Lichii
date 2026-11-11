// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon::Nx2Perf {

[[nodiscard]] inline bool VerifySwitchSet() {
    const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
    return value != nullptr && value[0] == '1';
}

/// EDEN_NX2_PERF_VERIFY=1. A plain flag: the buffer cache, the memory tracker and the 3D engine
/// check it on hot paths, and a function's static is checked for its initialization on every
/// call (MSVC reads a thread-local for it).
inline bool g_verify = VerifySwitchSet();

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_verify = VerifySwitchSet();
}

inline bool Verify() {
    return g_verify;
}

/// The frame period the game asks for (nx2:vi2 present word 12 at the speed limit), 0 while
/// unknown or unpaced. The host frame pacer (vk_host_pacer.h) never holds frames longer.
inline std::atomic<long long> g_guest_frame_interval_ns{0};

/// The display's measured refresh period while the host frame pacer runs, else 0. The game's
/// present pacing (nx2_gsv.h) times its frames to whole multiples of it (genlock).
inline std::atomic<long long> g_host_refresh_ns{0};

/// The display's refresh period once the host frame pacer has measured it, paced or not, else
/// 0. Ocarina of Time's high refresh rate cap (nx2_gsv.h) stays at or below it.
inline std::atomic<long long> g_display_refresh_ns{0};

/// At 30 FPS and below the host frame pacer presents frames as soon as they are rendered and
/// publishes how long before the display's next refresh the last one was ready (ns), or -1
/// while it isn't pacing that way. The game's present pacing (nx2_gsv.h) moves its frames until
/// that is ~6 ms: each lands on a refresh with little wait (input lag) and evenly spaced.
inline std::atomic<long long> g_host_phase_ns{-1};

/// Host frame times: the interval between frames handed to the swapchain, counted in 0.25 ms
/// buckets (the last holds 63.75 ms and up). The debug server's status returns the counts; the
/// difference between two calls gives that window's percentiles (1% and 0.1% lows).
inline constexpr size_t FrameTimeBuckets = 256;
inline constexpr long long FrameTimeBucketNs = 250'000;
inline std::array<std::atomic<unsigned long long>, FrameTimeBuckets> g_frame_times{};

inline void NoteFrameTime(long long interval_ns) {
    // Longer gaps are loading screens or a paused game, not frames.
    if (interval_ns <= 0 || interval_ns > 1'000'000'000) {
        return;
    }
    const auto bucket =
        (std::min)(static_cast<size_t>(interval_ns / FrameTimeBucketNs), FrameTimeBuckets - 1);
    g_frame_times[bucket].fetch_add(1, std::memory_order_relaxed);
}

} // namespace VideoCommon::Nx2Perf
