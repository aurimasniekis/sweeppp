// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/DynamicLibrary.hpp"

#include <string>

#if defined(_WIN32)
#include <format>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace sweeppp {
namespace {

#if defined(_WIN32)
/// FormatMessage into a std::string, so a load failure reads as prose rather
/// than as an error number the operator has to look up.
std::string lastErrorMessage() {
    const DWORD code = GetLastError();
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return std::format("error {}", static_cast<unsigned long>(code));
    }

    std::wstring wide(buffer, length);
    LocalFree(buffer);
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) {
        wide.pop_back();
    }

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(),
                        bytes, nullptr, nullptr);
    return narrow;
}
#else
/// dlerror() is a one-shot: it clears itself, and returns null when nothing
/// has failed. Both are easy to get wrong at the call site, so it is wrapped
/// once here.
std::string lastErrorMessage() {
    const char* message = dlerror();
    return message != nullptr ? std::string(message) : std::string("no error reported");
}
#endif

} // namespace

Result<DynamicLibrary> DynamicLibrary::open(const std::filesystem::path& path) {
    DynamicLibrary library;
    library.m_path = path;

#if defined(_WIN32)
    library.m_handle = static_cast<void*>(LoadLibraryW(path.c_str()));
#else
    // RTLD_LOCAL, and it matters: a plugin brings its own copy of ImGui, and
    // RTLD_GLOBAL would let those symbols interpose on the host's. Two
    // deliberate copies sharing one context is the supported arrangement; one
    // copy sometimes silently replacing the other is not.
    //
    // RTLD_NOW rather than RTLD_LAZY so a missing symbol is a load failure
    // reported here, with the name, instead of a crash at the first call.
    (void)dlerror();
    library.m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

    if (library.m_handle == nullptr) {
        return fail<DynamicLibrary>(ErrorCode::IoError, "{}: {}", path.string(),
                                    lastErrorMessage());
    }
    return library;
}

Result<void*> DynamicLibrary::symbol(const char* name) const {
    if (m_handle == nullptr) {
        return fail<void*>(ErrorCode::InvalidArgument, "no library is loaded");
    }

#if defined(_WIN32)
    // A function pointer to void* needs the cast on Windows; the pointer sizes
    // agree on every ABI we target.
    void* found = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
    (void)dlerror();
    void* found = dlsym(m_handle, name);
#endif

    if (found == nullptr) {
        return fail<void*>(ErrorCode::NotFound, "{}: no symbol '{}' ({})", m_path.string(), name,
                           lastErrorMessage());
    }
    return found;
}

} // namespace sweeppp
