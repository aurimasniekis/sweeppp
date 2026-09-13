// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#pragma once

#include "sweeps/Config.hpp"

#if SWEEPSFILE_HAS_STD_EXPECTED
#include <expected>
#else
#include "sweeps/vendor/expected.hpp"
#endif

#include <string>
#include <string_view>
#include <utility>

namespace sweeps {

/// Why an operation failed. Coarse on purpose: the human-readable message
/// carries the detail, the code exists so callers can branch (refuse to retry
/// an Unsupported, report a Corrupt differently from a NotFound) without
/// parsing strings.
enum class ErrorCode {
    Unknown,
    InvalidArgument,
    NotFound,
    Unsupported, ///< Understood, but this build cannot do it.
    Unavailable, ///< Could work, but not right now.
    IoError,
    ParseError,
    OutOfRange,
    OutOfMemory,
    TimedOut,
    Cancelled,
    PermissionDenied,
    AlreadyExists,
    DeviceError,
    ProtocolError, ///< Malformed remote message or session file.
    Corrupt,       ///< Data read back does not match what was written.
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

/// An error with a message attached. Errors cross module boundaries and end up
/// in front of an operator, so the message is part of the type rather than
/// something the caller has to reconstruct from a code.
class Error {
public:
    Error() = default;

    Error(ErrorCode code, std::string message) : m_code(code), m_message(std::move(message)) {}

    explicit Error(std::string message) : m_message(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
    [[nodiscard]] const std::string& message() const noexcept { return m_message; }

    /// "Corrupt: index checksum mismatch" -- what gets logged and what gets
    /// shown in a UI.
    [[nodiscard]] std::string describe() const;

    /// Prepends context while preserving the code, so a failure deep in the
    /// reader arrives at the caller saying which file it was reading.
    [[nodiscard]] Error withContext(std::string_view context) const;

private:
    ErrorCode m_code = ErrorCode::Unknown;
    std::string m_message;
};

/// The library's fallible return type.
///
/// `std::expected` when the standard this library was *compiled at* provides it,
/// and a vendored equivalent otherwise. The two have the same shape for
/// everything used here, so the switch is invisible at every call site.
///
/// Which one it is was decided when the library was configured and is recorded
/// in `Config.hpp`. It is deliberately not re-derived here from
/// `__cpp_lib_expected`: this header is read by consumers as well as by the
/// library, and a consumer on a newer standard than the compiled library would
/// otherwise see a different type of a different layout behind the same
/// function signatures -- a mismatch the linker cannot see, because return types
/// are not mangled.
///
/// The vendored copy is tl::expected, renamed into `sweeps::vendor` so that a
/// consumer using tl::expected itself cannot end up with two definitions of one
/// type. It is vendored rather than fetched so this library has no dependencies
/// at all: nothing to resolve, nothing to download, nothing for `find_package`
/// to fail on.
#if SWEEPSFILE_HAS_STD_EXPECTED
template <typename T>
using Result = std::expected<T, Error>;
using Status = std::expected<void, Error>;
using std::unexpected;
#else
template <typename T>
using Result = vendor::expected<T, Error>;
using Status = vendor::expected<void, Error>;
using vendor::unexpected;
#endif

/// Success for a `Status`.
///
/// Not constexpr: the vendored `expected`'s default constructor is not constexpr for
/// every T under C++17.
[[nodiscard]] inline Status ok() {
    return Status{};
}

namespace detail {

/// Builds an error result with a formatted message.
///
///     return detail::fail<Tile>(ErrorCode::Corrupt, "tile at {} is short", offset);
///
/// Deliberately in `detail` rather than in `sweeps`. Consumers that share this
/// error type -- Sweep++ does -- keep their own `fail()` returning
/// `std::unexpected`, and argument-dependent lookup on `sweeps::ErrorCode`
/// would otherwise drag this overload into every one of their call sites and
/// make each ambiguous. Library sources pull it in with a file-local
/// `using detail::fail;`.
template <typename T = void, typename... Args>
[[nodiscard]] unexpected<Error> fail(ErrorCode code, std::string_view fmt, Args&&... args);

} // namespace detail

} // namespace sweeps

#include "sweeps/Text.hpp"

namespace sweeps {
namespace detail {

template <typename T, typename... Args>
[[nodiscard]] unexpected<Error> fail(ErrorCode code, std::string_view fmt, Args&&... args) {
    return unexpected<Error>(Error{code, detail::format(fmt, std::forward<Args>(args)...)});
}

} // namespace detail
} // namespace sweeps
