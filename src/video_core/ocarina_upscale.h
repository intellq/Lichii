// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss };

constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                    bool dlss_available,
                                                    std::string_view override) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if ((dlss_enabled || override == "dlss") && dlss_available) {
        return OcarinaUpscaleBackend::Dlss;
    }
    return fsr_enabled ? OcarinaUpscaleBackend::Fsr : OcarinaUpscaleBackend::Native;
}

/// Texture LOD bias added to Ocarina's mipmapped samplers while FSR upscales. AMD recommends
/// log2(render / output) - 1: -1.58 for docked 1440p -> 4K.
constexpr float OcarinaFsrLodBias(OcarinaUpscaleBackend backend) {
    return backend == OcarinaUpscaleBackend::Fsr ? -1.58f : 0.0f;
}

/// The same bias for any output: AMD's log2(render / output) - 1, with the ratio taken over the
/// area so a scale on one axis only (2560x1440 -> 3440x1440) counts half. 0 without FSR.
inline float OcarinaFsrLodBias(OcarinaUpscaleBackend backend, float output_area_per_render) {
    if (backend != OcarinaUpscaleBackend::Fsr || !(output_area_per_render > 0.0f)) {
        return 0.0f;
    }
    return -0.5f * std::log2(output_area_per_render) - 1.0f;
}

/// Size, in pixels, the last presented frame took on screen (the window's layout after the
/// aspect ratio), width in the high half; 0 before the first frame. Written by the renderer,
/// read by Ocarina's upscaler and the texture cache.
inline std::atomic<std::uint64_t> ocarina_screen_extent{0};

inline void SetOcarinaScreenExtent(std::uint32_t width, std::uint32_t height) {
    ocarina_screen_extent.store((std::uint64_t{width} << 32) | height, std::memory_order_relaxed);
}

struct OcarinaExtent {
    std::uint32_t width{};
    std::uint32_t height{};
    bool operator==(const OcarinaExtent&) const = default;
};

/// The size FSR writes Ocarina's 2560x1440 scene at for an output choice (Settings::
/// OcarinaFsrOutput: 0 4K, 1 the size on screen, 2 the game's size). Never below the scene on
/// either axis (FSR only upscales) nor above 3x of it (FSR's limit); the size on screen falls
/// back to 4K until a frame was presented.
constexpr OcarinaExtent OcarinaFsrOutputExtent(unsigned choice, OcarinaExtent screen) {
    constexpr OcarinaExtent scene{2560, 1440};
    if (choice == 2) {
        return scene;
    }
    if (choice != 1 || screen.width == 0 || screen.height == 0) {
        return {3840, 2160};
    }
    const auto fit = [](std::uint32_t value, std::uint32_t low) {
        return value < low ? low : value > low * 3 ? low * 3 : value;
    };
    return {fit(screen.width, scene.width), fit(screen.height, scene.height)};
}

inline OcarinaExtent OcarinaScreenExtent() {
    const std::uint64_t value = ocarina_screen_extent.load(std::memory_order_relaxed);
    return {static_cast<std::uint32_t>(value >> 32), static_cast<std::uint32_t>(value)};
}

/// Whether a sampler with that bias keeps it for an image: only docked (handheld isn't measured),
/// not for images the GPU wrote (render targets would read a mip too sharp), and not once the
/// upscaler failed this session (the game's own pass runs again).
constexpr bool KeepOcarinaFsrLodBias(bool docked, bool upscaler_failed, bool gpu_written) {
    return docked && !upscaler_failed && !gpu_written;
}

} // namespace VideoCommon
