// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nso.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
constexpr std::string_view FpsUnlock = "FPS Unlock (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;
constexpr size_t DynamicCallOffset = sizeof(Loader::NSOHeader) + 0x6E6CC4;
constexpr size_t DynamicRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46200;
constexpr size_t DynamicStateOffset = sizeof(Loader::NSOHeader) + 0x4E9FE00;
constexpr size_t DynamicRoutineSize = 120;
constexpr size_t UnlockRoutineSize = 128;
constexpr size_t DynamicStateSize = 24;
constexpr size_t WaterHookOffset = sizeof(Loader::NSOHeader) + 0x475F8;
constexpr size_t WaterMulOffset = sizeof(Loader::NSOHeader) + 0x4760C;
constexpr size_t WaterRoutineOffset = sizeof(Loader::NSOHeader) + 0x3C46400;
constexpr size_t WaterRoutineSize = 84;
constexpr u32 WaterOriginalInstruction = 0x1E380009;
constexpr u32 WaterMulOriginalInstruction = 0x1B097D08;
constexpr u32 WaterCallInstruction = 0x94EFFB82;
constexpr std::string_view WaterRoutine =
    "0101221E0008211E0820201E0D020054C99200B021114EBD0028211E01102C1E0228211E"
    "4800381E1F0500714A000054280080520101221E0038211E01103C1E0048211E20110EBD"
    "C0035FD6E8031F2AC0035FD6";
constexpr size_t UnlockStateSize = 0xA8;
constexpr size_t SoundEntryOffset = sizeof(Loader::NSOHeader) + 0x415300;
constexpr size_t SoundStepOffset = sizeof(Loader::NSOHeader) + 0x415334;
constexpr size_t SoundGateOffset = sizeof(Loader::NSOHeader) + 0x3C46800;
constexpr size_t SoundFeedOffset = sizeof(Loader::NSOHeader) + 0x3C46A00;
constexpr u32 SoundEntryOriginalInstruction = 0xD503237F; // pacibsp
constexpr u32 SoundEntryBranchInstruction = 0x14E0C540;   // b main+0x3C46800
constexpr u32 SoundStepOriginalInstruction = 0xBD401100;  // ldr s0, [x8, #0x10]
constexpr u32 SoundStepCallInstruction = 0x94E0C5B3;      // bl main+0x3C46A00
constexpr std::string_view SoundGate =
    "D09200B010823A9102030035C9810090294944F9290140F9301140BD11102E1E0022311EAA010054"
    "120240BD522A301E4022311ECB000054523A311E120200BD29008052090600B90A000014120200BD"
    "1F0600B9C0035FD61F0200B929008052090600B903000014090640B9690000347F2303D5A43A1F17"
    "C0035FD6";
constexpr std::string_view SoundFeed = "001140BD10102E1E0068301EC0035FD6";
constexpr u32 DynamicCallInstruction = 0x94D57D4F;
constexpr u32 DynamicOriginalInstruction = 0x1E201828;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 4> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
}};

