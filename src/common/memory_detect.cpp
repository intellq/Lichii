// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#include <charconv>
#include <string_view>

#ifdef __linux__
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#include <unistd.h>
#endif

#include "common/logging.h"
#include "common/literals.h"
#include "common/memory_detect.h"

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    GlobalMemoryStatusEx(&memorystatus);
    mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
    mem_info.TotalSwapMemory = memorystatus.ullTotalPageFile - mem_info.TotalPhysicalMemory;
#elif defined(__APPLE__)
    u64 ramsize;
    struct xsw_usage vmusage;
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0);
    sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0);
    mem_info.TotalPhysicalMemory = ramsize;
    mem_info.TotalSwapMemory = vmusage.xsu_total;
#elif defined(__FreeBSD__)
    u_long physmem, swap_total;
    std::size_t sizeof_u_long = sizeof(u_long);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    sysctlbyname("hw.physmem", &physmem, &sizeof_u_long, nullptr, 0);
    sysctlbyname("vm.swap_total", &swap_total, &sizeof_u_long, nullptr, 0);
    mem_info.TotalPhysicalMemory = physmem;
    mem_info.TotalSwapMemory = swap_total;
#elif defined(__linux__)
    struct sysinfo meminfo;
    sysinfo(&meminfo);
    mem_info.TotalPhysicalMemory = meminfo.totalram;
    mem_info.TotalSwapMemory = meminfo.totalswap;
#else
    mem_info.TotalPhysicalMemory = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

HostRam GetHostRam() {
#ifdef _WIN32
    ULONGLONG kilobytes{};
    if (GetPhysicallyInstalledSystemMemory(&kilobytes) && kilobytes != 0) {
        return {kilobytes * 1024, true};
    }
#endif
#ifdef __APPLE__
    return {GetMemInfo().TotalPhysicalMemory, true};
#else
#ifdef __linux__
    // udev keeps the firmware's memory devices (SMBIOS type 17) readable without root. This
    // counts memory the GPU reserves, e.g. the Steam Deck's 16 GiB, of which Linux manages 11.5.
    if (std::ifstream file{"/run/udev/data/+dmi:id"}) {
        const std::string data{std::istreambuf_iterator<char>{file}, {}};
        if (const u64 installed = InstalledRamFromUdev(data); installed != 0) {
            return {installed, true};
        }
    }
#endif
    return {GetMemInfo().TotalPhysicalMemory, false};
#endif
}

u64 InstalledRamFromUdev(std::string_view data) {
    constexpr std::string_view prefix = "E:MEMORY_DEVICE_";
    constexpr std::string_view suffix = "_SIZE=";
    u64 total = 0;
    while (!data.empty()) {
        const size_t end = data.find('\n');
        std::string_view line = data.substr(0, end);
        data = end == std::string_view::npos ? std::string_view{} : data.substr(end + 1);
        if (!line.starts_with(prefix)) {
            continue;
        }
        line.remove_prefix(prefix.size());
        const size_t digits = line.find_first_not_of("0123456789");
        if (digits == 0 || digits == std::string_view::npos ||
            !line.substr(digits).starts_with(suffix)) {
            continue;
        }
        line.remove_prefix(digits + suffix.size());
        u64 size = 0;
        bool valid = !line.empty();
        for (const char c : line) {
            if (c < '0' || c > '9') {
                valid = false;
                break;
            }
            size = size * 10 + static_cast<u64>(c - '0');
        }
        if (valid) {
            total += size;
        }
    }
    return total;
}

bool IsBelowSwitch2RamMinimum(HostRam ram) {
    using namespace Common::Literals;
    return ram.bytes != 0 && ram.bytes < (ram.installed ? 16_GiB : 12_GiB);
}

ProcessMemoryUsage GetProcessMemoryUsage() {
    ProcessMemoryUsage usage{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX2 counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        usage.commit = counters.PrivateUsage;
        usage.working_set = counters.WorkingSetSize;
        usage.private_working_set = counters.PrivateWorkingSetSize;
    }
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        usage.system_commit = u64{performance.CommitTotal} * performance.PageSize;
        usage.system_commit_limit = u64{performance.CommitLimit} * performance.PageSize;
    }
