// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "performance_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <numeric>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QStatusBar>
#include <QStringList>
#include <QWindow>

#include "common/memory_detect.h"
#include "common/settings.h"
#include "core/core.h"
#include "main_window.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace {

using Common::DkbCpuTrace::Role;

// Layout of the "6 · 3 + big number" design at 100% scale; Qt scales it with the DPI.
constexpr int OverlayWidth = 300;
constexpr int OverlayHeight = 158;
constexpr qreal PaddingX = 10;
constexpr qreal PaddingTop = 6;
constexpr qreal ContentWidth = 280;
constexpr qreal HeaderHeight = 44;
constexpr qreal GraphTop = PaddingTop + HeaderHeight + 4;
constexpr qreal GraphHeight = 72;
constexpr qreal PlotWidth = 236;
constexpr qreal AxisX = 240;
constexpr qreal LabelX = 247;
constexpr qreal LabelHeight = 14;
constexpr qreal FooterTop = GraphTop + GraphHeight + 10;

// The minimal style: the fps number and the frame time on one line.
constexpr int MinimalWidth = 176;
constexpr int MinimalHeight = 40;

// The full style: the standard one with the memory rows between the header and the graph.
constexpr qreal FullRowHeight = 22;
constexpr qreal FullRowsHeight = FullRowHeight * 2 + 4;
constexpr int FullWidth = OverlayWidth;
constexpr int FullHeight = OverlayHeight + static_cast<int>(FullRowsHeight);
/// Less free memory than this is a warning.
constexpr u64 LowMemory = u64{1} << 30;
constexpr u64 LowVideoMemory = u64{512} << 20;

/// Gap between the overlay and the edges of the main window's content.
constexpr int Margin = 20;

constexpr auto HistoryWindow = std::chrono::seconds{5};
constexpr auto StatsWindow = std::chrono::milliseconds{500};
constexpr std::size_t FpsSamples = 10;
/// A frame this much slower than the median is a spike.
constexpr float SpikeRatio = 1.2f;

constexpr QRgb TextColor = qRgb(0xF2, 0xF3, 0xF5);
constexpr QRgb MutedColor = qRgb(0xA0, 0xA5, 0xAD);
constexpr QRgb DimColor = qRgb(0x6E, 0x73, 0x7B);
constexpr QRgb AccentColor = qRgb(0x5E, 0xB4, 0xFF);
constexpr QRgb SpikeColor = qRgb(0xFF, 0x5D, 0x52);
constexpr QRgb BackgroundColor = qRgba(0, 0, 0, 140);
constexpr QRgb LineColor = qRgba(0xF2, 0xF3, 0xF5, 217);
constexpr QRgb AxisColor = qRgba(255, 255, 255, 46);
constexpr QRgb MedianLineColor = qRgba(255, 255, 255, 56);
constexpr QRgb WarningColor = qRgb(0xF5, 0xA7, 0x42);

/// The busiest of these threads is the CPU side's limit: the guest cores and GPU emulation.
constexpr std::array CpuRoles{
    Role::GuestCore0, Role::GuestCore1,      Role::GuestCore2,  Role::GuestCore3, Role::GuestCore4,
    Role::GuestCore5, Role::GuestSingleCore, Role::GpuCommand, Role::GpuBack,
};