void WriteIps(const std::filesystem::path& path, u32 word, size_t offset = PatchOffset) {
    const bool ips32 = offset > 0xFFFFFF;
    std::vector<u8> ips =
        ips32 ? std::vector<u8>{'I', 'P', 'S', '3', '2'} : std::vector<u8>{'P', 'A', 'T', 'C', 'H'};
    if (ips32) {
        ips.push_back(static_cast<u8>(offset >> 24));
    }
    ips.push_back(static_cast<u8>(offset >> 16));
    ips.push_back(static_cast<u8>(offset >> 8));
    ips.push_back(static_cast<u8>(offset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    if (ips32) {
        ips.push_back('E');
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

// An IPS (24-bit offsets) or IPS32 file with one record: `bytes` at `offset` of the NSO file.
std::vector<u8> MakeIps(bool ips32, u32 offset, std::span<const u8> bytes) {
    std::vector<u8> ips;
    if (ips32) {
        ips = {'I', 'P', 'S', '3', '2', static_cast<u8>(offset >> 24)};
    } else {
        ips = {'P', 'A', 'T', 'C', 'H'};
    }
    ips.insert(ips.end(), {static_cast<u8>(offset >> 16), static_cast<u8>(offset >> 8),
                           static_cast<u8>(offset), static_cast<u8>(bytes.size() >> 8),
                           static_cast<u8>(bytes.size())});
    ips.insert(ips.end(), bytes.begin(), bytes.end());
    if (ips32) {
        ips.push_back('E');
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    return ips;
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt,
                                     size_t user_mod_offset = PatchOffset) {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word,
                     user_mod_offset);
        }
        system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    }

    ~PatchManagerTestContext() {
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Common::FS::SetEdenPath(TestDirectories[i].first, previous[i]);
        }
        Settings::values.disabled_addons.erase(OcarinaTitleId);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system.GetFileSystemController(),
                                     system.GetContentProvider()};
    }

    // Writes `contents` as <load>/<Ocarina>/<mod>/exefs/<file_name>; returns its path.
    std::filesystem::path AddModFile(std::string_view mod, std::string_view file_name,
                                     std::span<const u8> contents) {
        const auto exefs =
            root / "load" / fmt::format("{:016X}", OcarinaTitleId) / std::string{mod} / "exefs";
        std::filesystem::create_directories(exefs);
        const auto path = exefs / std::string{file_name};
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(contents.data()),
                   static_cast<std::streamsize>(contents.size()));
        file.flush();
        REQUIRE(file.good());
        return path;
    }

