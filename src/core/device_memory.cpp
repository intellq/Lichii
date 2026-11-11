// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>

#include "common/logging.h"
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
        Common::Nx2Hitch::ZeroTrimBegin();
        const auto [resident, trimmed] = buffer.TrimZeroPages();
        Common::Nx2Hitch::ZeroTrimEnd();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        if (trimmed < Busy) {
            interval = (std::min)(interval * 2, max_interval);
            continue;
        }
        interval = 5s;
        LOG_INFO(HW_Memory,
                 "Trimmed {} MiB of all-zero guest pages out of {} MiB resident in {} ms; the kernel "
                 "zero-filled {} MiB so far",
                 trimmed >> 20, resident >> 20, ms, Common::HostMemory::ZeroFilledBytes() >> 20);
    }
}
#endif

} // namespace Core
