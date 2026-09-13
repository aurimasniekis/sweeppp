// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <sweeps/Result.hpp>
#include <utility>

namespace sweeppp {

/// The error type is libsweepsfile's, re-exported rather than duplicated.
///
/// One definition, not two: errors cross the boundary between the session
/// container and the rest of Sweep++ constantly, and a second `Error` type
/// would mean a translation at every crossing -- plus two `ErrorCode`
/// enumerations that drift apart the first time one gains a value.
///
/// What is *not* shared is the result type. `sweeps::Result` is
/// `tl::expected` because that library is C++17; Sweep++ is C++23 and has 88
/// `std::unexpected` sites. Sharing only the error makes the boundary a move
/// rather than a rewrite -- see `adopt()`.
using ErrorCode = sweeps::ErrorCode;
using Error = sweeps::Error;

/// The project-wide fallible return type.
template <typename T>
using Result = std::expected<T, Error>;

/// `Result<void>` for operations that either work or explain themselves.
using Status = std::expected<void, Error>;

/// Builds an error result with a formatted message.
///
///     return fail<FftPlan>(ErrorCode::InvalidArgument,
///                          "FFT size {} is not a power of two", size);
template <typename T = void, typename... Args>
[[nodiscard]] std::unexpected<Error> fail(ErrorCode code, std::format_string<Args...> fmt,
                                          Args&&... args) {
    return std::unexpected(Error{code, std::format(fmt, std::forward<Args>(args)...)});
}

template <typename... Args>
[[nodiscard]] std::unexpected<Error> fail(std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected(
        Error{ErrorCode::Unknown, std::format(fmt, std::forward<Args>(args)...)});
}

/// Success for a `Status`.
inline constexpr Status ok() {
    return Status{};
}

/// Moves a libsweepsfile result into a Sweep++ one.
///
/// The two differ only in their expected template -- `tl::expected` on one
/// side, `std::expected` on the other -- and share the error type, so this is a
/// move of the value or a move of the error, never a conversion. It exists
/// because the alternative was aliasing `sweeppp::Result` to `sweeps::Result`
/// and rewriting 88 `std::unexpected` sites to `tl::unexpected`.
template <typename T>
[[nodiscard]] Result<T> adopt(sweeps::Result<T>&& result) {
    if (!result) {
        return std::unexpected(std::move(result).error());
    }
    return std::move(result).value();
}

[[nodiscard]] inline Status adopt(sweeps::Status&& status) {
    if (!status) {
        return std::unexpected(std::move(status).error());
    }
    return ok();
}

/// Propagates an error out of the current function, or binds the value.
/// Statement-expression based, so it works only on GCC/Clang -- which is the
/// full set of compilers this project targets for the engine.
#define SWEEPPP_TRY(expr)                                                                          \
    ({                                                                                             \
        auto&& _sweeppp_result = (expr);                                                           \
        if (!_sweeppp_result) [[unlikely]] {                                                       \
            return std::unexpected(std::move(_sweeppp_result).error());                            \
        }                                                                                          \
        std::move(_sweeppp_result).value();                                                        \
    })

/// Propagates an error from a `Status`-returning call that yields no value.
#define SWEEPPP_TRY_VOID(expr)                                                                     \
    do {                                                                                           \
        auto&& _sweeppp_status = (expr);                                                           \
        if (!_sweeppp_status) [[unlikely]] {                                                       \
            return std::unexpected(std::move(_sweeppp_status).error());                            \
        }                                                                                          \
    } while (false)

} // namespace sweeppp

/// Lets `std::format("{}", error)` work. Declared on the library's type, which
/// is the only Error there is -- a formatter for `sweeppp::Error` would be a
/// redeclaration of this one, since the two names denote the same class.
template <>
struct std::formatter<sweeps::Error> : std::formatter<std::string> {
    auto format(const sweeps::Error& error, std::format_context& ctx) const {
        return std::formatter<std::string>::format(error.describe(), ctx);
    }
};