QFont MonoFont(int pixel_size, QFont::Weight weight = QFont::Normal) {
    QFont font;
    font.setFamilies({QStringLiteral("Geist Mono"), QStringLiteral("Cascadia Mono"),
                      QStringLiteral("JetBrains Mono"), QStringLiteral("Consolas"),
                      QStringLiteral("SF Mono"), QStringLiteral("Menlo"),
                      QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Liberation Mono")});
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(pixel_size);
    font.setWeight(weight);
    font.setLetterSpacing(QFont::AbsoluteSpacing, (pixel_size >= 40 ? -0.03 : -0.01) * pixel_size);
    return font;
}

/// The baseline of text centred in a line box, as CSS places it.
qreal Baseline(const QFont& font, qreal top, qreal line_height) {
    const QFontMetricsF metrics{font};
    return top + (line_height - metrics.ascent() - metrics.descent()) / 2 + metrics.ascent();
}

struct Run {
    QString text;
    QFont font;
    QColor color;
};

qreal Width(const std::vector<Run>& runs) {
    qreal width = 0;
    for (const Run& run : runs) {
        width += QFontMetricsF{run.font}.horizontalAdvance(run.text);
    }
    return width;
}

void DrawRuns(QPainter& painter, qreal x, qreal baseline, const std::vector<Run>& runs) {
    for (const Run& run : runs) {
        painter.setFont(run.font);
        painter.setPen(run.color);
        painter.drawText(QPointF{x, baseline}, run.text);
        x += QFontMetricsF{run.font}.horizontalAdvance(run.text);
    }
}

QString Percent(const std::optional<double>& percent) {
    return percent ? QStringLiteral("%1%").arg(std::round(*percent), 0, 'f', 0)
                   : QStringLiteral("—");
}

QString Gigabytes(u64 bytes) {
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0,
                                       'f', 1);
}

/// The frame rate of the slowest hundredth of the frames.
std::optional<double> LowFps(const std::vector<Core::FrameTimeHistory::Entry>& frames) {
    if (frames.size() < 2) {
        return std::nullopt;
    }
    std::vector<float> frame_ms;
    frame_ms.reserve(frames.size());
    for (const auto& frame : frames) {
        frame_ms.push_back(frame.ms);
    }
    const std::size_t count = std::max<std::size_t>(1, frame_ms.size() / 100);
    const auto slowest_end = frame_ms.begin() + static_cast<std::ptrdiff_t>(count);
    std::partial_sort(frame_ms.begin(), slowest_end, frame_ms.end(), std::greater<>{});
    const double sum = std::accumulate(frame_ms.begin(), slowest_end, 0.0);
    if (sum <= 0) {
        return std::nullopt;
    }
    return 1000.0 * static_cast<double>(count) / sum;
}

} // namespace

PerformanceOverlay::PerformanceOverlay(MainWindow* parent) : QWidget(parent), m_mainWindow{parent} {
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    ResetThemeColors();
    raise();

    // pos/stats
    ApplySettings();
    connect(parent, &MainWindow::positionChanged, this, &PerformanceOverlay::resetPosition);
    connect(parent, &MainWindow::sizeChanged, this, [this] { resetPosition({}); });
    connect(m_mainWindow, &MainWindow::statsUpdated, this, &PerformanceOverlay::updateStats);
}

void PerformanceOverlay::ApplySettings() {
    using UISettings::PerfOverlayPosition;
    using UISettings::PerfOverlayStyle;

    const auto style = static_cast<PerfOverlayStyle>(
        std::min(UISettings::values.perf_overlay_style.GetValue(),
                 static_cast<u32>(PerfOverlayStyle::Full)));
    const auto position = static_cast<PerfOverlayPosition>(
        std::min(UISettings::values.perf_overlay_position.GetValue(),
                 static_cast<u32>(PerfOverlayPosition::BottomRight)));

    // A new corner or a new size starts from the corner again; otherwise a drag is kept.
    if (style != m_style || position != m_position) {
        m_dragOffset = {};
    }
    m_style = style;
    m_position = position;

    switch (m_style) {
    case PerfOverlayStyle::Minimal:
        setFixedSize(MinimalWidth, MinimalHeight);
        break;
    case PerfOverlayStyle::Standard:
        setFixedSize(OverlayWidth, OverlayHeight);
        break;
    case PerfOverlayStyle::Full:
        setFixedSize(FullWidth, FullHeight);
        break;
    }
    resetPosition({});
    update();
}

void PerformanceOverlay::ResetThemeColors() {
    m_backgroundColor = QColor::fromRgba(BackgroundColor);
    m_borderColor = QColor{Qt::transparent};
    m_textColor = QColor::fromRgb(TextColor);
    m_mutedColor = QColor::fromRgb(MutedColor);
    m_dimColor = QColor::fromRgb(DimColor);
    m_accentColor = QColor::fromRgb(AccentColor);
    m_spikeColor = QColor::fromRgb(SpikeColor);
    m_lineColor = QColor::fromRgba(LineColor);
    m_axisColor = QColor::fromRgba(AxisColor);
    m_medianLineColor = QColor::fromRgba(MedianLineColor);
    m_warningColor = QColor::fromRgb(WarningColor);
    m_cornerRadius = 5.0;
    update();
}

