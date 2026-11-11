// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

// Exercise the same save and backup helpers used by the dialog.
#include "yuzu/fritchii/saves_window.cpp"

namespace {
namespace fs = std::filesystem;

struct Fixture {
    fs::path root = fs::temp_directory_path() /
                    ("lichii-saves-test-" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root); }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void Write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
    REQUIRE(out.good());
}

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>{in}, {}};
}
} // namespace

TEST_CASE("Save copy selects the target profile and preserves its previous data", "[save_manager]") {
    Fixture fixture;
    const auto source_nand = fixture.root / "source" / "nand";
    const auto target_nand = fixture.root / "target" / "nand";
    const auto source = FritchiiUi::SaveRoot(source_nand);
    const auto target = FritchiiUi::SaveRoot(target_nand);
    constexpr u64 low = 0x8899AABBCCDDEEFFULL;
    constexpr u64 high = 0x0011223344556677ULL;
    constexpr auto user = "00112233445566778899AABBCCDDEEFF";
    constexpr auto game = "0400486024DEC000";
    std::array<char, 0x10 + 0xC8> profiles{};
    std::memcpy(profiles.data() + 0x10, &low, sizeof(low));
    std::memcpy(profiles.data() + 0x18, &high, sizeof(high));
    const auto profile_file = target_nand / "system/save/8000000000000010/su/avators/profiles.dat";
    Write(profile_file, std::string{profiles.begin(), profiles.end()});
    Write(source / "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" / game / "save.dat", "new");
    Write(target / user / game / "save.dat", "previous");
    const auto backup = FritchiiUi::BackupPath(target);
    REQUIRE(FritchiiUi::Backup(target, backup) == backup);
    REQUIRE(FritchiiUi::CopySaves(source, target) == 1);
    CHECK(Read(target / user / game / "save.dat") == "new");
    CHECK(Read(backup / user / game / "save.dat") == "previous");
    CHECK(Read(source / "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" / game / "save.dat") == "new");
    CHECK_THROWS(FritchiiUi::Backup(target, backup));
    CHECK(Read(backup / user / game / "save.dat") == "previous");
}
