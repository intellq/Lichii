// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace VideoCommon {

/// Stretches of frames in which an upscaler sits out (the game's own pass draws instead of the
/// replacement), for the log: when each starts and why, and when it ends and how long it lasted.
/// A stretch that starts within a second of the last logged line is only counted, and the next
/// logged start says how many were.
class UpscalerFallbackLog {
public:
    using Clock = std::chrono::steady_clock;

    struct Start {
        std::uint32_t unlogged; ///< stretches since the last logged start that weren't logged
    };
    struct End {
        std::uint64_t frames;
        float seconds;
    };

    /// The upscaler sits this frame out. Returns a start to log: a new stretch, or another reason
    /// in a logged one.
    std::optional<Start> Fallback(std::uint64_t frame, Clock::time_point now, const char* why) {
        const bool allowed = !last_logged || now - *last_logged >= std::chrono::seconds{1};
        if (!start_frame) {
            start_frame = frame;
            start_time = now;
            logged = allowed;
            reason = why;
            if (!allowed) {
                ++unlogged;
                return std::nullopt;
            }
        } else if (std::exchange(reason, why) == why || !logged || !allowed) {
            return std::nullopt;
        }
        last_logged = now;
        return Start{std::exchange(unlogged, 0)};
    }

    /// The upscaler runs this frame. Returns the end of a logged stretch.
    std::optional<End> Replaced(std::uint64_t frame, Clock::time_point now) {
        if (!start_frame) {
            return std::nullopt;
        }
        const End end{frame - *start_frame, std::chrono::duration<float>(now - start_time).count()};
        start_frame.reset();
        if (!logged) {
            return std::nullopt;
        }
        last_logged = now;
        return end;
    }

private:
    std::optional<std::uint64_t> start_frame;
    Clock::time_point start_time{};
    const char* reason{};
    bool logged{};
    std::optional<Clock::time_point> last_logged;
    std::uint32_t unlogged{};
};

} // namespace VideoCommon