QPoint PerformanceOverlay::AnchorPosition() const {
    using UISettings::PerfOverlayPosition;

    // The main window's content, between the menu bar and the status bar, in screen coordinates.
    QRect area{m_mainWindow->mapToGlobal(QPoint{0, 0}), m_mainWindow->size()};
    if (const QWidget* menu_bar = m_mainWindow->menuBar(); menu_bar->isVisible()) {
        area.setTop(area.top() + menu_bar->height());
    }
    if (const QWidget* status_bar = m_mainWindow->statusBar(); status_bar->isVisible()) {
        area.setBottom(area.bottom() - status_bar->height());
    }

    const bool right = m_position == PerfOverlayPosition::TopRight ||
                       m_position == PerfOverlayPosition::BottomRight;
    const bool bottom = m_position == PerfOverlayPosition::BottomLeft ||
                        m_position == PerfOverlayPosition::BottomRight;
    return QPoint{
        right ? area.right() + 1 - Margin - width() : area.left() + Margin,
        bottom ? area.bottom() + 1 - Margin - height() : area.top() + Margin,
    };
}

void PerformanceOverlay::resetPosition(const QPoint& _) {
    move(AnchorPosition() + m_dragOffset);
}

void PerformanceOverlay::updateStats(const Core::PerfStatsResults& results,
                                     const VideoCore::ShaderNotify& shaders) {
    auto& system = *QtCommon::system;
    auto& perf_stats = system.GetPerfStats();
    perf_stats.SetFrameTimeHistoryEnabled(isVisible());
    if (!isVisible()) {
        return;
    }

    m_fps.reset();
    if (!std::isnan(results.average_game_fps)) {
        m_fps = results.average_game_fps;
        m_fpsSamples.push_back(static_cast<float>(std::round(*m_fps)));
        if (m_fpsSamples.size() > FpsSamples) {
            m_fpsSamples.pop_front();
        }
    }

    m_frames = perf_stats.GetRecentFrameTimes(HistoryWindow);
    m_frameMs.reset();
    if (!m_frames.empty() &&
        Core::FrameTimeHistory::Clock::now() - m_frames.back().end < std::chrono::seconds{1}) {
        double sum = 0;
        int count = 0;
        for (auto it = m_frames.rbegin();
             it != m_frames.rend() && m_frames.back().end - it->end < StatsWindow; ++it) {
            sum += it->ms;
            ++count;
        }
        m_frameMs = sum / count;
    }

    // Busy time over the window against the window is busy ms per frame against frame time.
    auto cpu_snapshot = Common::DkbCpuTrace::ReadSnapshot();
    m_cpuPercent.reset();
    if (m_cpuSnapshot) {
        const double wall_ns = std::chrono::duration<double, std::nano>(
                                   cpu_snapshot.wall_time - m_cpuSnapshot->wall_time)
                                   .count();
        std::optional<u64> busiest_ns;
        for (const Role role : CpuRoles) {
            const auto index = static_cast<size_t>(role);
            if (const auto delta = Common::DkbCpuTrace::CpuTimeDelta(m_cpuSnapshot->threads[index],
                                                                     cpu_snapshot.threads[index])) {
                busiest_ns = std::max(busiest_ns.value_or(0), *delta);
            }
        }
        if (busiest_ns && wall_ns > 0) {
            m_cpuPercent = std::min(100.0, static_cast<double>(*busiest_ns) / wall_ns * 100.0);
        }
    }
    m_cpuSnapshot = cpu_snapshot;

    auto& renderer = system.GPU().Renderer();
    renderer.RequestGpuBusySample();
    const auto gpu_ms = renderer.GetGpuBusyMs();
    m_gpuPercent.reset();
    if (gpu_ms && m_frameMs && *m_frameMs > 0) {
        m_gpuPercent = std::min(100.0, *gpu_ms / *m_frameMs * 100.0);
    }

    m_memoryBytes.reset();
    m_memoryFree.reset();
    m_vramUsed.reset();
    m_vramFree.reset();
    if (m_style == UISettings::PerfOverlayStyle::Full) {
        // What the task manager shows; hosts without it report the resident memory.
        const auto usage = Common::GetProcessMemoryUsage();
        const u64 bytes =
            usage.private_working_set != 0 ? usage.private_working_set : usage.working_set;
        if (bytes != 0) {
            m_memoryBytes = bytes;
        }
        if (const u64 available = Common::GetAvailablePhysicalMemory(); available != 0) {
            m_memoryFree = available;
        }
        if (const auto device_memory = renderer.GetDeviceMemory()) {
            m_vramUsed = device_memory->used;
            m_vramFree = device_memory->available;
        }
    }

    QStringList tags;
    if (const QString& dlss = m_mainWindow->DlssStatusText(); !dlss.isEmpty()) {
        tags << dlss;
    }
    switch (Settings::values.scaling_filter.GetValue()) {
    case Settings::ScalingFilter::Fsr:
        tags << QStringLiteral("FSR");
        break;
    case Settings::ScalingFilter::Fsr3:
        tags << QStringLiteral("FSR 3");
        break;
    case Settings::ScalingFilter::Sgsr:
    case Settings::ScalingFilter::SgsrEdge:
        tags << QStringLiteral("SGSR");
        break;
    default:
        break;
    }
    if (const auto& scaling = Settings::values.resolution_info; scaling.active) {
        tags << QStringLiteral("%1×").arg(scaling.up_factor, 0, 'g', 3);
    }
    if (const double generated = m_mainWindow->FrameGenFps(); generated >= 0.5) {
        tags << tr("FG +%1").arg(std::round(generated), 0, 'f', 0);
    }
    m_tags = tags.join(QStringLiteral("  "));

    update();
}

