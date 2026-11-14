// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

namespace Kernel::Svc {

/// Used to output a message on a debug hardware unit - does nothing on a retail unit
Result OutputDebugString(Core::System& system, u64 address, u64 len) {
    R_SUCCEED_IF(len == 0);
    // The first lines are logged in full, then only a summary every 1000 calls, so a game that
    // prints every frame cannot flood the log. EDEN_NX2_DEBUG_STRING_LIMIT=0 logs everything.
    static const bool limited = [] {
        const char* env = std::getenv("EDEN_NX2_DEBUG_STRING_LIMIT");
        return env == nullptr || std::strtol(env, nullptr, 10) != 0;
    }();
    constexpr u64 FULL_LINES = 200;
    constexpr u64 SUMMARY_EVERY = 1000;
    if (limited) {
        static std::atomic<u64> calls{0};
        const u64 count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count > FULL_LINES) {
            if ((count - FULL_LINES) % SUMMARY_EVERY == 0) {
                LOG_INFO(Debug_Emulated, "OutputDebugString: {} calls so far, only the first {} logged",
                         count, FULL_LINES);
            }
            R_SUCCEED();
        }
        len = (std::min)(len, u64{4096});
    }
    std::string msg_buffer(len, 0);
    GetCurrentMemory(system.Kernel()).ReadBlock(address, msg_buffer.data(), len);
    LOG_INFO(Debug_Emulated, "{}", msg_buffer);
    R_SUCCEED();
}

Result OutputDebugString64(Core::System& system, uint64_t debug_str, uint64_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

Result OutputDebugString64From32(Core::System& system, uint32_t debug_str, uint32_t len) {
    R_RETURN(OutputDebugString(system, debug_str, len));
}

} // namespace Kernel::Svc
