// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <new>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/thread.h"
#endif

#include "core/device_memory.h"
#include "common/nx2_hitch.h"
#include "hle/kernel/board/nintendo/nx/k_system_control.h"

namespace Core {

#ifdef HAS_NCE
constexpr size_t VirtualReserveSize = 1ULL << 38;
#else
constexpr size_t VirtualReserveSize = 1ULL << 39;
#endif

DeviceMemory::DeviceMemory()
    : buffer{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
             VirtualReserveSize} {
#ifdef _WIN32
    // Opt out of the Windows zero-page pass for performance comparisons.
    if (const char* value = std::getenv("EDEN_NX2_ZERO_TRIM"); value == nullptr || value[0] != '0') {
        zero_trim_thread = std::jthread{[this](std::stop_token stop) { TrimZeroPages(stop); }};
    }
#endif
}

DeviceMemory::~DeviceMemory() = default;

#ifdef _WIN32
// Windows can't punch holes in the guest memory section, so pages holding only zeros (games clear
// their heaps while booting: Ocarina leaves ~7 GB) stay resident until they leave the working set.
// A pass comes every 5 s while they keep turning up, backing off when little is trimmed.
void DeviceMemory::TrimZeroPages(std::stop_token stop) {
    using namespace std::chrono_literals;
    Common::SetCurrentThreadName("ZeroTrim");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    constexpr size_t Busy = size_t{64} << 20;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::chrono::seconds interval = 5s;
    static const bool idle_timing = [] {
        const char* value = std::getenv("EDEN_NX2_ZERO_TRIM_IDLE");
        return value == nullptr || value[0] != '0';
    }();
    const std::chrono::seconds max_interval = idle_timing ? 300s : 60s;
    while (true) {
        {
            std::unique_lock lock{mutex};
            wake.wait_for(lock, stop, interval, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        // Avoid scanning while the guest is mapping memory in bulk.
        for (int attempt = 0; idle_timing && attempt < 10; ++attempt) {
            const u64 before = Common::Nx2Hitch::g_host_activity_bytes.load(std::memory_order_relaxed);
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, 500ms, [] { return false; });
            }
            if (stop.stop_requested()) {
                return;
            }
            if (Common::Nx2Hitch::g_host_activity_bytes.load(std::memory_order_relaxed) - before <
                (u64{32} << 20)) {
                break;
            }
        }
        const auto start = std::chrono::steady_clock::now();
        size_t resident = 0;
        size_t trimmed = 0;
        Common::Nx2Hitch::ZeroTrimBegin();
        try {
            const auto result = buffer.TrimZeroPages();
            resident = result.first;
            trimmed = result.second;
        } catch (const std::bad_alloc&) {
            Common::Nx2Hitch::ZeroTrimEnd();
            // Optional working-set maintenance must not terminate the process when
            // its temporary page lists cannot be allocated under host RAM pressure.
            // Avoid allocating or formatting a log message on this low-memory path.
            interval = 60s;
            continue;
        }
        Common::Nx2Hitch::ZeroTrimEnd();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        if (trimmed < Busy) {
            // A slow pass is logged too: its working set query and page reads take ~1 s at
            // ~7 GB resident, and freezes need lining up against it (fable52_answer_1 M8c).
            if (ms >= 100 && Common::Nx2Hitch::Enabled()) {
                const auto phases = Common::HostMemory::LastZeroTrimPhases();
                LOG_INFO(HW_Memory,
                         "Zero-page pass: {} MiB trimmed of {} MiB resident in {} ms (next in {} s); "
                         "working set query {} ms, page reads {} ms, {} unlock runs {} ms "
                         "(longest {:.1f})",
                         trimmed >> 20, resident >> 20, ms,
                         (std::min)(interval * 2, max_interval).count(),
                         phases.query_us / 1000, phases.scan_us / 1000, phases.unlock_runs,
                         phases.unlock_us / 1000, static_cast<double>(phases.longest_unlock_us) / 1e3);
            }
            interval = (std::min)(interval * 2, max_interval);
            continue;
        }
        interval = 5s;
        const auto phases = Common::HostMemory::LastZeroTrimPhases();
        LOG_INFO(HW_Memory,
                 "Trimmed {} MiB of all-zero guest pages out of {} MiB resident in {} ms; the kernel "
                 "zero-filled {} MiB so far; working set query {} ms, page reads {} ms, {} unlock "
                 "runs {} ms (longest {:.1f})",
                 trimmed >> 20, resident >> 20, ms, Common::HostMemory::ZeroFilledBytes() >> 20,
                 phases.query_us / 1000, phases.scan_us / 1000, phases.unlock_runs,
                 phases.unlock_us / 1000, static_cast<double>(phases.longest_unlock_us) / 1e3);
    }
}
#endif

} // namespace Core