void PerformanceOverlay::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    painter.setBrush(m_backgroundColor);
    if (m_borderColor.alpha() > 0) {
        painter.setPen(QPen{m_borderColor, 1.0});
        painter.drawRoundedRect(QRectF{rect()}.adjusted(0.5, 0.5, -0.5, -0.5), m_cornerRadius,
                                m_cornerRadius);
    } else {
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(rect(), m_cornerRadius, m_cornerRadius);
    }

    switch (m_style) {
    case UISettings::PerfOverlayStyle::Minimal:
        paintMinimal(painter);
        break;
    case UISettings::PerfOverlayStyle::Standard:
        paintDetailed(painter, false);
        break;
    case UISettings::PerfOverlayStyle::Full:
        paintDetailed(painter, true);
        break;
    }
}

void PerformanceOverlay::paintMinimal(QPainter& painter) {
    const QFont fps_font = MonoFont(24, QFont::DemiBold);
    const QFont small_font = MonoFont(13);
    const QFont small_bold_font = MonoFont(13, QFont::DemiBold);

    // The fps number on the left, the frame time on the right, on one baseline.
    const qreal baseline = Baseline(fps_font, 0, MinimalHeight);
    const std::vector<Run> fps_runs{
        {m_fps ? QString::number(std::round(*m_fps), 'f', 0) : QStringLiteral("—"), fps_font,
         m_textColor},
    };
    DrawRuns(painter, PaddingX, baseline, fps_runs);
    DrawRuns(painter, PaddingX + Width(fps_runs) + 5, baseline,
             {{QStringLiteral("fps"), small_font, m_mutedColor}});

    const std::vector<Run> ms_runs{
        {m_frameMs ? QString::number(*m_frameMs, 'f', 1) : QStringLiteral("—"), small_bold_font,
         m_textColor},
        {QStringLiteral(" ms"), small_font, m_mutedColor},
    };
    DrawRuns(painter, MinimalWidth - PaddingX - Width(ms_runs), baseline, ms_runs);
}

