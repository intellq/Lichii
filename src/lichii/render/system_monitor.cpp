// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system_monitor.h"

#include <chrono>
#include <optional>
#include <string>

#ifdef __linux__
#include <dlfcn.h>
#include <unistd.h>
#include <QDir>
#include <QFile>
#include <QStringList>
#endif

#ifdef __linux__
namespace {

std::optional<QByteArray> ReadFile(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

std::optional<double> ReadNumber(const QString& path) {
    const auto data = ReadFile(path);
    if (!data) {
        return std::nullopt;
    }
    bool ok = false;
    const double value = data->trimmed().toDouble(&ok);
    return ok ? std::optional<double>{value} : std::nullopt;
}

/// The value of a "Key:   123 kB" line of /proc/meminfo or /proc/self/status, in kB
std::optional<double> ReadKbField(const QByteArray& text, const QByteArray& key) {
    for (const QByteArray& line : text.split('\n')) {
        if (line.startsWith(key)) {
            const QList<QByteArray> parts = line.mid(key.size()).simplified().split(' ');
            bool ok = false;
            const double value = parts.value(0).toDouble(&ok);
            if (ok) {
                return value;
            }
        }
    }
    return std::nullopt;
}

/// The first file of a hwmon directory under `dir` named `name`, empty when none
QString HwmonFile(const QString& dir, const QString& name) {
    const QDir hwmon{dir + QStringLiteral("/hwmon")};
    for (const QString& entry : hwmon.entryList({QStringLiteral("hwmon*")}, QDir::Dirs)) {
        const QString path = hwmon.filePath(entry) + QLatin1Char('/') + name;
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

// NVML, loaded at run time when the GPU is NVIDIA's (no amdgpu card).
struct NvmlUtilization {
    unsigned gpu;
    unsigned memory;
};
struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct Nvml {
    void* lib = nullptr;
    void* device = nullptr;
    int (*utilization)(void*, NvmlUtilization*) = nullptr;
    int (*temperature)(void*, int, unsigned*) = nullptr;
    int (*memory)(void*, NvmlMemory*) = nullptr;
    int (*power)(void*, unsigned*) = nullptr;
    int (*clock)(void*, int, unsigned*) = nullptr;

    bool Open() {
        lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            return false;
        }
        const auto init = reinterpret_cast<int (*)()>(dlsym(lib, "nvmlInit_v2"));
        const auto handle =
            reinterpret_cast<int (*)(unsigned, void**)>(dlsym(lib, "nvmlDeviceGetHandleByIndex_v2"));
        utilization = reinterpret_cast<decltype(utilization)>(
            dlsym(lib, "nvmlDeviceGetUtilizationRates"));
        temperature =
            reinterpret_cast<decltype(temperature)>(dlsym(lib, "nvmlDeviceGetTemperature"));
        memory = reinterpret_cast<decltype(memory)>(dlsym(lib, "nvmlDeviceGetMemoryInfo"));
        power = reinterpret_cast<decltype(power)>(dlsym(lib, "nvmlDeviceGetPowerUsage"));
        clock = reinterpret_cast<decltype(clock)>(dlsym(lib, "nvmlDeviceGetClockInfo"));
        if (!init || !handle || init() != 0 || handle(0, &device) != 0) {
            dlclose(lib);
            lib = nullptr;
            device = nullptr;
            return false;
        }
        return true;
    }

    void Sample(MachineStats& stats) const {
        if (!device) {
            return;
        }
        NvmlUtilization util{};
        if (utilization && utilization(device, &util) == 0) {
            stats.gpu_pct = static_cast<float>(util.gpu);
        }
        unsigned value = 0;
        if (temperature && temperature(device, 0 /* NVML_TEMPERATURE_GPU */, &value) == 0) {
            stats.gpu_temp_c = static_cast<float>(value);
        }
        if (power && power(device, &value) == 0) {
            stats.gpu_power_w = static_cast<float>(value) / 1000.0f;
        }
        if (clock && clock(device, 0 /* NVML_CLOCK_GRAPHICS */, &value) == 0) {
            stats.gpu_clock_mhz = static_cast<float>(value);
        }
        NvmlMemory mem{};
        if (memory && memory(device, &mem) == 0) {
            stats.vram_used_mb = static_cast<float>(mem.used / (1024.0 * 1024.0));
            stats.vram_total_mb = static_cast<float>(mem.total / (1024.0 * 1024.0));
        }
    }
};

} // namespace

struct SystemMonitor::Impl {
    using Clock = std::chrono::steady_clock;

    QString cpu_temp;    ///< the CPU package's hwmon temp1_input, empty when unknown
    QString gpu_device;  ///< /sys/class/drm/cardN/device of an amdgpu card, empty when none
    QString gpu_temp;    ///< its hwmon edge temperature
    QString gpu_power;   ///< its hwmon power1_average, else power1_input (microwatts)
    QString gpu_clock;   ///< its hwmon freq1_input, the shader clock (Hz)
    Nvml nvml;
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    long processors = sysconf(_SC_NPROCESSORS_ONLN);

    std::optional<double> last_busy;
    std::optional<double> last_total;
    std::optional<double> last_game_ticks;
    std::optional<Clock::time_point> last_time;

    Impl() {
        // The CPU package: k10temp (AMD), coretemp (Intel) or zenpower.
        const QDir hwmon{QStringLiteral("/sys/class/hwmon")};
        for (const QString& entry : hwmon.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString dir = hwmon.filePath(entry);
            const auto name = ReadFile(dir + QStringLiteral("/name"));
            if (!name) {
                continue;
            }
            const QByteArray trimmed = name->trimmed();
            if ((trimmed == "k10temp" || trimmed == "coretemp" || trimmed == "zenpower") &&
                QFile::exists(dir + QStringLiteral("/temp1_input"))) {
                cpu_temp = dir + QStringLiteral("/temp1_input");
                break;
            }
        }

        // The amdgpu card: the one with gpu_busy_percent. Its hwmon has the edge temperature,
        // the board power and the shader clock, which the GPU load alone left out.
        const QDir drm{QStringLiteral("/sys/class/drm")};
        for (const QString& entry : drm.entryList({QStringLiteral("card*")}, QDir::Dirs)) {
            if (entry.contains(QLatin1Char('-'))) {
                continue;
            }
            const QString device = drm.filePath(entry) + QStringLiteral("/device");
            if (QFile::exists(device + QStringLiteral("/gpu_busy_percent"))) {
                gpu_device = device;
                break;
            }
        }
        if (!gpu_device.isEmpty()) {
            gpu_temp = HwmonFile(gpu_device, QStringLiteral("temp1_input"));
            gpu_power = HwmonFile(gpu_device, QStringLiteral("power1_average"));
            if (gpu_power.isEmpty()) {
                gpu_power = HwmonFile(gpu_device, QStringLiteral("power1_input"));
            }
            gpu_clock = HwmonFile(gpu_device, QStringLiteral("freq1_input"));
        } else {
            nvml.Open();
        }
    }

    ~Impl() {
        if (nvml.lib) {
            if (const auto shutdown = reinterpret_cast<int (*)()>(dlsym(nvml.lib, "nvmlShutdown"))) {
                shutdown();
            }
            dlclose(nvml.lib);
        }
    }

    void SampleCpu(MachineStats& stats) {
        const auto now = Clock::now();
        if (const auto stat = ReadFile(QStringLiteral("/proc/stat"))) {
            // cpu  user nice system idle iowait irq softirq steal
            const QList<QByteArray> fields = stat->left(stat->indexOf('\n')).simplified().split(' ');
            double total = 0;
            double idle = 0;
            for (qsizetype i = 1; i < fields.size() && i <= 8; ++i) {
                const double value = fields[i].toDouble();
                total += value;
                if (i == 4 || i == 5) {
                    idle += value;
                }
            }
            const double busy = total - idle;
            if (last_total && total > *last_total) {
                stats.cpu_total_pct =
                    static_cast<float>((busy - *last_busy) / (total - *last_total) * 100.0);
            }
            last_total = total;
            last_busy = busy;
        }
        if (const auto stat = ReadFile(QStringLiteral("/proc/self/stat"))) {
            // The fields after the command name, which ends at the last ')': utime and stime are
            // the 14th and 15th of the line.
            const QList<QByteArray> fields =
                stat->mid(stat->lastIndexOf(')') + 2).simplified().split(' ');
            if (fields.size() > 12) {
                const double ticks = fields[11].toDouble() + fields[12].toDouble();
                if (last_game_ticks && last_time && ticks_per_second > 0 && processors > 0) {
                    const double wall = std::chrono::duration<double>(now - *last_time).count();
                    if (wall > 0) {
                        stats.cpu_game_pct =
                            static_cast<float>((ticks - *last_game_ticks) / ticks_per_second /
                                               (wall * processors) * 100.0);
                    }
                }
                last_game_ticks = ticks;
            }
        }
        last_time = now;
        if (!cpu_temp.isEmpty()) {
            if (const auto milli = ReadNumber(cpu_temp)) {
                stats.cpu_temp_c = static_cast<float>(*milli / 1000.0);
            }
        }
    }

    void SampleMemory(MachineStats& stats) {
        if (const auto info = ReadFile(QStringLiteral("/proc/meminfo"))) {
            const auto total = ReadKbField(*info, "MemTotal:");
            const auto available = ReadKbField(*info, "MemAvailable:");
            if (total) {
                stats.ram_total_mb = static_cast<float>(*total / 1024.0);
                if (available) {
                    stats.ram_used_mb = static_cast<float>((*total - *available) / 1024.0);
                }
            }
        }
        if (const auto status = ReadFile(QStringLiteral("/proc/self/status"))) {
            if (const auto rss = ReadKbField(*status, "VmRSS:")) {
                stats.ram_game_mb = static_cast<float>(*rss / 1024.0);
            }
        }
    }

    void SampleGpu(MachineStats& stats) {
        if (gpu_device.isEmpty()) {
            nvml.Sample(stats);
            return;
        }
        if (const auto busy = ReadNumber(gpu_device + QStringLiteral("/gpu_busy_percent"))) {
            stats.gpu_pct = static_cast<float>(*busy);
        }
        constexpr double MiB = 1024.0 * 1024.0;
        if (const auto used = ReadNumber(gpu_device + QStringLiteral("/mem_info_vram_used"))) {
            stats.vram_used_mb = static_cast<float>(*used / MiB);
        }
        if (const auto total = ReadNumber(gpu_device + QStringLiteral("/mem_info_vram_total"))) {
            stats.vram_total_mb = static_cast<float>(*total / MiB);
        }
        if (!gpu_temp.isEmpty()) {
            if (const auto milli = ReadNumber(gpu_temp)) {
                stats.gpu_temp_c = static_cast<float>(*milli / 1000.0);
            }
        }
        if (!gpu_power.isEmpty()) {
            if (const auto micro = ReadNumber(gpu_power)) {
                stats.gpu_power_w = static_cast<float>(*micro / 1e6);
            }
        }
        if (!gpu_clock.isEmpty()) {
            if (const auto hz = ReadNumber(gpu_clock)) {
                stats.gpu_clock_mhz = static_cast<float>(*hz / 1e6);
            }
        }
    }
};

SystemMonitor::SystemMonitor() : impl{std::make_unique<Impl>()} {
    if (const auto info = ReadFile(QStringLiteral("/proc/cpuinfo"))) {
        for (const QByteArray& line : info->split('\n')) {
            if (line.startsWith("model name")) {
                cpu_name = QString::fromUtf8(line.mid(line.indexOf(':') + 1)).simplified();
                break;
            }
        }
    }
}

SystemMonitor::~SystemMonitor() = default;

MachineStats SystemMonitor::Sample() {
    MachineStats stats;
    impl->SampleCpu(stats);
    impl->SampleMemory(stats);
    impl->SampleGpu(stats);
    return stats;
}

#else

struct SystemMonitor::Impl {};

SystemMonitor::SystemMonitor() : impl{std::make_unique<Impl>()} {}

SystemMonitor::~SystemMonitor() = default;

MachineStats SystemMonitor::Sample() {
    return {};
}

#endif
