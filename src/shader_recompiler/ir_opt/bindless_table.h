// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>

#include "common/common_types.h"

namespace Shader::Optimization {

// Shared by shader translation and the stamp of precompiled SPIR-V.
[[nodiscard]] inline u32 ParseHandleTableSize(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return 64U;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    return static_cast<u32>(std::clamp(parsed, 0L, 1024L));
}

[[nodiscard]] inline u32 HandleTableSize() {
    static const u32 size = ParseHandleTableSize(std::getenv("LICHII_BINDLESS_TABLE"));
    return size;
}

} // namespace Shader::Optimization