void PerformanceOverlay::paintDetailed(QPainter& painter, bool full) {
    const QColor text = m_textColor;
    const QColor muted = m_mutedColor;
    const QColor accent = m_accentColor;
    const QFont fps_font = MonoFont(40, QFont::DemiBold);
    const QFont small_font = MonoFont(13);
    const QFont small_bold_font = MonoFont(13, QFont::DemiBold);
    const QFont footer_font = MonoFont(11);

    // Header: the big fps number on the left, frame time and CPU/GPU % on the right.
    const qreal fps_baseline = Baseline(fps_font, PaddingTop, HeaderHeight);
    const std::vector<Run> fps_runs{
        {m_fps ? QString::number(std::round(*m_fps), 'f', 0) : QStringLiteral("—"), fps_font, text},
    };
    DrawRuns(painter, PaddingX, fps_baseline, fps_runs);
    DrawRuns(painter, PaddingX + Width(fps_runs) + 6, fps_baseline,
             {{QStringLiteral("fps"), small_font, muted}});

    const qreal right = PaddingX + ContentWidth;
    const qreal right_top = PaddingTop + (HeaderHeight - 34) / 2;
    const std::vector<Run> ms_runs{
        {m_frameMs ? QString::number(*m_frameMs, 'f', 1) : QStringLiteral("—"), small_bold_font,
         text},
        {QStringLiteral(" ms"), small_font, muted},
    };
    DrawRuns(painter, right - Width(ms_runs), Baseline(small_font, right_top, 17), ms_runs);

    // The higher percentage is the limit and gets the accent.
    const bool both = m_cpuPercent && m_gpuPercent;
    const bool cpu_limit = both && std::round(*m_cpuPercent) >= std::round(*m_gpuPercent);
    const bool gpu_limit = both && !cpu_limit;
    const std::vector<Run> load_runs{
        {QStringLiteral("CPU ") + Percent(m_cpuPercent), cpu_limit ? small_bold_font : small_font,
         cpu_limit ? accent : muted},
        {QStringLiteral("  "), small_font, muted},
        {QStringLiteral("GPU ") + Percent(m_gpuPercent), gpu_limit ? small_bold_font : small_font,
         gpu_limit ? accent : muted},
    };
    DrawRuns(painter, right - Width(load_runs), Baseline(small_font, right_top + 17, 17),
             load_runs);

    // The full style's memory rows: what the emulator uses on the left, what is left on the right.
    qreal offset = 0;
    if (full) {
        const QFont value_font = MonoFont(14, QFont::DemiBold);
        const auto memory_row = [&](const QString& name, const std::optional<u64>& used,
                                    const std::optional<u64>& available, u64 low) {
            const qreal baseline =
                Baseline(value_font, PaddingTop + HeaderHeight + 2 + offset, FullRowHeight);
            DrawRuns(painter, PaddingX, baseline,
                     {{used ? Gigabytes(*used) : QStringLiteral("—"), value_font, text},
                      {name, small_font, muted}});
            if (available) {
                const std::vector<Run> free_runs{
                    {tr("%1 free").arg(Gigabytes(*available)), small_font,
                     *available < low ? m_warningColor : muted},
                };
                DrawRuns(painter, right - Width(free_runs), baseline, free_runs);
            }
            offset += FullRowHeight;
        };
        memory_row(QStringLiteral(" RAM"), m_memoryBytes, m_memoryFree, LowMemory);
        memory_row(QStringLiteral(" VRAM"), m_vramUsed, m_vramFree, LowVideoMemory);
        offset = FullRowsHeight;
    }

    paintGraph(painter, QRectF{PaddingX, GraphTop + offset, ContentWidth, GraphHeight});

    // Footer: fps min/median/max over the last 5 s, and the features that are on.
    QString fps_range = QStringLiteral("— — —");
    const std::vector<float> fps_samples(m_fpsSamples.begin(), m_fpsSamples.end());
    if (const auto summary = Core::Summarize(fps_samples)) {
        fps_range = QStringLiteral("%1 %2 %3")
                        .arg(summary->min_ms, 0, 'f', 0)
                        .arg(std::round(summary->median_ms), 0, 'f', 0)
                        .arg(summary->max_ms, 0, 'f', 0);
    }
    const qreal footer_baseline = Baseline(footer_font, FooterTop + offset, LabelHeight);
    std::vector<Run> range_runs{
        {fps_range, footer_font, muted},
        {QStringLiteral(" fps"), footer_font, m_dimColor},
    };
    if (full) {
        const auto low_fps = LowFps(m_frames);
        range_runs.push_back({tr("  low "), footer_font, m_dimColor});
        range_runs.push_back(
            {low_fps ? QString::number(std::round(*low_fps), 'f', 0) : QStringLiteral("—"),
             footer_font, muted});
    }
    DrawRuns(painter, PaddingX, footer_baseline, range_runs);

    const qreal tags_width = ContentWidth - Width(range_runs) - 12;
    const QString tags =
        QFontMetricsF{footer_font}.elidedText(m_tags, Qt::ElideRight, tags_width);
    DrawRuns(painter, right - QFontMetricsF{footer_font}.horizontalAdvance(tags), footer_baseline,
             {{tags, footer_font, muted}});
}