#elif defined(__linux__)
    const auto read = [](const char* path) {
        std::ifstream file{path};
        return std::string{std::istreambuf_iterator<char>{file}, {}};
    };
    const std::string status = read("/proc/self/status");
    usage.commit = ProcKilobytesField(status, "VmData:");
    usage.working_set = ProcKilobytesField(status, "VmRSS:");
    usage.private_working_set = ProcKilobytesField(status, "RssAnon:");
    // The memory trace printed "system commit 0 of 0 MiB" on Linux.
    const std::string meminfo = read("/proc/meminfo");
    usage.system_commit = ProcKilobytesField(meminfo, "Committed_AS:");
    usage.system_commit_limit = ProcKilobytesField(meminfo, "CommitLimit:");
#endif
    return usage;
}

std::optional<u64> GetSystemCommitRoom() {
#ifdef _WIN32
    PERFORMANCE_INFORMATION performance{};
    performance.cb = sizeof(performance);
    if (GetPerformanceInfo(&performance, sizeof(performance))) {
        const u64 limit = performance.CommitLimit;
        const u64 committed = performance.CommitTotal;
        return (committed < limit ? limit - committed : 0) * performance.PageSize;
    }
#endif
    return std::nullopt;
}

u64 ProcKilobytesField(std::string_view text, std::string_view key) {
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.starts_with(key)) {
            continue;
        }
        line.remove_prefix(key.size());
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        u64 kilobytes = 0;
        const auto [ptr, error] =
            std::from_chars(line.data(), line.data() + line.size(), kilobytes);
        if (error != std::errc{} || ptr == line.data()) {
            return 0;
        }
        return kilobytes << 10;
    }
    return 0;
}

#if defined(__linux__) && defined(__GLIBC__)
namespace {
/// Resident set in bytes from /proc/self/statm (its second field, in pages), 0 when unreadable.
u64 ResidentBytes() {
    std::ifstream file{"/proc/self/statm"};
    u64 size = 0;
    u64 resident = 0;
    if (!(file >> size >> resident)) {
        return 0;
    }
    return resident * static_cast<u64>(sysconf(_SC_PAGESIZE));
}
} // Anonymous namespace
#endif

HeapTrimResult TrimHeap([[maybe_unused]] std::string_view reason) {
    HeapTrimResult result{};
#if defined(__linux__) && defined(__GLIBC__)
    result.resident_before = ResidentBytes();
    const auto start = std::chrono::steady_clock::now();
    const int released = malloc_trim(0);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.resident_after = ResidentBytes();
    const s64 handed_back =
        (static_cast<s64>(result.resident_before) - static_cast<s64>(result.resident_after)) >> 20;
    LOG_INFO(Common_Memory,
             "malloc_trim after {}: resident {} -> {} MiB ({} MiB handed back) in {:.1f} ms{}",
             reason, result.resident_before >> 20, result.resident_after >> 20, handed_back, ms,
             released != 0 ? "" : " (nothing to release)");
#endif
    return result;
}

u64 GetAvailablePhysicalMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    if (!GlobalMemoryStatusEx(&memorystatus)) {
        return 0;
    }
    return memorystatus.ullAvailPhys;
#elif defined(__linux__)
    // MemAvailable counts the page cache the kernel can drop, which sysinfo's freeram doesn't.
    std::FILE* const file = std::fopen("/proc/meminfo", "r");
    if (file == nullptr) {
        return 0;
    }
    u64 available_kib = 0;
    char line[128];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        unsigned long long value = 0;
        if (std::sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
            available_kib = value;
            break;
        }
    }
    std::fclose(file);
    return available_kib * 1024;
#else
    return 0;
#endif
}

bool IsHostLowOnMemory() {
    using Clock = std::chrono::steady_clock;
    static std::atomic<Clock::rep> checked_at{};
    static std::atomic<bool> low{};
    const Clock::rep now = Clock::now().time_since_epoch().count();
    const Clock::rep last = checked_at.load(std::memory_order_relaxed);
    if (last == 0 || now - last >= Clock::duration{std::chrono::seconds{1}}.count()) {
        checked_at.store(now, std::memory_order_relaxed);
        low.store(IsLowOnMemory(GetAvailablePhysicalMemory(), GetMemInfo().TotalPhysicalMemory),
                  std::memory_order_relaxed);
    }
    return low.load(std::memory_order_relaxed);
}

} // namespace Common
