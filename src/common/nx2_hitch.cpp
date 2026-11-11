// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>

#include "common/logging.h"
#include "common/host_memory.h"
#include "common/nx2_hitch.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#endif

namespace Common::Nx2Hitch {

namespace {

constexpr long long ThresholdUs = 50'000;
constexpr size_t Causes = static_cast<size_t>(Cause::Count);
constexpr size_t MaxThreads = 48;

struct ThreadEntry {
    std::string name;
#ifdef _WIN32
    HANDLE handle{};
#elif defined(__linux__)
    clockid_t clock{};
#endif
};
std::mutex thread_mutex;
std::vector<ThreadEntry> threads;

struct Snapshot {
    std::array<u64, Causes> ns{};
    std::array<u64, Causes> count{};
    u64 page_faults{};
    u64 user_us{};
    u64 kernel_us{};
    u64 zero_filled{};
    size_t thread_count{};
    std::array<u64, MaxThreads> thread_us{};
};

Snapshot Take() {
    Snapshot snapshot;
    snapshot.zero_filled = HostMemory::ZeroFilledBytes();
    for (size_t i = 0; i < Causes; ++i) {
        snapshot.ns[i] = g_counters.ns[i].load(std::memory_order_relaxed);
        snapshot.count[i] = g_counters.count[i].load(std::memory_order_relaxed);
    }
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        snapshot.page_faults = counters.PageFaultCount;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto microseconds = [](const FILETIME& t) {
            return ((u64{t.dwHighDateTime} << 32) | t.dwLowDateTime) / 10;
        };
        snapshot.user_us = microseconds(user);
        snapshot.kernel_us = microseconds(kernel);
    }
#elif defined(__linux__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        snapshot.page_faults = static_cast<u64>(usage.ru_minflt) + static_cast<u64>(usage.ru_majflt);
        snapshot.user_us = static_cast<u64>(usage.ru_utime.tv_sec) * 1'000'000 +
                           static_cast<u64>(usage.ru_utime.tv_usec);
        snapshot.kernel_us = static_cast<u64>(usage.ru_stime.tv_sec) * 1'000'000 +
                             static_cast<u64>(usage.ru_stime.tv_usec);
    }
#endif
    {
        std::scoped_lock lock{thread_mutex};
        snapshot.thread_count = threads.size();
        for (size_t i = 0; i < snapshot.thread_count; ++i) {
#ifdef _WIN32
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(threads[i].handle, &created, &exited, &kernel, &user)) {
                snapshot.thread_us[i] = (((u64{kernel.dwHighDateTime} << 32) | kernel.dwLowDateTime) +
                                         ((u64{user.dwHighDateTime} << 32) | user.dwLowDateTime)) / 10;
            }
#elif defined(__linux__)
            timespec value{};
            if (clock_gettime(threads[i].clock, &value) == 0) {
                snapshot.thread_us[i] = static_cast<u64>(value.tv_sec) * 1'000'000 +
                                        static_cast<u64>(value.tv_nsec) / 1'000;
            }
#endif
        }
    }
    return snapshot;
}

