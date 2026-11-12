// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <thread>

#include "common/logging.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/renderer_base.h"

namespace VideoCore {

namespace {
std::atomic<u64> upscale_evaluations{};
std::atomic<u64> upscale_sizes{}; ///< 16 bits each: render width, height, output width, height
std::atomic<UpscalerKind> upscale_kind{};
std::atomic<u64> presented_image{}; ///< width in the high 32 bits, height in the low
} // Anonymous namespace

void NotePresentedImage(u32 width, u32 height) noexcept {
    presented_image.store(u64{width} << 32 | height, std::memory_order_relaxed);
}

std::pair<u32, u32> LastPresentedImage() noexcept {
    const u64 size = presented_image.load(std::memory_order_relaxed);
    return {static_cast<u32>(size >> 32), static_cast<u32>(size)};
}

void NoteUpscale(UpscalerKind kind, u32 render_width, u32 render_height, u32 output_width,
                 u32 output_height) noexcept {
    const u64 sizes = u64{render_width & 0xFFFF} << 48 | u64{render_height & 0xFFFF} << 32 |
                      u64{output_width & 0xFFFF} << 16 | u64{output_height & 0xFFFF};
    upscale_sizes.store(sizes, std::memory_order_relaxed);
    upscale_kind.store(kind, std::memory_order_relaxed);
    upscale_evaluations.fetch_add(1, std::memory_order_relaxed);
}

UpscalerStatus LastUpscale() noexcept {
    const u64 sizes = upscale_sizes.load(std::memory_order_relaxed);
    return {
        .kind = upscale_kind.load(std::memory_order_relaxed),
        .evaluations = upscale_evaluations.load(std::memory_order_relaxed),
        .render_width = static_cast<u32>(sizes >> 48 & 0xFFFF),
        .render_height = static_cast<u32>(sizes >> 32 & 0xFFFF),
        .output_width = static_cast<u32>(sizes >> 16 & 0xFFFF),
        .output_height = static_cast<u32>(sizes & 0xFFFF),
    };
}

RendererBase::RendererBase(Core::Frontend::EmuWindow& window_,
                           std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : render_window{window_}, context{std::move(context_)} {
    RefreshBaseSettings();
}

RendererBase::~RendererBase() = default;

void RendererBase::RefreshBaseSettings() {
    Settings::RefreshNvidiaDlssRuntimeSettings();
    UpdateCurrentFramebufferLayout();
}

void RendererBase::UpdateCurrentFramebufferLayout() {
    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();

    render_window.UpdateCurrentFramebufferLayout(layout.width, layout.height);
}

bool RendererBase::IsScreenshotPending() const {
    return renderer_settings.screenshot_requested;
}

void RendererBase::RequestScreenshot(void* data, std::function<void(bool)> callback,
                                     const Layout::FramebufferLayout& layout,
                                     Service::Nvnflinger::LayerStackId layer_stack) {
    if (renderer_settings.screenshot_requested) {
        LOG_ERROR(Render, "A screenshot is already requested or in progress, ignoring the request");
        return;
    }
    auto async_callback{[callback_ = std::move(callback)](bool invert_y) {
        std::thread t{callback_, invert_y};
        t.detach();
    }};
    renderer_settings.screenshot_bits = data;
    renderer_settings.screenshot_complete_callback = async_callback;
    renderer_settings.screenshot_framebuffer_layout = layout;
    renderer_settings.screenshot_layer_stack = layer_stack;
    renderer_settings.screenshot_requested = true;
}

} // namespace VideoCore
