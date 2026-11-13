// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "detailed_performance_overlay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <QCoreApplication>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QWindow>

#include "common/settings.h"
#include "core/core.h"
#include "main_window.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"
#include "video_core/upscaler_status.h"

namespace {

using Common::DkbCpuTrace::Role;

// The panel's look: gold labels and parchment values on a dark brown framed panel, the sections
// parted by a thin ornamented divider.
const QColor LabelColor{236, 196, 104};
const QColor ValueColor{244, 236, 214};
const QColor DimColor{170, 152, 120};

constexpr qreal FontPixels = 15;
constexpr qreal PaddingX = 24;
constexpr qreal PaddingY = 20;
constexpr qreal CellPaddingX = 7;
constexpr qreal CellPaddingY = 1.5;
constexpr qreal ItemGap = 4;
constexpr qreal DividerScale = 0.72;
constexpr qreal PlotHeight = 48;
constexpr qreal CounterPaddingX = 14;
constexpr qreal CounterPaddingY = 9;
constexpr qreal BigFontPixels = 40;
constexpr qreal LowFontPixels = 18;
constexpr qreal HeaderGap = 12;

constexpr auto LowsWindow = std::chrono::seconds{10};
constexpr auto RateWindow = std::chrono::seconds{1};

constexpr std::array GuestCores{
    Role::GuestCore0, Role::GuestCore1, Role::GuestCore2,      Role::GuestCore3,
    Role::GuestCore4, Role::GuestCore5, Role::GuestSingleCore,
};

QFont PanelFont() {
    QFont font;
    font.setFamilies({QStringLiteral("Roboto"), QStringLiteral("Roboto Medium"),
                      QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans"),
                      QStringLiteral("Segoe UI")});
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(static_cast<int>(FontPixels));
    font.setWeight(QFont::Medium);
    return font;
}

QColor Rgba(int r, int g, int b, qreal alpha) {
    return QColor{r, g, b, std::clamp(static_cast<int>(std::lround(alpha * 255)), 0, 255)};
}

/// The framed panel: a dark brown fill shading down, a light edge and an inset line.
void DrawPanel(QPainter& painter, const QRectF& rect, qreal s, qreal alpha) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.fillRect(rect, Rgba(20, 15, 10, 0.85 * alpha));
    const QRectF inner = rect.adjusted(1 * s, 1 * s, -1 * s, -1 * s);
    QLinearGradient fill{inner.topLeft(), inner.bottomLeft()};
    fill.setColorAt(0, Rgba(44, 35, 25, alpha));
    fill.setColorAt(1, Rgba(33, 26, 19, alpha));
    painter.fillRect(inner, fill);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{QColor{110, 90, 58}, 1.4 * s});
    painter.drawRect(inner);
    // the inset line, a small diamond on each corner
    const qreal inset = 7 * s;
    const QRectF line = rect.adjusted(inset, inset, -inset, -inset);
    painter.setPen(QPen{QColor{122, 101, 66}, 1.0});
    painter.drawRect(line);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{122, 101, 66});
    const qreal d = 2.5 * s;
    for (const QPointF& corner :
         {line.topLeft(), line.topRight(), line.bottomLeft(), line.bottomRight()}) {
        painter.drawPolygon(QPolygonF{{corner.x(), corner.y() - d},
                                      {corner.x() + d, corner.y()},
                                      {corner.x(), corner.y() + d},
                                      {corner.x() - d, corner.y()}});
    }
    painter.restore();
}

qreal DividerHeight() {
    return 26 * DividerScale;
}

