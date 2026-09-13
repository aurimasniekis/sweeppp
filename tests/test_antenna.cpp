// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <sweeppp/core/Clock.hpp>
#include <sweeppp/rf/Antenna.hpp>

using namespace sweeppp;

namespace {

/// A directory that removes itself, for the shadowing cases which need two.
class ScopedDir {
public:
    explicit ScopedDir(std::string_view tag)
        : m_path(std::filesystem::temp_directory_path() /
                 std::format("sweeppp-antennas-{}-{}", tag, monotonicNs())) {
        std::filesystem::create_directories(m_path);
    }
    ~ScopedDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    std::filesystem::path write(std::string_view name, std::string_view text) const {
        const std::filesystem::path file = m_path / name;
        std::ofstream out(file);
        out << text;
        return file;
    }

private:
    std::filesystem::path m_path;
};

constexpr std::string_view kTwoGood = R"(
[[antenna]]
id = "d190"
name = "Diamond D-190"
category = "Wideband"
type = "discone"
start = "25 MHz"
stop = "1.3 GHz"
gain_dbi = 2.1
bias_t = false

[[antenna]]
id = "horn"
name = "Broadband horn"
category = "Microwave"
type = "horn"
start = 1.0e9
stop = 6.0e9
gain_dbi = 12.0
bias_t = true
notes = "boresight only"
)";

} // namespace

TEST_CASE("a good antenna file loads every field") {
    const ScopedDir dir("good");
    const std::filesystem::path file = dir.write("lib.toml", kTwoGood);

    std::vector<std::string> problems;
    const auto library = AntennaLibrary::load(file, &problems);
    REQUIRE(library.has_value());
    CHECK(problems.empty());
    REQUIRE(library->entries().size() == 2);

    const Antenna* horn = library->find("horn");
    REQUIRE(horn != nullptr);
    CHECK(horn->name == "Broadband horn");
    CHECK(horn->category == "Microwave");
    CHECK(horn->type == "horn");
    CHECK(horn->startHz == doctest::Approx(1.0e9));
    CHECK(horn->stopHz == doctest::Approx(6.0e9));
    CHECK(horn->gainDbi == doctest::Approx(12.0));
    CHECK(horn->needsBiasT);
    CHECK(horn->notes == "boresight only");

    // "25 MHz" and 25000000.0 have to mean the same thing, or half the file
    // formats in the application would parse one way and this one another.
    const Antenna* discone = library->find("d190");
    REQUIRE(discone != nullptr);
    CHECK(discone->startHz == doctest::Approx(25e6));
    CHECK(discone->stopHz == doctest::Approx(1.3e9));
}

TEST_CASE("one bad row costs its own row, not the file") {
    // The whole point of the per-row report: an operator who fat-fingers one
    // stop frequency must not lose the other nine antennas they own.
    const ScopedDir dir("badrow");
    const std::filesystem::path file = dir.write("lib.toml", R"(
[[antenna]]
name = "Good one"
start = "100 MHz"
stop = "1 GHz"

[[antenna]]
name = "Backwards"
start = "1 GHz"
stop = "100 MHz"

[[antenna]]
name = "Nameless neighbour"
start = "2 GHz"
stop = "3 GHz"
)");

    std::vector<std::string> problems;
    const auto library = AntennaLibrary::load(file, &problems);
    REQUIRE(library.has_value());
    CHECK(library->entries().size() == 2);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().find("Backwards") != std::string::npos);

    // The id was never written down; it comes from the name.
    CHECK(library->find("good-one") != nullptr);
    CHECK(library->find("nameless-neighbour") != nullptr);
}

TEST_CASE("a file with no antenna table at all fails outright") {
    const ScopedDir dir("empty");
    const std::filesystem::path file = dir.write("lib.toml", "[something_else]\nkey = 1\n");

    const auto library = AntennaLibrary::load(file, nullptr);
    CHECK_FALSE(library.has_value());
}

TEST_CASE("a user entry shadows a shipped one carrying the same id") {
    const ScopedDir user("user");
    const ScopedDir shipped("shipped");

    shipped.write("starter.toml", R"(
[[antenna]]
id = "d190"
name = "Shipped discone"
start = "25 MHz"
stop = "1.3 GHz"

[[antenna]]
id = "only-shipped"
name = "Untouched"
start = "1 GHz"
stop = "2 GHz"
)");
    user.write("custom.toml", R"(
[[antenna]]
id = "d190"
name = "My discone"
start = "30 MHz"
stop = "1.2 GHz"
)");

    // Search-path order: user directory first.
    const std::array<std::filesystem::path, 2> directories{user.path(), shipped.path()};
    const AntennaLibrary library = AntennaLibrary::discover(directories, nullptr);

    REQUIRE(library.entries().size() == 2);

    const Antenna* shadowed = library.find("d190");
    REQUIRE(shadowed != nullptr);
    CHECK(shadowed->name == "My discone");
    CHECK_FALSE(shadowed->builtin);

    const Antenna* untouched = library.find("only-shipped");
    REQUIRE(untouched != nullptr);
    CHECK(untouched->builtin);
}