private:
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Core::System system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso, size_t offset = PatchOffset) {
    REQUIRE(nso.size() >= offset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + offset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

namespace {

std::vector<u8> MakeDynamicMain() {
    auto nso = MakeMain(OcarinaBuildId, TbzInstruction);
    nso.resize(DynamicStateOffset + UnlockStateSize);
    Loader::NSOHeader header;
    std::memcpy(&header, nso.data(), sizeof(header));
    header.segments[0].size = 63201744;
    header.segments[1].location = 63205376;
    header.segments[1].size = 12603520;
    header.segments[2].location = 75812864;
    header.segments[2].size = 5217152;
    header.segments[2].bss_size = 1414272;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + DynamicCallOffset, &DynamicOriginalInstruction,
                sizeof(DynamicOriginalInstruction));
    std::memcpy(nso.data() + WaterHookOffset, &WaterOriginalInstruction,
                sizeof(WaterOriginalInstruction));
    std::memcpy(nso.data() + WaterMulOffset, &WaterMulOriginalInstruction,
                sizeof(WaterMulOriginalInstruction));
    std::memcpy(nso.data() + SoundEntryOffset, &SoundEntryOriginalInstruction,
                sizeof(SoundEntryOriginalInstruction));
    std::memcpy(nso.data() + SoundStepOffset, &SoundStepOriginalInstruction,
                sizeof(SoundStepOriginalInstruction));
    return nso;
}

std::vector<u8> MakeOcarinaMain() {
    return MakeDynamicMain();
}

constexpr size_t OcarinaCave = FileSys::OotHighRefreshCode::CaveOffset;

void WriteOotHighRefreshCode(std::span<u8> code, const FileSys::OotHighRefreshCode& parts) {
    std::ranges::copy(parts.call, code.begin() + FileSys::OotHighRefreshCode::CallOffset);
    std::ranges::copy(parts.water, code.begin() + FileSys::OotHighRefreshCode::WaterOffset);
    std::ranges::copy(parts.unlock, code.begin() + FileSys::OotHighRefreshCode::UnlockOffset);
    std::ranges::copy(parts.cave, code.begin() + FileSys::OotHighRefreshCode::CaveOffset);
}

} // Anonymous namespace

TEST_CASE("Built-in FPS Unlock defaults on and replaces Dynamic FPS", "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    const auto listed = [&](std::string_view name) {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, name, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return *it;
    };
    const auto routine = [](const std::vector<u8>& nso, size_t size) {
        return Common::HexToString(std::span{nso}.subspan(DynamicRoutineOffset, size));
    };
    constexpr std::string_view RoutineHead =
        "2818201ECA9200B04A0138912BE03BD54C0140F94B0100F9CC0200B46B010CCB0DE03BD5"
        "4E0180D2AE09CE9A7F010EEBCB818B9A4C0540F94C0000B5EC030BAA6E010CCB8C0D8E8B"
        "4C0500F98201239EA301239E4218231E4208211E";
    const auto water_patched = [](const std::vector<u8>& nso) {
        return WordAt(nso, WaterHookOffset) == WaterCallInstruction &&
               WordAt(nso, WaterMulOffset) == NopInstruction &&
               Common::HexToString(std::span{nso}.subspan(WaterRoutineOffset, WaterRoutineSize)) ==
                   WaterRoutine;
    };
    const auto water_untouched = [](const std::vector<u8>& nso) {
        return WordAt(nso, WaterHookOffset) == WaterOriginalInstruction &&
               WordAt(nso, WaterMulOffset) == WaterMulOriginalInstruction &&
               WordAt(nso, WaterRoutineOffset) == 0;
    };
    const auto hex_at = [](const std::vector<u8>& nso, size_t offset, size_t size) {
        return Common::HexToString(std::span{nso}.subspan(offset, size));
    };
    const auto sound_gate_patched = [&](const std::vector<u8>& nso) {
        return WordAt(nso, SoundEntryOffset) == SoundEntryBranchInstruction &&
               WordAt(nso, SoundStepOffset) == SoundStepCallInstruction &&
               hex_at(nso, SoundGateOffset, SoundGate.size() / 2) == SoundGate &&
               hex_at(nso, SoundFeedOffset, SoundFeed.size() / 2) == SoundFeed;
    };
    const auto sound_gate_untouched = [](const std::vector<u8>& nso) {
        return WordAt(nso, SoundEntryOffset) == SoundEntryOriginalInstruction &&
               WordAt(nso, SoundStepOffset) == SoundStepOriginalInstruction &&
               WordAt(nso, SoundGateOffset) == 0 && WordAt(nso, SoundFeedOffset) == 0;
    };
    const auto nso = MakeDynamicMain();

    // Default: FPS Unlock applies, Dynamic FPS is listed as replaced by it when it applies.
    REQUIRE(listed(FpsUnlock).enabled);
    REQUIRE(listed(DynamicFps).enabled);
    REQUIRE(listed(DynamicFps).version ==
            fmt::format("IPS, replaced by {} when it applies", FpsUnlock));
    auto patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(routine(patched, UnlockRoutineSize) ==
            std::string{RoutineHead} + "E300001C4268231EC300001C4278231E4840201EC0035FD6"
                                       "1F2003D50AD7A33C0000C040");
    REQUIRE(WordAt(patched) == BranchInstruction);
    REQUIRE(water_patched(patched));
    REQUIRE(sound_gate_patched(patched));

    // Unchecking only Dynamic FPS keeps FPS Unlock.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{DynamicFps}};
    REQUIRE_FALSE(listed(DynamicFps).enabled);
    REQUIRE(routine(manager.PatchNSO(nso, "main"), UnlockRoutineSize) ==
            routine(patched, UnlockRoutineSize));

    // Without FPS Unlock, Dark's routine applies unchanged.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock}};
    REQUIRE_FALSE(listed(FpsUnlock).enabled);
    REQUIRE(listed(DynamicFps).version == "IPS");
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(routine(patched, DynamicRoutineSize) ==
            std::string{RoutineHead} + "C300001C0309231E4020231E4D0000544840201EC0035FD6"
                                       "0AD7833F");
    REQUIRE(WordAt(patched, DynamicRoutineOffset + DynamicRoutineSize) == 0);
    REQUIRE(water_patched(patched));
    // Dark's step stays at or above 1.0, so the sound manager keeps running every frame.
    REQUIRE(sound_gate_untouched(patched));

    // Both unchecked: hook and code cave stay untouched.
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{FpsUnlock},
                                                        std::string{DynamicFps}};
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicOriginalInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset) == 0);
    REQUIRE(water_untouched(patched));
    REQUIRE(sound_gate_untouched(patched));

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched) == TbzInstruction);

    const auto unrelated = context.Manager(OtherTitleId).GetPatches();
    REQUIRE(std::ranges::find(unrelated, DynamicFps, &FileSys::Patch::name) == unrelated.end());
    REQUIRE(std::ranges::find(unrelated, FpsUnlock, &FileSys::Patch::name) == unrelated.end());
}