std::string BusyThreads(const Snapshot& last, const Snapshot& now) {
    std::vector<std::pair<u64, std::string>> busy;
    std::scoped_lock lock{thread_mutex};
    for (size_t i = 0; i < now.thread_count; ++i) {
        const u64 before = i < last.thread_count ? last.thread_us[i] : now.thread_us[i];
        if (now.thread_us[i] >= before && now.thread_us[i] != before) {
            busy.emplace_back(now.thread_us[i] - before, threads[i].name);
        }
    }
    std::ranges::sort(busy, std::greater{}, &std::pair<u64, std::string>::first);
    std::string result;
    for (size_t i = 0; i < std::min<size_t>(3, busy.size()); ++i) {
        result += fmt::format("{}{} {:.1f} ms", result.empty() ? "" : ", ", busy[i].second,
                              static_cast<double>(busy[i].first) / 1e3);
    }
    return result.empty() ? "none measured" : result;
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_HITCH_LOG");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

Cause CompileCause(u64 key, u32 core) {
    if (!Enabled()) {
        return Cause::Recompile;
    }
    static std::mutex mutex;
    static std::unordered_map<u64, u8> seen;
    const u8 bit = static_cast<u8>(1U << (core & 7));
    std::scoped_lock lock{mutex};
    u8& mask = seen[key];
    const bool repeated = (mask & ~bit) != 0;
    mask |= bit;
    return repeated ? Cause::RecompileRepeat : Cause::Recompile;
}

void RegisterThread(const char* name) {
    // Do not cache the environment before the frontend has loaded nx2_switches.txt.
    const char* value = std::getenv("EDEN_NX2_HITCH_LOG");
    if (value == nullptr || value[0] != '1') {
        return;
    }
    static thread_local bool registered = false;
    if (registered) {
        return;
    }
    ThreadEntry entry{.name = name};
#ifdef _WIN32
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &entry.handle,
                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return;
    }
#elif defined(__linux__)
    if (pthread_getcpuclockid(pthread_self(), &entry.clock) != 0) {
        return;
    }
#else
    return;
#endif
    std::scoped_lock lock{thread_mutex};
    if (threads.size() < MaxThreads) {
        threads.push_back(std::move(entry));
        registered = true;
    }
#ifdef _WIN32
    else {
        CloseHandle(entry.handle);
    }
#endif
}

void OnPresent(long long gap_us) {
    if (!Enabled()) {
        return;
    }
    static Snapshot last = Take();
    static u64 window_start_ns = NowNs();
    static int lines_in_window = 0;
    const Snapshot now = Take();
    if (gap_us >= ThresholdUs) {
        // At most 60 lines a minute (a loading screen can stall many presents in a row).
        const u64 now_ns = NowNs();
        if (now_ns - window_start_ns >= 60'000'000'000ULL) {
            window_start_ns = now_ns;
            lines_in_window = 0;
        }
        if (lines_in_window++ < 60) {
            const auto ms = [&](Cause cause) {
                const auto i = static_cast<size_t>(cause);
                return static_cast<double>(now.ns[i] - last.ns[i]) / 1e6;
            };
            const auto n = [&](Cause cause) {
                const auto i = static_cast<size_t>(cause);
                return now.count[i] - last.count[i];
            };
            const u64 trim_since = g_counters.zero_trim_since.load(std::memory_order_relaxed);
            const double trim_running_ms =
                trim_since != 0 ? static_cast<double>(now_ns - trim_since) / 1e6 : 0.0;
            LOG_INFO(Render, // with the frame lines it lines up against
                     "Hitch: {:.1f} ms between presents; in it: fence releases {:.1f} ms ({}), "
                     "read-backs {:.1f} ms ({}), save data {:.1f} ms ({}), "
                     "zero-page passes ended {:.1f} ms ({}), one running for {:.0f} ms, "
                     "file throttle {:.1f} ms ({}), file reads {:.1f} ms ({}), "
                     "host map/unmap/protect {:.1f}/{:.1f}/{:.1f} ms, "
                     "new/repeated JIT {:.1f}/{:.1f} ms, page faults {}, zero-filled {} MiB, "
                     "CPU user/kernel {:.1f}/{:.1f} ms, "
                     "busiest threads {}",
                     static_cast<double>(gap_us) / 1e3, ms(Cause::FullDrain),
                     n(Cause::FullDrain), ms(Cause::ReadBack), n(Cause::ReadBack),
                     ms(Cause::SaveData), n(Cause::SaveData), ms(Cause::ZeroTrim), n(Cause::ZeroTrim),
                     trim_running_ms, ms(Cause::FileThrottle), n(Cause::FileThrottle),
                     ms(Cause::FileRead), n(Cause::FileRead), ms(Cause::HostMap),
                     ms(Cause::HostUnmap), ms(Cause::HostProtect), ms(Cause::Recompile),
                     ms(Cause::RecompileRepeat), now.page_faults - last.page_faults,
                     (now.zero_filled - last.zero_filled) >> 20,
                     static_cast<double>(now.user_us - last.user_us) / 1e3,
                     static_cast<double>(now.kernel_us - last.kernel_us) / 1e3,
                     BusyThreads(last, now));
        }
    }
    last = now;
}

} // namespace Common::Nx2Hitch
