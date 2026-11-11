// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <regex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/fs/file_lock.h"
#include "common/logging.h"
#include "video_core/shader_cache_v2/cache.h"
#include "video_core/shader_cache_v2/precompiled_shader_stamp.h"
#include "video_core/shader_cache_v2/records.h"
#include "video_core/shader_cache_v2/stale_variants.h"

namespace VideoCommon::ShaderCacheV2 {
namespace {

u64 ParseStamp(const std::string& hex) {
    return std::strtoull(hex.c_str(), nullptr, 16);
}

std::filesystem::path LockPath(std::filesystem::path log) {
    log += ".lock";
    return log;
}

} // Anonymous namespace

void NormalizeShaderFiles(const std::filesystem::path& title_dir) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(title_dir, error)) {
        return;
    }
    static const std::regex lowering_log{R"(lichii-cache\.v2-lowering-0x([0-9a-f]+)\.bin)"};
    static const std::regex infos{R"(precompiled_infos\.fork-v(\d+)-shader-0x([0-9a-f]+)\.bin)"};
    static const std::regex prepared{
        R"(precompiled_prepared\.fork-v(\d+)-shader-0x([0-9a-f]+)\.txt)"};
    static const std::regex stamped_temporary{
        R"(precompiled_infos\.fork-v\d+-shader-0x[0-9a-f]+\.bin\.tmp)"};
    static const std::regex warm_marker{R"(warm-compute-0x([0-9a-f]+)\.done)"};
    const u64 lowering = LoweringStamp();

    std::vector<fs::path> logs_to_remove;
    fs::path current_log;
    // Files of this build to take their plain name, and files of other builds to remove.
    std::vector<std::pair<fs::path, fs::path>> renames;
    std::vector<fs::path> removals;
    for (const auto& entry : fs::directory_iterator(title_dir, error)) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        std::smatch match;
        if (std::regex_match(name, match, lowering_log)) {
            if (ParseStamp(match[1].str()) == lowering) {
                current_log = entry.path();
            } else {
                logs_to_remove.push_back(entry.path());
            }
        } else if (std::regex_match(name, match, infos) ||
                   std::regex_match(name, match, prepared)) {
            if (ParseStamp(match[2].str()) != PrecompiledShaderStamp) {
                removals.push_back(entry.path());
                continue;
            }
            const bool is_infos = name.starts_with("precompiled_infos");
            renames.emplace_back(entry.path(),
                                 title_dir / (std::string{is_infos ? "precompiled_infos"
                                                                   : "precompiled_prepared"} +
                                              ".fork-v" + match[1].str() +
                                              (is_infos ? ".bin" : ".txt")));
        } else if (std::regex_match(name, stamped_temporary)) {
            removals.push_back(entry.path());
        } else if (std::regex_match(name, match, warm_marker)) {
            if (ParseStamp(match[1].str()) == PrecompiledShaderStamp) {
                renames.emplace_back(entry.path(), title_dir / "warm-compute.done");
            } else {
                removals.push_back(entry.path());
            }
        }
    }

    size_t renamed = 0;
    size_t removed = 0;
    u64 bytes = 0;
    const auto remove = [&](const fs::path& path) {
        const auto size = fs::file_size(path, error);
        if (fs::remove(path, error) && !error) {
            bytes += size;
            ++removed;
        }
    };
    // Record logs: only while no other process has them open.
    const auto plain_log = title_dir / FileName;
    if (!current_log.empty()) {
        const Common::FS::FileLock plain_lock{LockPath(plain_log)};
        const Common::FS::FileLock variant_lock{LockPath(current_log)};
        if (plain_lock.Held() && variant_lock.Held()) {
            const auto replaced = fs::file_size(plain_log, error);
            const bool had_plain = !error;
            fs::rename(current_log, plain_log, error);
            if (!error) {
                ++renamed;
                if (had_plain) {
                    bytes += replaced;
                    ++removed;
                }
            }
        }
    }
    if (!current_log.empty() && !fs::exists(current_log, error)) {
        fs::remove(LockPath(current_log), error);
    }
    for (const auto& log : logs_to_remove) {
        {
            const Common::FS::FileLock lock{LockPath(log)};
            if (!lock.Held()) {
                continue;
            }
            remove(log);
        }
        fs::remove(LockPath(log), error);
    }
    for (const auto& path : removals) {
        remove(path);
    }
    for (const auto& [from, to] : renames) {
        const auto replaced = fs::file_size(to, error);
        const bool had_plain = !error;
        fs::rename(from, to, error);
        if (!error) {
            ++renamed;
            if (had_plain) {
                bytes += replaced;
                ++removed;
            }
        }
    }
    if (renamed != 0 || removed != 0) {
        LOG_INFO(Render_Vulkan,
                 "Shader folder: {} files of this build took their plain name, {} files of other "
                 "builds removed ({} MiB)",
                 renamed, removed, bytes / (1024 * 1024));
    }
}

} // namespace VideoCommon::ShaderCacheV2
