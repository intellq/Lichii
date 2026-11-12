// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "common/settings.h"
#include "common/nx2_hitch.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"
#include "core/hle/service/filesystem/fsp/fs_read_trace.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    // The 40 MB/s throttle emulates a Switch 1 card (Eden cbd4cf2d8c, Mario Strikers' intro).
    // Switch 2 storage is many times faster, and a Switch 2 title that streams a movie with
    // hundreds of reads a second (Ocarina of Time's opening: 300-460) sleeps most of each second
    // on the thread that also hands decoder images back, so its video falls seconds behind.
    // Hyrule Warriors: Age of Imprisonment does not survive reads that complete with no delay at
    // all: from ~8 s into boot it follows null and wild pointers and never leaves its first
    // loading screen. It boots at 100 and 200 MB/s, not at 300 or 400 (upstream 45d1fdbdf4).
    if (!Settings::CurrentProgramIsSwitch2()) {
        bytes_per_second = ReadBytesPerSecond;
    } else if (FileSys::GetBaseTitleID(system.GetApplicationProcessProgramID()) ==
               AgeOfImprisonmentProgramId) {
        bytes_per_second = AgeOfImprisonmentReadBytesPerSecond;
    }
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    // The pace the constructor chose (none for most Switch 2 titles). EDEN_NX2_READ_THROTTLE=0
    // turns it off.
    static const bool throttle_enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_READ_THROTTLE");
        return value == nullptr || value[0] != '0';
    }();
    if (throttle_enabled && bytes_per_second > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_read_tick) {
            const Common::Nx2Hitch::Scope timed{Common::Nx2Hitch::Cause::FileThrottle};
            std::this_thread::sleep_for(next_read_tick - now);
        }
        next_read_tick = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                                  1'000'000'000 / bytes_per_second};
    }

    // Read the data from the Storage backend
    const ReadTraceScope trace{ReadTraceSource::Storage, length};
    const Common::Nx2Hitch::Scope timed{Common::Nx2Hitch::Cause::FileRead,
                                       static_cast<u64>(length)};
    backend->Read(out_bytes.data(), length, offset);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