void PerformanceOverlay::paintGraph(QPainter& painter, const QRectF& area) {
    painter.fillRect(QRectF{area.left() + AxisX, area.top(), 1, area.height()},
                     m_axisColor);
    if (m_frames.size() < 2) {
        return;
    }

    std::vector<float> frame_ms;
    frame_ms.reserve(m_frames.size());
    for (const auto& frame : m_frames) {
        frame_ms.push_back(frame.ms);
    }
    const auto summary = *Core::Summarize(frame_ms);

    // Min and max span the plot; a steady frame rate keeps a small span so jitter stays flat.
    float low = summary.min_ms;
    float high = summary.max_ms;
    const float min_span = std::max(2.0f, summary.median_ms * 0.1f);
    if (high - low < min_span) {
        const float middle = (low + high) / 2;
        low = middle - min_span / 2;
        high = middle + min_span / 2;
    }
    const qreal plot_top = area.top() + 6;
    const qreal plot_bottom = area.bottom() - 7;
    const auto y_of = [&](float ms) {
        return plot_top + (high - ms) / (high - low) * (plot_bottom - plot_top);
    };
    const auto newest = m_frames.back().end;
    const double window_s = std::chrono::duration<double>(HistoryWindow).count();

    QPolygonF line;
    line.reserve(static_cast<qsizetype>(m_frames.size()));
    for (const auto& frame : m_frames) {
        const double age_s = std::chrono::duration<double>(newest - frame.end).count();
        line << QPointF{area.left() + PlotWidth * (1.0 - age_s / window_s), y_of(frame.ms)};
    }

    painter.setBrush(Qt::NoBrush);
    QPen median_pen{m_medianLineColor, 0.75};
    median_pen.setDashPattern({2.0 / 0.75, 3.0 / 0.75});
    painter.setPen(median_pen);
    const qreal median_y = y_of(summary.median_ms);
    painter.drawLine(QPointF{area.left(), median_y}, QPointF{area.left() + PlotWidth, median_y});

    QPen line_pen{m_lineColor, 1.1};
    line_pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(line_pen);
    painter.drawPolyline(line);

    const float spike_ms = summary.median_ms * SpikeRatio;
    QPen spike_pen{m_spikeColor, 1.1};
    spike_pen.setCapStyle(Qt::RoundCap);
    spike_pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(spike_pen);
    for (qsizetype i = 1; i < line.size(); ++i) {
        if (frame_ms[i - 1] > spike_ms || frame_ms[i] > spike_ms) {
            painter.drawLine(line[i - 1], line[i]);
        }
    }

    // Min, median and max as ticks on the axis at their real heights, labelled beside them.
    struct Tick {
        float ms;
        QColor color;
        QFont font;
    };
    const std::array<Tick, 3> ticks{{
        {summary.max_ms,
         summary.max_ms > spike_ms ? m_spikeColor : m_textColor,
         MonoFont(11, QFont::Medium)},
        {summary.median_ms, m_textColor, MonoFont(11, QFont::Medium)},
        {summary.min_ms, m_mutedColor, MonoFont(11)},
    }};
    std::array<qreal, 3> label_tops{};
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const qreal y = y_of(ticks[i].ms);
        painter.fillRect(QRectF{area.left() + AxisX - 2, y - 0.5, 5, 1}, ticks[i].color);
        label_tops[i] = y - LabelHeight / 2;
    }
    // Push overlapping labels apart, keeping them within the graph and the footer's padding.
    for (std::size_t i = 1; i < label_tops.size(); ++i) {
        label_tops[i] = std::max(label_tops[i], label_tops[i - 1] + LabelHeight);
    }
    label_tops[2] = std::min(label_tops[2], area.bottom() + 7 - LabelHeight);
    for (std::size_t i = label_tops.size() - 1; i-- > 0;) {
        label_tops[i] = std::min(label_tops[i], label_tops[i + 1] - LabelHeight);
    }
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        DrawRuns(painter, area.left() + LabelX,
                 Baseline(ticks[i].font, label_tops[i], LabelHeight),
                 {{QString::number(ticks[i].ms, 'f', 1), ticks[i].font, ticks[i].color}});
    }
}

void PerformanceOverlay::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (QtCommon::system && QtCommon::system->IsPoweredOn()) {
        QtCommon::system->GetPerfStats().SetFrameTimeHistoryEnabled(false);
    }
    m_fps.reset();
    m_fpsSamples.clear();
    m_frameMs.reset();
    m_cpuPercent.reset();
    m_gpuPercent.reset();
    m_memoryBytes.reset();
    m_memoryFree.reset();
    m_vramUsed.reset();
    m_vramFree.reset();
    m_cpuSnapshot.reset();
    m_frames.clear();
}

void PerformanceOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    resetPosition({});
}

void PerformanceOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    // Wayland ignores move() on top-level windows, so the overlay sat where the compositor put it:
    // let the compositor drag it instead.
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) && windowHandle() &&
        windowHandle()->startSystemMove()) {
        return;
    }
    m_drag_start_pos = event->pos();
}

void PerformanceOverlay::mouseMoveEvent(QMouseEvent* event) {
    // drag
    if (event->buttons() & Qt::LeftButton) {
        QPoint new_global_pos = event->globalPosition().toPoint() - m_drag_start_pos;
        m_dragOffset = new_global_pos - AnchorPosition();
        move(new_global_pos);
    }
}

void PerformanceOverlay::closeEvent(QCloseEvent* event) {
    emit closed();
}