/// The divider across the panel: a dot at each end, a line, and a diamond ornament in the middle.
void DrawDivider(QPainter& painter, qreal x0, qreal x1, qreal y) {
    const qreal s = DividerScale;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor line{96, 80, 56};
    const QColor dot{110, 92, 64};
    const QColor ornament{138, 114, 76};
    const qreal orn_w = std::min(78 * s, (x1 - x0) * 0.5);
    const qreal k = orn_w / 108;
    const qreal cx = (x0 + x1) / 2;
    const qreal dot_r = 1.5 * s;
    const qreal gap = 5 * s;
    painter.setPen(Qt::NoPen);
    painter.setBrush(dot);
    painter.drawEllipse(QPointF{x0 + 4 * s + dot_r, y}, dot_r, dot_r);
    painter.drawEllipse(QPointF{x1 - 4 * s - dot_r, y}, dot_r, dot_r);
    painter.setPen(QPen{line, 1.0 * s});
    painter.drawLine(QPointF{x0 + 4 * s + 2 * dot_r + gap, y},
                     QPointF{cx - orn_w / 2 - gap, y});
    painter.drawLine(QPointF{cx + orn_w / 2 + gap, y},
                     QPointF{x1 - 4 * s - 2 * dot_r - gap, y});
    const qreal d = 9 * k * 1.08;
    const qreal inner = 3.2 * k * 1.08;
    const qreal t = 1.6 * k;
    painter.setPen(QPen{ornament, t});
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(QPolygonF{{cx, y - d}, {cx + d, y}, {cx, y + d}, {cx - d, y}});
    painter.drawLine(QPointF{cx - orn_w / 2, y}, QPointF{cx - d - 3 * k, y});
    painter.drawLine(QPointF{cx + d + 3 * k, y}, QPointF{cx + orn_w / 2, y});
    painter.setPen(Qt::NoPen);
    painter.setBrush(ornament);
    painter.drawPolygon(
        QPolygonF{{cx, y - inner}, {cx + inner, y}, {cx, y + inner}, {cx - inner, y}});
    painter.restore();
}

QString None() {
    return QStringLiteral("-");
}

QString Fixed(double value, int decimals) {
    return QString::number(value, 'f', decimals);
}

/// A text the status bar and the settings share, in the UI's language.
QString TranslateShared(const QString& text) {
    const QByteArray source = text.toUtf8();
    for (const char* const context : {"MainWindow", "ConfigurationShared"}) {
        const QString translated = QCoreApplication::translate(context, source.constData());
        if (translated != text) {
            return translated;
        }
    }
    return text;
}

} // namespace

DetailedPerformanceOverlay::DetailedPerformanceOverlay(MainWindow* parent)
    : QWidget(parent), m_mainWindow{parent},
      m_enums{ConfigurationShared::ComboboxEnumeration(this)} {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    m_stats.cpu_name = m_monitor.CpuName();
    relayout();
    raise();

    resetPosition(m_mainWindow->pos());
    connect(parent, &MainWindow::positionChanged, this,
            &DetailedPerformanceOverlay::resetPosition);
    connect(m_mainWindow, &MainWindow::statsUpdated, this,
            &DetailedPerformanceOverlay::updateStats);
}

DetailedPerformanceOverlay::~DetailedPerformanceOverlay() = default;

void DetailedPerformanceOverlay::SetLevel(int level) {
    m_level = std::clamp(level, 1, Levels);
    relayout();
}

void DetailedPerformanceOverlay::resetPosition(const QPoint&) {
    const QPoint pos = m_mainWindow->pos();
    move(pos.x() + m_offset.x(), pos.y() + m_offset.y());
}

QString DetailedPerformanceOverlay::enumText(u32 index, u32 value) const {
    if (const auto it = m_enums->find(index); it != m_enums->end()) {
        for (const auto& [id, text] : it->second) {
            if (id == value) {
                return text;
            }
        }
    }
    return None();
}

