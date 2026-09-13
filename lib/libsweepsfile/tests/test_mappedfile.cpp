// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sweeps/Clock.hpp>
#include <sweeps/MappedFile.hpp>

using namespace sweeps;

namespace {

class ScopedTempFile {
public:
    explicit ScopedTempFile(const std::string& name)
        : m_path(std::filesystem::temp_directory_path() /
                 ("sweepsfile-map-" + std::to_string(monotonicNs()) + "-" + name)) {}

    ~ScopedTempFile() {
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    void write(const std::string& contents) const {
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("a mapped file exposes its bytes and its size") {
    const ScopedTempFile file("basic.bin");
    const std::string contents = "SWPP and then some payload bytes";
    file.write(contents);

    auto mapped = MappedFile::open(file.path());
    REQUIRE(mapped.has_value());
    CHECK(mapped->valid());
    CHECK(mapped->size() == contents.size());
    CHECK(std::memcmp(mapped->data(), contents.data(), contents.size()) == 0);
}

TEST_CASE("a mapping is movable and releases exactly once") {
    const ScopedTempFile file("moved.bin");
    file.write("0123456789");

    auto opened = MappedFile::open(file.path());
    REQUIRE(opened.has_value());

    MappedFile moved = std::move(*opened);
    CHECK(moved.valid());
    CHECK(moved.size() == 10);

    // The moved-from mapping must be inert: it is destroyed at the end of this
    // scope too, and a double unmap is the failure this guards against.
    CHECK_FALSE(opened->valid());
    CHECK(opened->size() == 0);

    MappedFile assigned;
    assigned = std::move(moved);
    CHECK(assigned.valid());
    CHECK(assigned.size() == 10);
    CHECK_FALSE(moved.valid());
}

TEST_CASE("mapping reports missing and empty files distinctly") {
    const ScopedTempFile missing("absent.bin");
    auto absent = MappedFile::open(missing.path());
    REQUIRE_FALSE(absent.has_value());
    CHECK(absent.error().code() == ErrorCode::NotFound);

    // An empty file is not a mapping failure to explain in platform terms; the
    // system call refuses zero-length mappings on every platform we target, so
    // it is reported for what it is.
    const ScopedTempFile empty("empty.bin");
    empty.write("");
    auto mapped = MappedFile::open(empty.path());
    REQUIRE_FALSE(mapped.has_value());
    CHECK(mapped.error().code() == ErrorCode::Corrupt);
}

TEST_CASE("a directory is not a mappable file") {
    auto mapped = MappedFile::open(std::filesystem::temp_directory_path());
    REQUIRE_FALSE(mapped.has_value());
    CHECK(mapped.error().code() == ErrorCode::NotFound);
}
