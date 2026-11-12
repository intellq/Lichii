// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <utility>

#include "common/common_types.h"

namespace VideoCore {

/// A temporal upscaler that draws in place of a game's own upscaler
enum class UpscalerKind : u8 {
    None,
    Dlss,
    Fsr3,
    Fsr4,
};

/// What replaces a game's upscaler, for the status bar
struct UpscalerStatus {
    bool dlss_available{}; ///< Streamline started and NVIDIA DLSS runs on this device
    UpscalerKind kind{}; ///< the upscaler that evaluated last
    u64 evaluations{}; ///< evaluations recorded so far, all upscalers together
    u32 render_width{};
    u32 render_height{};
    u32 output_width{};
    u32 output_height{};
    /// Why the game's own upscale drew the last frame, when the renderer knows (a literal)
    const char* fallback_reason{};
};

/// What draws Ocarina of Time's scene, chosen from the status bar for the session (not saved)
enum class UpscalerChoice : u8 {
    Fsr4,
    Fsr3,
    Game, ///< the game's own upscaler
};

/// True while the running game started with FSR 4: the status bar then cycles FSR 4, FSR 3.1 and
/// the game's upscaler. Written by the renderer, read by the status bar.
inline std::atomic<bool> upscaler_cycle_available{false};
/// The status bar's choice, read by the renderer each frame. Back to FSR 4 when a game starts.
inline std::atomic<UpscalerChoice> upscaler_choice{UpscalerChoice::Fsr4};

/// Records one evaluation (from the thread that records the frame's commands).
void NoteUpscale(UpscalerKind kind, u32 render_width, u32 render_height, u32 output_width,
                 u32 output_height) noexcept;

/// The evaluations so far and the last one's upscaler and sizes (from any thread).
[[nodiscard]] UpscalerStatus LastUpscale() noexcept;

/// Records the size of the game's image the presenter last drew to the window, after the
/// resolution scaling (from the thread that records the frame's commands).
void NotePresentedImage(u32 width, u32 height) noexcept;

/// The last size NotePresentedImage recorded, {0, 0} before the first (from any thread).
[[nodiscard]] std::pair<u32, u32> LastPresentedImage() noexcept;

} // namespace VideoCore
