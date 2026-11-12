// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <QString>

/// The machine's load for the detailed performance overlay. A value the machine does not give is
/// negative.
struct MachineStats {
    /// CPU: the whole machine and this process, percent of all logical processors
    float cpu_total_pct = -1;
    float cpu_game_pct = -1;
    float cpu_temp_c = -1;
    /// Memory, MB: in use on the machine, installed, and held by this process
    float ram_used_mb = -1;
    float ram_total_mb = -1;
    float ram_game_mb = -1;
    /// GPU: load, edge temperature, board power, shader clock and dedicated memory (every
    /// program on the card) from the amdgpu sysfs files or NVIDIA's NVML
    float gpu_pct = -1;
    float gpu_temp_c = -1;
    float gpu_power_w = -1;
    float gpu_clock_mhz = -1;
    float vram_used_mb = -1;
    float vram_total_mb = -1;
};

/// Reads MachineStats from the OS. The CPU percentages are over the time between two Sample
/// calls, so the first one reports none.
class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    [[nodiscard]] MachineStats Sample();

    /// The CPU's model name, empty when unknown
    [[nodiscard]] const QString& CpuName() const {
        return cpu_name;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    QString cpu_name;
};
