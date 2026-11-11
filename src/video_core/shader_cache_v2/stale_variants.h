// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace VideoCommon::ShaderCacheV2 {

/// Leaves one file of each kind in a title's shader folder, under its plain name. Files named
/// after a translator or shader stamp (lichii-cache.v2-lowering-<stamp>.bin,
/// precompiled_infos/prepared ...-shader-<stamp>, warm-compute-<stamp>.done) take the plain name
/// when they are this build's, replacing the older file, and are removed otherwise. Each file
/// keeps the stamp it was written for inside, so a build with another translator replaces it.
/// A record log another process holds open is left alone.
void NormalizeShaderFiles(const std::filesystem::path& title_dir);

} // namespace VideoCommon::ShaderCacheV2
