// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <catch2/catch_test_macros.hpp>

#include "common/nx2_hitch.h"

TEST_CASE("Hitch attribution only collects counters when requested", "[nx2_hitch]") {
    using namespace Common::Nx2Hitch;
    const char* const option = std::getenv("EDEN_NX2_HITCH_LOG");
    const bool expected = option != nullptr && option[0] == '1';
    REQUIRE(Enabled() == expected);
    const auto index = static_cast<size_t>(Cause::ReadBack);
    const auto count_before = g_counters.count[index].load(std::memory_order_relaxed);
    const auto ns_before = g_counters.ns[index].load(std::memory_order_relaxed);
    Add(Cause::ReadBack, 123);
    CHECK(g_counters.count[index].load(std::memory_order_relaxed) == count_before + expected);
    CHECK(g_counters.ns[index].load(std::memory_order_relaxed) == ns_before + (expected ? 123 : 0));
}

TEST_CASE("Hitch attribution identifies repeated JIT compilation only when enabled", "[nx2_hitch]") {
    using namespace Common::Nx2Hitch;
    constexpr u64 block = 0x1234ABCDEF;
    CHECK(CompileCause(block, 0) == Cause::Recompile);
    CHECK(CompileCause(block, 1) == (Enabled() ? Cause::RecompileRepeat : Cause::Recompile));
}