TEST_CASE("Built-in FPS Unlock skips the sound gate's sites only as a whole patch",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    auto nso = MakeDynamicMain();
    SECTION("Wrong original job entry") {
        nso[SoundEntryOffset] ^= 1;
    }
    SECTION("Wrong original step load") {
        nso[SoundStepOffset] ^= 1;
    }
    SECTION("Nonzero gate cave") {
        nso[SoundGateOffset + 4] = 1;
    }
    SECTION("Nonzero sound state") {
        nso[DynamicStateOffset + 0xA4] = 1;
    }
    // FPS Unlock doesn't apply, so Dark's routine does, without the sound gate.
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, DynamicCallOffset) == DynamicCallInstruction);
    REQUIRE(WordAt(patched, DynamicRoutineOffset + DynamicRoutineSize) == 0);
    REQUIRE(WordAt(patched, SoundStepOffset) != SoundStepCallInstruction);
    REQUIRE(WordAt(patched, SoundEntryOffset) != SoundEntryBranchInstruction);
}

TEST_CASE("Built-in Dynamic FPS rejects mismatched or truncated modules transactionally",
          "[patch_manager]") {
    PatchManagerTestContext context;
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    auto nso = MakeDynamicMain();
    std::string module = "main";
    u64 title = OcarinaTitleId;
    SECTION("Wrong build") {
        nso[offsetof(Loader::NSOHeader, build_id)] ^= 1;
    }
    SECTION("Wrong module") {
        module = "sdk";
    }
    SECTION("Wrong title") {
        title = OtherTitleId;
    }
    SECTION("Incomplete header") {
        nso.resize(sizeof(Loader::NSOHeader) - 1);
    }
    SECTION("Incomplete call") {
        nso.resize(DynamicCallOffset + 3);
    }
    SECTION("Incomplete routine") {
        nso.resize(DynamicRoutineOffset + DynamicRoutineSize - 1);
    }
    SECTION("Incomplete timer state") {
        nso.resize(DynamicStateOffset + DynamicStateSize - 1);
    }
    SECTION("Timer state outside BSS") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[2].bss_size = 15;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Wrong original hook") {
        nso[DynamicCallOffset] ^= 1;
    }
    SECTION("Nonzero code cave") {
        nso[DynamicRoutineOffset + DynamicRoutineSize - 1] = 1;
    }
    SECTION("Nonzero timer state") {
        nso[DynamicStateOffset + DynamicStateSize - 1] = 1;
    }
    SECTION("Wrong original water hook") {
        nso[WaterHookOffset] ^= 1;
    }
    SECTION("Wrong original water multiply") {
        nso[WaterMulOffset] ^= 1;
    }
    SECTION("Nonzero water code cave") {
        nso[WaterRoutineOffset + WaterRoutineSize - 1] = 1;
    }
    SECTION("Code cave outside executable padding") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[0].size = 0x3C46000;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    SECTION("Code cave overlaps read-only segment") {
        Loader::NSOHeader header;
        std::memcpy(&header, nso.data(), sizeof(header));
        header.segments[1].location = 0x3C46200;
        std::memcpy(nso.data(), &header, sizeof(header));
    }
    REQUIRE(std::ranges::equal(context.Manager(title).PatchNSO(nso, module), nso));
}

TEST_CASE("Built-in Dynamic FPS leaves both records alone when a user mod changes either",
          "[patch_manager]") {
    size_t offset = DynamicCallOffset;
    u32 mod_word = 0x11223344;
    SECTION("User call takes precedence") {}
    SECTION("Identical call still takes precedence for the whole patch") {
        mod_word = DynamicCallInstruction;
    }
    SECTION("User routine takes precedence") {
        offset = DynamicRoutineOffset;
    }
    SECTION("Last routine byte takes precedence") {
        offset = DynamicRoutineOffset + DynamicRoutineSize - sizeof(u32);
    }
    SECTION("User timer state takes precedence") {
        offset = DynamicStateOffset;
    }
    PatchManagerTestContext context{mod_word, offset};
    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    const auto nso = MakeDynamicMain();
    auto expected = nso;
    std::memcpy(expected.data() + offset, &mod_word, sizeof(mod_word));
    const auto actual = context.Manager().PatchNSO(nso, "main");
    const auto mismatch = std::ranges::mismatch(actual, expected);
    CAPTURE(offset);
    CAPTURE(mismatch.in1 - actual.begin());
    REQUIRE(mismatch.in1 == actual.end());
}