void DetailedPerformanceOverlay::updateStats(const Core::PerfStatsResults& results,
                                             const VideoCore::ShaderNotify& shaders) {
    if (!isVisible()) {
        return;
    }
    auto& system = *QtCommon::system;
    auto& perf_stats = system.GetPerfStats();
    perf_stats.SetFrameTimeHistoryEnabled(m_level >= 2);

    Stats& s = m_stats;
    s.fps = std::isnan(results.average_game_fps) ? -1.0f
                                                 : static_cast<float>(results.average_game_fps);
    const double generated = m_mainWindow->FrameGenFps();
    s.fg_fps = generated >= 0.5 && s.fps > 0 ? static_cast<float>(s.fps + generated) : 0.0f;
    if (m_level < 2) {
        relayout();
        return;
    }

    // The frames: the rate and frame times over the last second, the lows over the last ten.
    const auto frames = perf_stats.GetRecentFrameTimes(LowsWindow);
    s.frame_ms_avg = s.frame_ms_max = s.low1_fps = s.low01_fps = -1;
    s.frame_ms.fill(0);
    if (!frames.empty() &&
        Core::FrameTimeHistory::Clock::now() - frames.back().end < std::chrono::seconds{1}) {
        double sum = 0;
        float max = 0;
        int count = 0;
        for (auto it = frames.rbegin();
             it != frames.rend() && frames.back().end - it->end < RateWindow; ++it) {
            sum += it->ms;
            max = std::max(max, it->ms);
            ++count;
        }
        if (count > 0) {
            s.frame_ms_avg = static_cast<float>(sum / count);
            s.frame_ms_max = max;
        }
        if (frames.size() >= 100) {
            std::vector<float> slowest;
            slowest.reserve(frames.size());
            for (const auto& frame : frames) {
                slowest.push_back(frame.ms);
            }
            std::sort(slowest.begin(), slowest.end(), std::greater<>{});
            const auto fps_at = [&](std::size_t share) {
                const std::size_t index = (slowest.size() + share - 1) / share - 1;
                return slowest[index] > 0 ? 1000.0f / slowest[index] : -1.0f;
            };
            s.low1_fps = fps_at(100);
            s.low01_fps = fps_at(1000);
        }
        const std::size_t shown = std::min(frames.size(), PlotFrames);
        for (std::size_t i = 0; i < shown; ++i) {
            s.frame_ms[PlotFrames - shown + i] = frames[frames.size() - shown + i].ms;
        }
    }

    // The machine, and the share of the card's memory this process holds.
    auto& renderer = system.GPU().Renderer();
    s.machine = m_monitor.Sample();
    const auto vram_game = renderer.GetProcessVramUsage();
    s.vram_game_mb = vram_game ? static_cast<float>(*vram_game / (1024.0 * 1024.0)) : -1.0f;

    // The GPU work per frame: the CPU time of the threads that feed it over the frames since the
    // last update, and the GPU's own busy time.
    const auto snapshot = Common::DkbCpuTrace::ReadSnapshot();
    s.front_ms = s.back_ms = s.core_ms = -1;
    if (m_cpuSnapshot && s.fps > 0) {
        const double wall_s =
            std::chrono::duration<double>(snapshot.wall_time - m_cpuSnapshot->wall_time).count();
        const double frame_count = s.fps * wall_s;
        const auto per_frame = [&](Role role) -> float {
            const auto index = static_cast<std::size_t>(role);
            const auto delta = Common::DkbCpuTrace::CpuTimeDelta(m_cpuSnapshot->threads[index],
                                                                 snapshot.threads[index]);
            if (!delta || frame_count <= 0) {
                return -1.0f;
            }
            return static_cast<float>(static_cast<double>(*delta) / 1e6 / frame_count);
        };
        s.front_ms = per_frame(Role::GpuCommand);
        s.back_ms = per_frame(Role::GpuBack);
        for (const Role role : GuestCores) {
            s.core_ms = std::max(s.core_ms, per_frame(role));
        }
    }
    m_cpuSnapshot = snapshot;

    renderer.RequestGpuBusySample();
    const auto gpu_ms = renderer.GetGpuBusyMs();
    s.gpu_ms = gpu_ms ? static_cast<float>(*gpu_ms) : -1.0f;
    // Without the OS's counter, the GPU's busy time against the frame time.
    if (s.machine.gpu_pct < 0 && s.gpu_ms >= 0 && s.frame_ms_avg > 0) {
        s.machine.gpu_pct = std::min(100.0f, s.gpu_ms / s.frame_ms_avg * 100.0f);
    }
    // The status bar asked the same a moment ago; asking again in the same update reads the same.
    s.shaders_building = const_cast<VideoCore::ShaderNotify&>(shaders).ShadersBuilding();

    if (m_level >= 3) {
        updatePicture();
        s.gpu_name = QString::fromStdString(renderer.GetDeviceInfo().model);
    }
    relayout();
}

