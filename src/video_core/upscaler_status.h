// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

/// Records one evaluation (from the thread that records the frame's commands).
void NoteUpscale(UpscalerKind kind, u32 render_width, u32 render_height, u32 output_width,
                 u32 output_height) noexcept;

/// The evaluations so far and the last one's upscaler and sizes (from any thread).
[[nodiscard]] UpscalerStatus LastUpscale() noexcept;

} // namespace VideoCore