TEST_CASE("Built-in Texture Pool x2 doubles the allocator's initial pool size",
          "[patch_manager]") {
    constexpr std::string_view TexturePool = "Texture Pool x2 (built-in)";
    constexpr size_t CallOffset = sizeof(Loader::NSOHeader) + 0xC2A36C;
    constexpr size_t RoutineOffset = sizeof(Loader::NSOHeader) + 0x3C462C0;
    constexpr u32 StpInstruction = 0xA90127E8;
    constexpr u32 CallInstruction = 0x94C06FD5;
    PatchManagerTestContext context;
    const auto manager = context.Manager();
    auto nso = MakeDynamicMain();
    std::memcpy(nso.data() + CallOffset, &StpInstruction, sizeof(StpInstruction));

    auto patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == CallInstruction);
    REQUIRE(Common::HexToString(std::span{patched}.subspan(RoutineOffset, 12)) ==
            "08F97FD3E82701A9C0035FD6");
    REQUIRE(WordAt(patched, RoutineOffset + 12) == 0);

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{TexturePool}};
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == StpInstruction);
    REQUIRE(WordAt(patched, RoutineOffset) == 0);

    Settings::values.disabled_addons.erase(OcarinaTitleId);
    std::memcpy(nso.data() + CallOffset, &NopInstruction, sizeof(NopInstruction));
    patched = manager.PatchNSO(nso, "main");
    REQUIRE(WordAt(patched, CallOffset) == NopInstruction);
    REQUIRE(WordAt(patched, RoutineOffset) == 0);
}

TEST_CASE("High refresh rate built-in replaces Dark's routine only while its setting is on",
          "[patch_manager]") {
    constexpr std::string_view HighRefresh = "High refresh rate (built-in, experimental)";
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr size_t BaseRoutine = Code + 0x3C46200;
    constexpr size_t Routine = Code + 0x3C46E00;
    constexpr size_t CallOffset = Code + 0x6E6CC4;
    constexpr std::array<u8, 4> DynamicCall{0x4F, 0x7D, 0xD5, 0x94};
    constexpr std::array<u8, 4> HighCall{0x4F, 0x80, 0xD5, 0x94};
    const auto word_at = [](const std::vector<u8>& nso, size_t offset) {
        u32 word;
        std::memcpy(&word, nso.data() + offset, sizeof(word));
        return word;
    };
    const auto listed = [](const FileSys::PatchManager& manager, std::string_view name) {
        const auto patches = manager.GetPatches();
        return std::ranges::find(patches, name, &FileSys::Patch::name) != patches.end();
    };
    struct RestoreSetting {
        ~RestoreSetting() {
            Settings::values.oot_high_refresh.SetValue(false);
        }
    } restore;

    PatchManagerTestContext context;
    auto nso = MakeDynamicMain();

    SECTION("Off: FPS Unlock remains active and high refresh is not listed") {
        Settings::values.oot_high_refresh.SetValue(false);
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), patched.begin() + CallOffset));
        REQUIRE(word_at(patched, BaseRoutine + 0x7C) == 0x40C00000);
        REQUIRE(word_at(patched, SoundEntryOffset) == SoundEntryBranchInstruction);
        REQUIRE(word_at(patched, SoundStepOffset) == SoundStepCallInstruction);
        REQUIRE(word_at(patched, SoundGateOffset) != 0);
        REQUIRE(word_at(patched, SoundFeedOffset) != 0);
        for (size_t offset = 0; offset < 0x110; offset += 4) {
            REQUIRE(word_at(patched, Routine + offset) == 0);
        }
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)));
        REQUIRE_FALSE(listed(context.Manager(), HighRefresh));
        REQUIRE(listed(context.Manager(), DynamicFps));
    }

    SECTION("On: the high refresh routine, with its speed scale word") {
        Settings::values.oot_high_refresh.SetValue(true);
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(std::equal(HighCall.begin(), HighCall.end(), patched.begin() + CallOffset));
        REQUIRE(word_at(patched, BaseRoutine + 0x7C) == 0x40C00000);
        REQUIRE(word_at(patched, SoundEntryOffset) == SoundEntryBranchInstruction);
        REQUIRE(word_at(patched, SoundStepOffset) == SoundStepCallInstruction);
        REQUIRE(word_at(patched, SoundGateOffset) != 0);
        REQUIRE(word_at(patched, SoundFeedOffset) != 0);
        REQUIRE(word_at(patched, Routine + 0x18) == 0xB40002CC); // cbz x12, +0x70 (Dark's)
        REQUIRE(word_at(patched, Routine + 0x6C) == 0x14000002); // b +0x74
        REQUIRE(word_at(patched, Routine + 0x70) == 0xD65F03C0); // ret
        REQUIRE(word_at(patched, Routine + 0x78) == 0x1E230848); // fmul s8, s2, s3
        REQUIRE(word_at(patched, Routine + 0x84) == 0x40400000); // 3.0f
        REQUIRE(word_at(patched, Routine + 0x88) == 0x3F800000); // 1.0f
        REQUIRE(word_at(patched, Routine + 0x8C) == 0);
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)) ==
                std::optional<size_t>{0x3C46E88});
        REQUIRE(listed(context.Manager(), HighRefresh));
    }

    SECTION("On, with another mod immediately after FPS Unlock") {
        Settings::values.oot_high_refresh.SetValue(true);
        nso[BaseRoutine + 0x80] = 0x5A;
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(patched[BaseRoutine + 0x80] == 0x5A);
        REQUIRE(std::equal(HighCall.begin(), HighCall.end(), patched.begin() + CallOffset));
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)) ==
                std::optional<size_t>{0x3C46E88});
    }

    SECTION("On, with the extended cave in use: FPS Unlock remains active") {
        Settings::values.oot_high_refresh.SetValue(true);
        nso[Routine + 0x100] = 1;
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, BaseRoutine + 0x7C) == 0x40C00000);
        REQUIRE(word_at(patched, Routine + 0x100) == 1);
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)));
    }

    SECTION("On, but disabled in Add-Ons: FPS Unlock remains active") {
        Settings::values.oot_high_refresh.SetValue(true);
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{HighRefresh}};
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, BaseRoutine + 0x7C) == 0x40C00000);
    }
}

