// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <doctest/doctest.h>
#include <string>
#include <sweeps/Clock.hpp>
#include <sweeps/Result.hpp>
#include <sweeps/Text.hpp>

using namespace sweeps;

TEST_CASE("the mini-formatter substitutes positionally and escapes braces") {

    CHECK(detail::format("no arguments") == "no arguments");
    CHECK(detail::format("{}", 42) == "42");
    CHECK(detail::format("{} and {}", 1, 2) == "1 and 2");
    CHECK(detail::format("{}-extract", std::string("session")) == "session-extract");
    CHECK(detail::format("{}", std::string_view("view")) == "view");
    CHECK(detail::format("{}", "literal") == "literal");
    CHECK(detail::format("{}", true) == "true");
    CHECK(detail::format("{}", 'x') == "x");

    // Braces double to escape, as in std::format.
    CHECK(detail::format("{{}}") == "{}");
    CHECK(detail::format("{{{}}}", 7) == "{7}");

    // Surplus arguments are dropped and missing ones leave the placeholder
    // consuming nothing -- a wrong count must not truncate the message around
    // it, because that message is what an operator reads.
    CHECK(detail::format("{}", 1, 2) == "1");
    CHECK(detail::format("{} {}", 1) == "1 ");

    // A specification is not understood, so it is passed through rather than
    // silently swallowed along with the argument.
    CHECK(detail::format("{:.2f}", 1) == "{:.2f}");
}

TEST_CASE("formatFrequencyShort renders four significant digits") {
    // These exact strings reach the file: a segment's `reason` is built from
    // this function, and `reason` is written into the SegmentOpen record. Drift
    // here is a change in file content, not a cosmetic change.
    CHECK(formatFrequencyShort(2.4e9) == "2.4 GHz");
    CHECK(formatFrequencyShort(868.3e6) == "868.3 MHz");
    CHECK(formatFrequencyShort(12.5e3) == "12.5 kHz");
    CHECK(formatFrequencyShort(100.0) == "100 Hz");
    CHECK(formatFrequencyShort(0.0) == "0 Hz");
    CHECK(formatFrequencyShort(9765.625) == "9.766 kHz");
    CHECK(formatFrequencyShort(19531.25) == "19.53 kHz");
    CHECK(formatFrequencyShort(-2.4e9) == "-2.4 GHz");
}

TEST_CASE("formatBytes uses binary units") {
    CHECK(formatBytes(0) == "0 B");
    CHECK(formatBytes(512) == "512 B");
    CHECK(formatBytes(1024) == "1.0 KiB");
    CHECK(formatBytes(1536 * 1024) == "1.5 MiB");
    CHECK(formatBytes(3ULL * 1024 * 1024 * 1024) == "3.00 GiB");
    CHECK(formatBytes(2ULL * 1024 * 1024 * 1024 * 1024) == "2.00 TiB");
}

TEST_CASE("clock formatters render the strings a manifest records") {
    CHECK(formatWallClockIso8601(0) == "1970-01-01T00:00:00Z");
    CHECK(formatWallClockCompact(0) == "19700101-000000");
    CHECK(formatWallClockIso8601(1'770'000'000'000'000'000ULL) == "2026-02-02T02:40:00Z");

    CHECK(formatDuration(0.000'001) == "1.0 us");
    CHECK(formatDuration(0.0125) == "12.50 ms");
    CHECK(formatDuration(1.5) == "1.500 s");
    CHECK(formatDuration(90.0) == "1:30");
    CHECK(formatDuration(3725.0) == "1:02:05");

    const std::uint64_t first = monotonicNs();
    CHECK(monotonicNs() >= first);
}

TEST_CASE("an Error carries a code, a message and prependable context") {
    const Error error{ErrorCode::Corrupt, "index checksum mismatch"};
    CHECK(error.code() == ErrorCode::Corrupt);
    CHECK(error.describe() == "Corrupt: index checksum mismatch");

    const Error contextual = error.withContext("reading session.sweeps");
    CHECK(contextual.code() == ErrorCode::Corrupt);
    CHECK(contextual.message() == "reading session.sweeps: index checksum mismatch");

    // A code with no message describes as just the code, rather than as a code
    // followed by a dangling colon.
    CHECK(Error{ErrorCode::NotFound, ""}.describe() == "NotFound");
    CHECK(Error{ErrorCode::NotFound, ""}.withContext("opening").message() == "opening");
}

TEST_CASE("Result carries either a value or an error") {
    const Result<int> good = 42;
    REQUIRE(good.has_value());
    CHECK(*good == 42);

    const Result<int> bad =
        detail::fail<int>(ErrorCode::InvalidArgument, "size {} is not valid", 7);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code() == ErrorCode::InvalidArgument);
    CHECK(bad.error().message() == "size 7 is not valid");

    CHECK(ok().has_value());
    const Status failed = detail::fail(ErrorCode::IoError, "disk gone");
    CHECK_FALSE(failed.has_value());
}