void DetailedPerformanceOverlay::updatePicture() {
    auto& picture = m_stats.picture;
    const auto size_text = [](u32 width, u32 height) {
        return width && height ? QStringLiteral("%1x%2").arg(width).arg(height) : None();
    };

    const QSize window = m_mainWindow->RenderWindowPixelSize();
    picture[0] = size_text(static_cast<u32>(window.width()), static_cast<u32>(window.height()));
    const auto [image_width, image_height] = VideoCore::LastPresentedImage();
    picture[1] = size_text(image_width, image_height);

    // Upscaling: the upscaler that replaced the game's own since the last update, else the
    // window's filter.
    const auto upscaler = QtCommon::system->GPU().Renderer().GetUpscalerStatus();
    m_upscalerRunning = upscaler.evaluations > m_upscalerEvaluations;
    m_upscalerEvaluations = upscaler.evaluations;
    if (m_upscalerRunning && upscaler.kind != VideoCore::UpscalerKind::None) {
        switch (upscaler.kind) {
        case VideoCore::UpscalerKind::Dlss:
            picture[2] = QStringLiteral("NVIDIA DLSS");
            break;
        case VideoCore::UpscalerKind::Fsr4:
            picture[2] = QStringLiteral("AMD FSR 4");
            break;
        default:
            picture[2] = QStringLiteral("AMD FSR 3.1");
            break;
        }
    } else {
        const auto filter = Settings::values.scaling_filter.GetValue();
        if (filter == Settings::ScalingFilter::Fsr) {
            picture[2] = tr("AMD FSR 1, sharpness %1%")
                             .arg(Settings::values.fsr_sharpening_slider.GetValue());
        } else {
            const auto it = ConfigurationShared::scaling_filter_texts_map.find(filter);
            picture[2] = it != ConfigurationShared::scaling_filter_texts_map.end()
                             ? TranslateShared(it->second)
                             : None();
        }
    }

    picture[3] = Settings::values.use_speed_limit.GetValue()
                     ? tr("%1%", "The speed limit").arg(Settings::SpeedLimit())
                     : tr("Off");
    picture[4] = Settings::values.vsync_mode.GetValue() == Settings::VSyncMode::Immediate
                     ? tr("Off")
                     : tr("On");
    picture[5] =
        enumText(Settings::EnumMetadata<Settings::AspectRatio>::Index(),
                 static_cast<u32>(Settings::values.aspect_ratio.GetValue()));
    picture[6] =
        enumText(Settings::EnumMetadata<Settings::ResolutionSetup>::Index(),
                 static_cast<u32>(Settings::values.resolution_setup.GetValue()));
    picture[7] = enumText(Settings::EnumMetadata<Settings::HdrMode>::Index(),
                          static_cast<u32>(Settings::values.hdr_mode.GetValue()));
    if (!Settings::values.frame_gen.GetValue()) {
        picture[8] = tr("Off");
    } else {
        const auto it = ConfigurationShared::frame_gen_type_texts_map.find(
            Settings::values.frame_gen_type.GetValue());
        const QString type = it != ConfigurationShared::frame_gen_type_texts_map.end()
                                 ? TranslateShared(it->second)
                                 : QString();
        picture[8] = Settings::values.frame_gen_target_rate.GetValue() == 0
                         ? tr("%1 %2x").arg(type).arg(Settings::FrameGenMultiplier())
                         : tr("%1 auto").arg(type);
    }
}

void DetailedPerformanceOverlay::relayout() {
    const QSize wanted = layout(nullptr);
    if (wanted != size()) {
        setFixedSize(wanted);
    }
    update();
}