TEST_CASE("High refresh rate mode switches live between the two boot layouts",
          "[patch_manager]") {
    using FileSys::OotHighRefreshLayout;
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr size_t Scale = 0x88; // the speed scale word, in the cave
    struct RestoreSetting {
        ~RestoreSetting() {
            Settings::values.oot_high_refresh.SetValue(false);
        }
    } restore;

    // What boots with the setting off and on.
    PatchManagerTestContext context;
    const auto nso = MakeOcarinaMain();
    Settings::values.oot_high_refresh.SetValue(false);
    const auto off = context.Manager().PatchNSO(nso, "main");
    Settings::values.oot_high_refresh.SetValue(true);
    const auto on = context.Manager().PatchNSO(nso, "main");
    REQUIRE(off != on);
    const auto off_parts = FileSys::ReadOotHighRefreshCode(std::span<const u8>{off}.subspan(Code));
    const auto on_parts = FileSys::ReadOotHighRefreshCode(std::span<const u8>{on}.subspan(Code));
    REQUIRE(off_parts);
    REQUIRE(on_parts);
    REQUIRE(FileSys::GetOotHighRefreshLayout(*off_parts) == OotHighRefreshLayout::Off);
    REQUIRE(FileSys::GetOotHighRefreshLayout(*on_parts) == OotHighRefreshLayout::On);

    SECTION("Switching writes exactly what boots in the other mode, both ways") {
        auto module = off;
        const auto switched_on = FileSys::SwitchOotHighRefreshCode(*off_parts, true);
        REQUIRE(switched_on);
        WriteOotHighRefreshCode(std::span{module}.subspan(Code), *switched_on);
        REQUIRE(std::ranges::equal(module, on));
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span<const u8>{module}.subspan(Code)) ==
                std::optional<size_t>{0x3C46E88});

        const auto switched_off = FileSys::SwitchOotHighRefreshCode(*switched_on, false);
        REQUIRE(switched_off);
        WriteOotHighRefreshCode(std::span{module}.subspan(Code), *switched_off);
        REQUIRE(std::ranges::equal(module, off));
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(std::span<const u8>{module}.subspan(Code)));
    }

    SECTION("Switching to the mode it is in changes nothing, the speed scale word kept") {
        auto turbo = *on_parts;
        constexpr std::array<u8, 4> TwoPointZero{0x00, 0x00, 0x00, 0x40}; // 2.0f
        std::ranges::copy(TwoPointZero, turbo.cave.begin() + Scale);
        REQUIRE(FileSys::GetOotHighRefreshLayout(turbo) == OotHighRefreshLayout::On);
        const auto still_on = FileSys::SwitchOotHighRefreshCode(turbo, true);
        REQUIRE(still_on);
        REQUIRE(still_on->cave == turbo.cave);
        REQUIRE(still_on->water == turbo.water);

        const auto still_off = FileSys::SwitchOotHighRefreshCode(*off_parts, false);
        REQUIRE(still_off);
        REQUIRE(still_off->cave == off_parts->cave);
        REQUIRE(still_off->water == off_parts->water);

        // Off from any speed scale: Dark's cave, the scale word zero again.
        const auto from_turbo = FileSys::SwitchOotHighRefreshCode(turbo, false);
        REQUIRE(from_turbo);
        REQUIRE(from_turbo->cave == off_parts->cave);
        REQUIRE(from_turbo->water == off_parts->water);
    }

    SECTION("Code in neither mode is never switched") {
        const auto refused = [](const FileSys::OotHighRefreshCode& parts) {
            return FileSys::GetOotHighRefreshLayout(parts) == OotHighRefreshLayout::Unknown &&
                   !FileSys::SwitchOotHighRefreshCode(parts, true) &&
                   !FileSys::SwitchOotHighRefreshCode(parts, false);
        };
        auto used_cave = *off_parts; // a word past Dark's routine in use
        used_cave.cave[0x100] = 1;
        REQUIRE(refused(used_cave));
        auto stray_scale = *off_parts; // Off holds no speed scale word
        stray_scale.cave[Scale + 3] = 0x3F;
        REQUIRE(refused(stray_scale));
        auto modified_unlock = *off_parts;
        modified_unlock.unlock[0] ^= 1;
        REQUIRE(refused(modified_unlock));
        auto no_call = *off_parts; // The FPS patch is off: the game's own fdiv
        no_call.call = {0x28, 0x18, 0x20, 0x1E};
        REQUIRE(refused(no_call));
        auto modded_water = *on_parts; // a mod's word at the water hook
        modded_water.water = {0x1F, 0x20, 0x03, 0xD5};
        REQUIRE(refused(modded_water));
        auto unpatched_water = *off_parts;
        unpatched_water.water = {0x09, 0x00, 0x38, 0x1E};
        REQUIRE(refused(unpatched_water));
        auto half_written = *off_parts; // the first half of the high refresh cave
        std::copy_n(on_parts->cave.begin(), 0x80, half_written.cave.begin());
        REQUIRE(refused(half_written));
        auto changed_routine = *on_parts;
        changed_routine.cave[0x5C] ^= 1;
        REQUIRE(refused(changed_routine));
        auto changed_accumulator = *on_parts;
        changed_accumulator.cave[0x10C] ^= 1;
        REQUIRE(refused(changed_accumulator));
    }

    SECTION("A module too small for the cave holds neither") {
        const auto cut = std::span<const u8>{off}.subspan(Code, OcarinaCave + 0x10F);
        REQUIRE_FALSE(FileSys::ReadOotHighRefreshCode(cut));
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(cut));
    }

    SECTION("Its state (Dark's timer, the ripple slots, 0x4E9FE00..0x4E9FEA0) must lie in BSS") {
        REQUIRE(FileSys::OotHighRefreshStateInBss(0x4100000, 0x5100000));
        REQUIRE(FileSys::OotHighRefreshStateInBss(0x4E9FE00, 0x4E9FEA0));
        REQUIRE_FALSE(FileSys::OotHighRefreshStateInBss(0x4E9FE01, 0x5100000));
        REQUIRE_FALSE(FileSys::OotHighRefreshStateInBss(0x4100000, 0x4E9FE17));
        REQUIRE_FALSE(FileSys::OotHighRefreshStateInBss(0, 0));
    }
}

