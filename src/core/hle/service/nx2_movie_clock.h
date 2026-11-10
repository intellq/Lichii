// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"

namespace Service {

// The clock a video decoder session (nx2:vdecsvc) drops late frames by: the pts the movie's
// audio has reached. Times are host microseconds; pause_end is when emulation last resumed.
class Nx2MovieClock {
public:
    // A longer gap between acquires is a pause, a longer lag a jump in the stream; both restart
    // the clock.
    static constexpr s64 PauseGap = 1'000'000;
    static constexpr s64 MaxCatchUp = 3'000'000;

    static u64 EarlyWaitUs(u64 pts, u64 due, u64 interval) {
        if (pts <= due || pts - due <= interval / 2) {
            return 0;
        }
        const u64 early = pts - due - interval / 2;
        return early < 1'000 ? 1'000 : early > 20'000 ? 20'000 : early;
    }

    void Reset() {
        offset.reset();
        audio_start.reset();
        audio_disabled = false;
    }

    // The client acquired the frame at pts. The clock starts from the first acquire and runs in
    // real time from there. A frame acquired ahead of the clock doesn't move it: Kirby Air Riders
    // takes a frame before it shows it and holds it until its own clock reaches it, and following
    // those acquires ran the clock ahead of the picture, so 563 of 588 frames came out late.
    void Acquired(s64 now, u64 pts, s64 pause_end, std::optional<u64> played = std::nullopt) {
        const s64 acquired_offset = now - static_cast<s64>(pts);
        if (!offset || now - last_acquire > PauseGap || pause_end > last_acquire ||
            pts < last_acquired_pts || acquired_offset - *offset > MaxCatchUp) {
            offset = acquired_offset;
            audio_start = played;
            audio_start_pts = pts;
            audio_start_time = now;
            audio_disabled = false;
        }
        last_acquire = now;
        last_acquired_pts = pts;
    }

    // The pts the movie's audio has reached, unless the client hasn't acquired a frame yet or for
    // a while (paused; the next acquire restarts the clock).
    std::optional<u64> Due(s64 now, s64 pause_end, std::optional<u64> played = std::nullopt) {
        if (!offset) {
            return std::nullopt;
        }
        const s64 due = now - *offset;
        if (due < 0 || now - last_acquire > PauseGap || pause_end > last_acquire) {
            return std::nullopt;
        }
        if (!audio_disabled && audio_start && played && *played >= *audio_start) {
            constexpr u64 SampleRate = 48'000;
            const u64 frames = *played - *audio_start;
            const u64 audio_us = frames / SampleRate * 1'000'000 +
                                 frames % SampleRate * 1'000'000 / SampleRate;
            const s64 wall_us = now - audio_start_time;
            if (wall_us > 500'000 && audio_us < static_cast<u64>(wall_us / 4)) {
                audio_disabled = true;
            } else {
                return audio_start_pts + audio_us;
            }
        }
        return static_cast<u64>(due);
    }

private:
    std::optional<s64> offset; ///< Host time minus pts of the frame the clock started from
    s64 last_acquire{};
    u64 last_acquired_pts{};
    std::optional<u64> audio_start;
    u64 audio_start_pts{};
    s64 audio_start_time{};
    bool audio_disabled{};
};

} // namespace Service