QSize DetailedPerformanceOverlay::layout(QPainter* painter) const {
    const QFont font = PanelFont();
    const QFontMetricsF metrics{font};
    const auto width_of = [&](const QString& text) { return metrics.horizontalAdvance(text); };
    const Stats& s = m_stats;
    if (painter) {
        painter->setFont(font);
        painter->setRenderHint(QPainter::TextAntialiasing);
    }

    // Level 1: the frame rate alone, in a small panel.
    if (m_level <= 1) {
        QString text = None();
        if (s.fps >= 0) {
            text = s.fg_fps > 0 ? tr("%1 fps (%2 frame gen)")
                                      .arg(Fixed(s.fps, 0), Fixed(s.fg_fps, 0))
                                : tr("%1 fps").arg(Fixed(s.fps, 0));
        }
        const QSizeF box{width_of(text) + 2 * CounterPaddingX,
                         metrics.height() + 2 * CounterPaddingY};
        if (painter) {
            DrawPanel(*painter, QRectF{QPointF{0, 0}, box}, 0.6, 0.9);
            painter->setPen(ValueColor);
            painter->drawText(QPointF{CounterPaddingX, CounterPaddingY + metrics.ascent()}, text);
        }
        return QSize{static_cast<int>(std::ceil(box.width())),
                     static_cast<int>(std::ceil(box.height()))};
    }

    // The texts, formatted once for both the measuring and the drawing.
    const auto value = [](float v, const QString& formatted) {
        return v < 0 ? None() : formatted;
    };
    const auto gb = [](float mb) { return mb < 0 ? -1.0f : mb / 1024.0f; };
    const auto pct = [&](float v) { return value(v, tr("%1%").arg(Fixed(v, 0))); };
    const auto temp = [&](float v) { return value(v, tr("%1 °C").arg(Fixed(v, 0))); };
    const auto gbs = [&](float mb) { return value(mb, tr("%1 GB").arg(Fixed(gb(mb), 1))); };
    const auto gb_of = [&](float mb) { return value(mb, tr("%1 GB").arg(Fixed(gb(mb), 1))); };
    const auto game_gb = [&](float mb) {
        return value(mb, tr("%1 GB").arg(Fixed(gb(mb), 1)));
    };
    const auto game_pct = [&](float v) { return value(v, tr("%1%").arg(Fixed(v, 0))); };
    const auto watts = [&](float v) { return value(v, tr("%1 W").arg(Fixed(v, 0))); };
    const auto mhz = [&](float v) { return value(v, tr("%1 MHz").arg(Fixed(v, 0))); };
    const auto fps = [&](float v) { return value(v, Fixed(v, 0)); };
    const auto frame_ms = [&](float v) { return value(v, tr("%1 ms").arg(Fixed(v, 1))); };
    const auto max_ms = [&](float v) { return value(v, tr("max %1 ms").arg(Fixed(v, 1))); };
    const auto wait_ms = [&](float v) { return value(v, tr("%1 ms").arg(Fixed(v, 2))); };

    struct Row {
        QString label;
        std::array<QString, 3> cells;
    };
    const MachineStats& m = s.machine;
    // the card's memory counts every program on it: this process's share beside it, the clock
    // where that is unknown
    const QString vram_third = s.vram_game_mb >= 0 ? game_gb(s.vram_game_mb) : mhz(m.gpu_clock_mhz);
    const std::vector<Row> machine_rows{
        {tr("GPU"), {pct(m.gpu_pct), temp(m.gpu_temp_c), watts(m.gpu_power_w)}},
        {tr("VRAM"), {gbs(m.vram_used_mb), gb_of(m.vram_total_mb), vram_third}},
        {tr("CPU"), {pct(m.cpu_total_pct), temp(m.cpu_temp_c), game_pct(m.cpu_game_pct)}},
        {tr("RAM"), {gbs(m.ram_used_mb), gb_of(m.ram_total_mb), game_gb(m.ram_game_mb)}},
    };
    std::vector<Row> frame_rows;
    // the maximum only in the full panel
    frame_rows.push_back({tr("Frame"),
                          {frame_ms(s.frame_ms_avg),
                           m_level >= 3 ? max_ms(s.frame_ms_max) : QString(), QString()}});

    // Every column is as wide as the widest text it can show, measured on samples of 8s, so the
    // panel keeps its width while the numbers change.
    std::array<qreal, 4> columns{};
    for (const std::vector<Row>* rows :
         std::array<const std::vector<Row>*, 2>{&machine_rows, &frame_rows}) {
        for (const Row& row : *rows) {
            columns[0] = std::max(columns[0], width_of(row.label));
        }
    }
    const std::array<std::vector<QString>, 3> samples{{
        {pct(100), gbs(888.8f * 1024), fps(888), frame_ms(888.8f)},
        {temp(188), gb_of(888.8f * 1024), max_ms(8888.8f)},
        {watts(888), mhz(8888), game_pct(100), game_gb(888.8f * 1024)},
    }};
    for (std::size_t c = 0; c < samples.size(); ++c) {
        columns[c + 1] = width_of(None());
        for (const QString& sample : samples[c]) {
            columns[c + 1] = std::max(columns[c + 1], width_of(sample));
        }
    }
    qreal table_width = 0;
    for (const qreal column : columns) {
        table_width += column + 2 * CellPaddingX;
    }

    struct Line {
        QString label;
        QString value;
    };
    std::vector<Line> waits;
    std::vector<Line> picture;
    std::vector<QString> names;
    const bool full = m_level >= 3;
    if (full) {
        waits = {
            {tr("GPU frame prep"), wait_ms(s.front_ms)},
            {tr("Vulkan recording per frame"), wait_ms(s.back_ms)},
            {tr("GPU busy per frame"), wait_ms(s.gpu_ms)},
            {tr("Busiest game core per frame"), wait_ms(s.core_ms)},
            {tr("Shaders building"),
             s.shaders_building < 0 ? None() : QString::number(s.shaders_building)},
        };
        const std::array<QString, 9> labels{
            tr("Window"),     tr("Game image"),  tr("Upscaling"),
            tr("Speed limit"), tr("VSync"),      tr("Display"),
            tr("Resolution"), tr("HDR output"), tr("Frame generation"),
        };
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (!s.picture[i].isEmpty()) {
                picture.push_back({labels[i], s.picture[i]});
            }
        }
        for (const QString& name : {s.gpu_name, s.cpu_name}) {
            if (!name.isEmpty()) {
                names.push_back(name);
            }
        }
    }

    // The frame rate on top, the main figure: "FPS", the average in a large font, and the 1% and
    // 0.1% lows stacked at the right in a font a little larger than the rest.
    QFont big_font = PanelFont();
    big_font.setPixelSize(static_cast<int>(BigFontPixels));
    big_font.setWeight(QFont::DemiBold);
    QFont low_font = PanelFont();
    low_font.setPixelSize(static_cast<int>(LowFontPixels));
    const QFontMetricsF big_metrics{big_font};
    const QFontMetricsF low_metrics{low_font};
    const QString fps_label = tr("FPS");
    const QString fps_value = fps(s.fps);
    // with frame generation on, the frames it adds per second below the lows
    std::vector<std::pair<QString, QString>> lows{
        {tr("1% low"), fps(s.low1_fps)},
        {tr("0.1% low"), fps(s.low01_fps)},
    };
    if (s.fg_fps > 0) {
        lows.push_back({QStringLiteral("Frame gen"), fps(s.fg_fps - s.fps)});
    }
    const auto low_width = [&](const std::pair<QString, QString>& low, const QString& number) {
        return low_metrics.horizontalAdvance(low.first + QLatin1Char(' ')) +
               low_metrics.horizontalAdvance(number);
    };
    qreal lows_width = 0;
    for (const auto& low : lows) {
        lows_width = std::max(lows_width, low_width(low, QStringLiteral("888")));
    }
    const qreal fps_left_width = low_metrics.horizontalAdvance(fps_label) + HeaderGap +
                                 big_metrics.horizontalAdvance(QStringLiteral("888"));
    const qreal header_height =
        std::max(big_metrics.height(),
                 static_cast<qreal>(lows.size()) * (low_metrics.height() + CellPaddingY));

    qreal content_width =
        std::max(table_width, fps_left_width + 2 * HeaderGap + lows_width + 2 * CellPaddingX);
    for (const auto* lines : {&waits, &picture}) {
        qreal label_width = 0;
        qreal value_width = 0;
        for (const Line& line : *lines) {
            label_width = std::max(label_width, width_of(line.label));
            value_width = std::max(value_width, width_of(line.value));
        }
        if (!lines->empty()) {
            content_width =
                std::max(content_width, label_width + value_width + 4 * CellPaddingX);
        }
    }
    for (const QString& name : names) {
        content_width = std::max(content_width, width_of(name));
    }

    const qreal line_height = metrics.height() + 2 * CellPaddingY;
    const qreal left = PaddingX;
    const qreal right = PaddingX + content_width;
    qreal y = PaddingY;

    const auto text_at = [&](qreal x, qreal top, const QString& text, const QColor& color) {
        if (painter) {
            painter->setPen(color);
            painter->drawText(QPointF{x, top + CellPaddingY + metrics.ascent()}, text);
        }
    };
    const auto cell_color = [](const QString& text) {
        return text == None() ? DimColor : ValueColor;
    };
    const auto table = [&](const std::vector<Row>& rows) {
        for (const Row& row : rows) {
            qreal x = left;
            text_at(x + CellPaddingX, y, row.label, LabelColor);
            x += columns[0] + 2 * CellPaddingX;
            for (std::size_t c = 0; c < row.cells.size(); ++c) {
                const QString& text = row.cells[c];
                text_at(x + CellPaddingX + columns[c + 1] - width_of(text), y, text,
                        cell_color(text));
                x += columns[c + 1] + 2 * CellPaddingX;
            }
            y += line_height;
        }
    };
    const auto lines = [&](const std::vector<Line>& rows) {
        for (const Line& row : rows) {
            text_at(left + CellPaddingX, y, row.label, LabelColor);
            text_at(right - CellPaddingX - width_of(row.value), y, row.value,
                    cell_color(row.value));
            y += line_height;
        }
    };
    const auto divider = [&]() {
        y += ItemGap;
        if (painter) {
            DrawDivider(*painter, left - 4, right + 4, y + DividerHeight() / 2);
        }
        y += DividerHeight() + ItemGap;
    };

    const qreal panel_width = content_width + 2 * PaddingX;
    if (painter) {
        // The panel's height is known once the layout ran without a painter: the widget's.
        DrawPanel(*painter, QRectF{0, 0, panel_width, static_cast<qreal>(height())}, 0.8, 0.94);
    }

    // The frame rate header.
    if (painter) {
        const qreal middle = y + header_height / 2;
        const qreal label_x = left + CellPaddingX;
        painter->setFont(low_font);
        painter->setPen(LabelColor);
        // Both centred on their capital and digit height, so the label sits level with the
        // number's middle.
        painter->drawText(QPointF{label_x, middle + low_metrics.capHeight() / 2}, fps_label);
        painter->setFont(big_font);
        painter->setPen(cell_color(fps_value));
        painter->drawText(QPointF{label_x + low_metrics.horizontalAdvance(fps_label) + HeaderGap,
                                  middle + big_metrics.capHeight() / 2},
                          fps_value);
        painter->setFont(low_font);
        const qreal low_line = low_metrics.height() + CellPaddingY;
        qreal low_top = middle - low_line * static_cast<qreal>(lows.size()) / 2;
        for (const auto& low : lows) {
            const QString label = low.first + QLatin1Char(' ');
            const qreal x = right - CellPaddingX - low_width(low, low.second);
            const qreal baseline = low_top + CellPaddingY / 2 + low_metrics.ascent();
            painter->setPen(LabelColor);
            painter->drawText(QPointF{x, baseline}, label);
            painter->setPen(cell_color(low.second));
            painter->drawText(QPointF{x + low_metrics.horizontalAdvance(label), baseline},
                              low.second);
            low_top += low_line;
        }
        painter->setFont(font);
    }
    y += header_height;
    divider();

    table(machine_rows);
    divider();
    table(frame_rows);

    // The frame times as a gold line over a dark band; frames not seen yet read 0.
    y += ItemGap * 2;
    const QRectF plot{left, y, content_width, PlotHeight};
    if (painter) {
        painter->fillRect(plot, QColor{0, 0, 0, 82});
        const float top = std::max(50.0f, s.frame_ms_max);
        const QRectF inner = plot.adjusted(4, 3, -4, -3);
        QPolygonF points;
        points.reserve(static_cast<qsizetype>(PlotFrames));
        for (std::size_t i = 0; i < PlotFrames; ++i) {
            const float v = std::clamp(s.frame_ms[i], 0.0f, top);
            points << QPointF{inner.left() + inner.width() * i / (PlotFrames - 1),
                              inner.bottom() - inner.height() * v / top};
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen{LabelColor, 1.0});
        painter->drawPolyline(points);
        painter->restore();
    }
    y += PlotHeight;

    if (full) {
        y += ItemGap * 2;
        lines(waits);
        divider();
        lines(picture);
        if (!names.empty()) {
            divider();
            for (const QString& name : names) {
                text_at(left, y, name, DimColor);
                y += line_height;
            }
        }
    }
    y += PaddingY;
    return QSize{static_cast<int>(std::ceil(panel_width)), static_cast<int>(std::ceil(y))};
}

void DetailedPerformanceOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    layout(&painter);
}

void DetailedPerformanceOverlay::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (QtCommon::system && QtCommon::system->IsPoweredOn()) {
        QtCommon::system->GetPerfStats().SetFrameTimeHistoryEnabled(false);
    }
    const QString cpu_name = m_stats.cpu_name;
    m_stats = {};
    m_stats.cpu_name = cpu_name;
    m_cpuSnapshot.reset();
}

void DetailedPerformanceOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    // Wayland ignores move() on top-level windows: let the compositor drag it instead.
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) && windowHandle() &&
        windowHandle()->startSystemMove()) {
        return;
    }
    m_drag_start_pos = event->pos();
}

void DetailedPerformanceOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        const QPoint new_global_pos = event->globalPosition().toPoint() - m_drag_start_pos;
        m_offset = new_global_pos - m_mainWindow->pos();
        move(new_global_pos);
    }
}