TEST_CASE("Dark's standalone Dynamic FPS mod is skipped and hidden for the built-in",
          "[patch_manager]") {
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr u32 CallOffset = Code + 0x6E6CC4;
    constexpr std::array<u8, 4> DynamicCall{0x4F, 0x7D, 0xD5, 0x94};
    constexpr std::array<u8, 4> Nop{0x1F, 0x20, 0x03, 0xD5};
    const auto ips_name = fmt::format("{}.ips", OcarinaBuildIdHex);
    const auto listed = [](const FileSys::PatchManager& manager, std::string_view name) {
        const auto patches = manager.GetPatches();
        return std::ranges::find(patches, name, &FileSys::Patch::name) != patches.end();
    };

    PatchManagerTestContext context;
    auto nso = MakeOcarinaMain();
    std::memcpy(nso.data() + PatchOffset, &TbzInstruction, sizeof(TbzInstruction));
    // What boots without mods: Dark's call and Max Resolution's branch.
    const auto builtin = context.Manager().PatchNSO(nso, "main");
    REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), builtin.begin() + CallOffset));
    REQUIRE(WordAt(builtin) == BranchInstruction);

    SECTION("An IPS writing Dark's call: skipped, not listed, the built-in applied") {
        const auto ips = MakeIps(false, CallOffset, Nop);
        const auto path = context.AddModFile("Dynamic FPS", ips_name, ips);
        REQUIRE(std::ranges::equal(context.Manager().PatchNSO(nso, "main"), builtin));
        REQUIRE_FALSE(listed(context.Manager(), "Dynamic FPS"));
        REQUIRE(listed(context.Manager(), DynamicFps));
        // The player's files stay as they were.
        REQUIRE(std::filesystem::file_size(path) == ips.size());
    }

    SECTION("An IPSwitch patch writing into Dark's routine: the same") {
        constexpr std::string_view Pchtxt = "@nsobid-E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348\n"
                                            "@flag offset_shift 0x100\n"
                                            "@enabled\n"
                                            "03C46274 00000000\n"
                                            "@stop\n";
        context.AddModFile("Dynamic FPS 60", "dynamic_fps.pchtxt",
                           {reinterpret_cast<const u8*>(Pchtxt.data()), Pchtxt.size()});
        REQUIRE(std::ranges::equal(context.Manager().PatchNSO(nso, "main"), builtin));
        REQUIRE_FALSE(listed(context.Manager(), "Dynamic FPS 60"));
        REQUIRE(listed(context.Manager(), DynamicFps));
    }

    SECTION("A mod patching an unrelated offset still applies and is listed") {
        constexpr u32 Unrelated = Code + 0x1000;
        context.AddModFile("Other mod", ips_name, MakeIps(true, Unrelated, Nop));
        auto expected = builtin;
        std::ranges::copy(Nop, expected.begin() + Unrelated);
        REQUIRE(std::ranges::equal(context.Manager().PatchNSO(nso, "main"), expected));
        REQUIRE(listed(context.Manager(), "Other mod"));
    }

    SECTION("Next to an unrelated mod, only the Dynamic FPS mod is left out") {
        constexpr u32 Unrelated = Code + 0x1000;
        context.AddModFile("Dynamic FPS", ips_name, MakeIps(false, CallOffset, Nop));
        context.AddModFile("Other mod", ips_name, MakeIps(true, Unrelated, Nop));
        auto expected = builtin;
        std::ranges::copy(Nop, expected.begin() + Unrelated);
        REQUIRE(std::ranges::equal(context.Manager().PatchNSO(nso, "main"), expected));
        REQUIRE(listed(context.Manager(), "Other mod"));
        REQUIRE_FALSE(listed(context.Manager(), "Dynamic FPS"));
    }

    SECTION("A mod patching Max Resolution's word still wins over that built-in") {
        context.AddModFile("Other mod", ips_name, MakeIps(true, PatchOffset, Nop));
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(WordAt(patched) == NopInstruction);
        REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), patched.begin() + CallOffset));
        REQUIRE(listed(context.Manager(), "Other mod"));
    }
}
