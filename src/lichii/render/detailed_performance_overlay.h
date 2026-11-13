// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <QString>
#include <QWidget>

#include "common/dkb_cpu_trace.h"
#include "core/perf_stats.h"
#include "qt_common/config/shared_translation.h"
#include "lichii/render/system_monitor.h"

namespace VideoCore {
class ShaderNotify;
}

class MainWindow;
class QPainter;

/// The detailed performance overlay: a dark framed panel with gold labels, in the top left corner
/// like the default overlay. Level 1 is a frame rate counter; level 2 adds the machine's load, the
/// frame rate with its lows, the frame times and their graph; level 3 adds the GPU work per frame,
/// the picture settings and the GPU and CPU names.
class DetailedPerformanceOverlay : public QWidget {
    Q_OBJECT

public:
    static constexpr int Levels = 3;

    explicit DetailedPerformanceOverlay(MainWindow* parent);
    ~DetailedPerformanceOverlay() override;

    void SetLevel(int level);

protected:
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    static constexpr std::size_t PlotFrames = 120;

    struct Stats {
        float fps = -1;
        float fg_fps = 0; ///< what the display gets with frame generation, 0 without it
        float frame_ms_avg = -1;
        float frame_ms_max = -1;
        float low1_fps = -1;
        float low01_fps = -1;
        std::array<float, PlotFrames> frame_ms{}; ///< oldest first, 0 for frames not seen yet
        MachineStats machine;
        float vram_game_mb = -1;
        float front_ms = -1;
        float back_ms = -1;
        float gpu_ms = -1;
        float core_ms = -1;
        int shaders_building = -1;
        /// window, game image, upscaling, frame limit, VSync, display, resolution, HDR, frame
        /// generation
        std::array<QString, 9> picture;
        QString gpu_name;
        QString cpu_name;
    };

    void resetPosition(const QPoint& pos);
    void updateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);
    void updatePicture();
    QString enumText(u32 index, u32 value) const;
    /// Lays the panel out; draws it when `painter` isn't null. Returns its size.
    QSize layout(QPainter* painter) const;
    void relayout();

    MainWindow* m_mainWindow = nullptr;
    QPoint m_offset{25, 75};
    QPoint m_drag_start_pos;
    int m_level = 1;

    Stats m_stats;
    SystemMonitor m_monitor;
    std::unique_ptr<ConfigurationShared::ComboboxTranslationMap> m_enums;
    std::optional<Common::DkbCpuTrace::Snapshot> m_cpuSnapshot;
    u64 m_upscalerEvaluations = 0;
    bool m_upscalerRunning = false;
};
