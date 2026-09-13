// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"

#include <filesystem>

namespace sweeppp {

/// One loaded shared object, and a symbol lookup on it.
///
/// The project's first and only use of dynamic loading. Deliberately tiny: the
/// interesting decisions -- what to load, what to do when it will not load,
/// when to give up on unloading -- all belong to PluginHost, and a loader that
/// also had opinions about them would be two things at once.
///
/// **`close()` does not exist, and that is a decision.** See the note on
/// `PluginManager` in `sweeppp/plugin/PluginHost.hpp`: a module stays mapped
/// for the life of the process. The destructor therefore drops the handle
/// without calling `dlclose`, which is why this type is movable without any of
/// the usual care about double-unloading.
class DynamicLibrary {
public:
    DynamicLibrary() = default;

    /// Loads `path`. The error carries the platform's own message --
    /// `dlerror()` or a formatted `GetLastError()` -- because "could not load"
    /// on its own tells an operator nothing about a missing dependency, a
    /// wrong architecture or a permission problem, and those are most of what
    /// actually goes wrong.
    [[nodiscard]] static Result<DynamicLibrary> open(const std::filesystem::path& path);

    /// A symbol, or an error naming it. Never returns a null pointer with a
    /// successful status: a symbol that resolves to null is a failure here.
    [[nodiscard]] Result<void*> symbol(const char* name) const;

    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    void* m_handle = nullptr;
    std::filesystem::path m_path;
};

} // namespace sweeppp