TEST_CASE("covers() is closed at both ends and rejects a range it only half contains") {
    const Antenna antenna{.id = "a", .name = "A", .startHz = 100e6, .stopHz = 1e9};

    CHECK(antenna.covers(100e6));
    CHECK(antenna.covers(1e9));
    CHECK(antenna.covers(500e6));
    CHECK_FALSE(antenna.covers(99.9e6));
    CHECK_FALSE(antenna.covers(1.000001e9));

    CHECK(antenna.covers(200e6, 800e6));
    CHECK(antenna.covers(100e6, 1e9));
    // Half inside is half measured through a stopband, which is not coverage.
    CHECK_FALSE(antenna.covers(800e6, 1.2e9));
    CHECK_FALSE(antenna.covers(50e6, 200e6));

    const Antenna degenerate{.id = "b", .name = "B", .startHz = 1e9, .stopHz = 1e9};
    CHECK_FALSE(degenerate.covers(1e9));
    CHECK_FALSE(degenerate.covers(1e9, 1e9));
}

TEST_CASE("a saved library reloads to the same antennas") {
    const ScopedDir dir("roundtrip");

    AntennaLibrary library;
    library.add(Antenna{.id = "d190",
                        .name = "Diamond D-190",
                        .category = "Wideband",
                        .type = "discone",
                        .startHz = 25e6,
                        .stopHz = 1.3e9,
                        .gainDbi = 2.1,
                        .needsBiasT = false,
                        .notes = "roof"});
    library.add(Antenna{.id = "adsb",
                        .name = "ADS-B stick",
                        .category = "ADS-B",
                        .type = "collinear",
                        .startHz = 1.05e9,
                        .stopHz = 1.13e9,
                        .gainDbi = 5.0,
                        .needsBiasT = true});

    const std::filesystem::path file = dir.path() / "custom.toml";
    REQUIRE(library.saveUserFile(file).has_value());

    std::vector<std::string> problems;
    const auto reloaded = AntennaLibrary::load(file, &problems);
    REQUIRE(reloaded.has_value());
    CHECK(problems.empty());
    REQUIRE(reloaded->entries().size() == 2);

    const Antenna* adsb = reloaded->find("adsb");
    REQUIRE(adsb != nullptr);
    CHECK(adsb->name == "ADS-B stick");
    CHECK(adsb->needsBiasT);
    CHECK(adsb->gainDbi == doctest::Approx(5.0));

    // Exactly, not approximately: the frequency was written as a raw double
    // rather than through the six-decimal formatter, so a save-load-save cycle
    // must not walk the operator's number.
    const Antenna* discone = reloaded->find("d190");
    REQUIRE(discone != nullptr);
    CHECK(discone->startHz == 25e6);
    CHECK(discone->stopHz == 1.3e9);
}

TEST_CASE("saving writes only what the operator can edit") {
    const ScopedDir user("saveuser");
    const ScopedDir shipped("saveshipped");

    shipped.write("starter.toml", R"(
[[antenna]]
id = "shipped"
name = "Shipped"
start = "1 GHz"
stop = "2 GHz"
)");

    const std::array<std::filesystem::path, 2> directories{user.path(), shipped.path()};
    AntennaLibrary library = AntennaLibrary::discover(directories, nullptr);
    library.add(Antenna{.id = "mine", .name = "Mine", .startHz = 100e6, .stopHz = 200e6});
    REQUIRE(library.entries().size() == 2);

    const std::filesystem::path file = user.path() / "custom.toml";
    REQUIRE(library.saveUserFile(file).has_value());

    const auto written = AntennaLibrary::load(file, nullptr);
    REQUIRE(written.has_value());
    REQUIRE(written->entries().size() == 1);
    CHECK(written->entries().front().id == "mine");

    // Editing a shipped entry writes a copy over it, and the copy is what the
    // user file then holds -- deleting it is how the original comes back.
    Antenna edited = *library.find("shipped");
    edited.name = "Shipped, adjusted";
    library.add(std::move(edited));
    REQUIRE(library.saveUserFile(file).has_value());

    const auto second = AntennaLibrary::load(file, nullptr);
    REQUIRE(second.has_value());
    CHECK(second->entries().size() == 2);
    REQUIRE(second->find("shipped") != nullptr);
    CHECK(second->find("shipped")->name == "Shipped, adjusted");

    // Deleting the copy restores the original: the shipped file was never
    // touched, so the next discover finds it again under its own name.
    library.remove("shipped");
    REQUIRE(library.saveUserFile(file).has_value());

    const AntennaLibrary restored = AntennaLibrary::discover(directories, nullptr);
    REQUIRE(restored.find("shipped") != nullptr);
    CHECK(restored.find("shipped")->name == "Shipped");
    CHECK(restored.find("shipped")->builtin);
}

TEST_CASE("makeId never hands back an id the library already uses") {
    AntennaLibrary library;
    CHECK(library.makeId("Diamond D-190") == "diamond-d-190");

    library.add(
        Antenna{.id = "diamond-d-190", .name = "Diamond D-190", .startHz = 25e6, .stopHz = 1.3e9});
    CHECK(library.makeId("Diamond D-190") == "diamond-d-190-2");

    // A name with nothing alphanumeric in it still has to yield something.
    CHECK_FALSE(library.makeId("///").empty());
}
