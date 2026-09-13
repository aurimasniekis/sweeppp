// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/core/CrashHandler.hpp"

#include "sweeppp/core/Clock.hpp"
#include "sweeppp/core/Version.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h, and not by convention: dbghelp.h does not compile without
// the types windows.h defines, and a pass that sorts includes must not be able
// to put it first.
#include <dbghelp.h>
#else
#include <cerrno>
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sweeppp {
namespace {

/// Composed by install(), read by the handler.
///
/// Fixed buffers rather than std::string, because reading a std::string is
/// only safe if nothing was mid-assignment when the process died, and because
/// the handler must hand a plain `const char*` to open().
constexpr std::size_t kPathBytes = 1024;
constexpr std::size_t kHeaderBytes = 512;

std::array<char, kPathBytes> g_path{};
std::array<char, kHeaderBytes> g_header{};
std::size_t g_headerLength = 0;
std::atomic<bool> g_installed{false};

/// Copies as much as fits and always leaves a terminator. Truncating a path is
/// better than the alternatives available on this code path.
void store(std::array<char, kPathBytes>& destination, std::string_view text) {
    const std::size_t length = std::min(text.size(), destination.size() - 1);
    std::memcpy(destination.data(), text.data(), length);
    destination[length] = '\0';
}

#if !defined(_WIN32)

/// A stack of its own for the handler.
///
/// The crash most worth a trace is the one a runaway recursion causes, and
/// that arrives as SIGSEGV with no stack left to run a handler on. Without
/// SA_ONSTACK the handler faults immediately and the process dies silently.
///
/// A figure of our own rather than SIGSTKSZ, which since glibc 2.34 expands to
/// sysconf(_SC_SIGSTKSZ) -- a function call, and so not a constant this array
/// can be sized by at all. It is generous against every platform's minimum
/// (macOS 32 KiB, Linux 8-16 KiB, more on machines with wide vector state) and
/// against what runs on it: this handler writes fixed buffers and one
/// backtrace, so the depth is bounded and small.
constexpr std::size_t kAlternateStackBytes = static_cast<std::size_t>(256) * 1024;
std::array<char, kAlternateStackBytes> g_alternateStack{};

void writeAll(int fd, const char* data, std::size_t size) noexcept {
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void writeText(int fd, const char* text) noexcept {
    writeAll(fd, text, std::strlen(text));
}

/// snprintf allocates and takes locks on some platforms, so the one number
/// this report needs is formatted by hand.
void writeNumber(int fd, int value) noexcept {
    std::array<char, 16> buffer{};
    std::size_t index = buffer.size();
    unsigned magnitude =
        value < 0 ? 0U - static_cast<unsigned>(value) : static_cast<unsigned>(value);
    do {
        buffer[--index] = static_cast<char>('0' + (magnitude % 10U));
        magnitude /= 10U;
    } while (magnitude > 0U && index > 1);
    if (value < 0) {
        buffer[--index] = '-';
    }
    writeAll(fd, buffer.data() + index, buffer.size() - index);
}

const char* signalName(int number) noexcept {
    switch (number) {
    case SIGSEGV:
        return "SIGSEGV (invalid memory reference)";
    case SIGBUS:
        return "SIGBUS (bad memory access)";
    case SIGFPE:
        return "SIGFPE (arithmetic exception)";
    case SIGILL:
        return "SIGILL (illegal instruction)";
    case SIGABRT:
        return "SIGABRT (abort)";
    default:
        return "signal";
    }
}

extern "C" void handleSignal(int number) {
    // O_TRUNC rather than append: a second crash in the same run is a new
    // report, not more of the old one.
    const int fd = ::open(g_path.data(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        writeAll(fd, g_header.data(), g_headerLength);
        writeText(fd, "signal: ");
        writeText(fd, signalName(number));
        writeText(fd, " (");
        writeNumber(fd, number);
        writeText(fd, ")\n\nbacktrace:\n");

        std::array<void*, 128> frames{};
        const int depth = ::backtrace(frames.data(), static_cast<int>(frames.size()));
        ::backtrace_symbols_fd(frames.data(), depth, fd);
        ::close(fd);
    }

    // Back to the default disposition and re-raise, so the operating system
    // still writes its own report and the exit status is the signal rather
    // than a tidy zero that hides the crash from whatever launched us.
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    // Unqualified: sigemptyset is a macro on macOS, and `::` in front of a
    // macro expansion does not compile.
    sigemptyset(&action.sa_mask);
    ::sigaction(number, &action, nullptr);
    ::raise(number);
}

void installPlatform() {
    // The first backtrace() on glibc dlopen()s libgcc and allocates, neither
    // of which is safe from a signal handler. Doing it once here means the
    // lazy work is already done by the time it matters.
    std::array<void*, 4> warmup{};
    (void)::backtrace(warmup.data(), static_cast<int>(warmup.size()));

    stack_t alternate{};
    alternate.ss_sp = g_alternateStack.data();
    alternate.ss_size = g_alternateStack.size();
    alternate.ss_flags = 0;
    ::sigaltstack(&alternate, nullptr);

    struct sigaction action{};
    action.sa_handler = handleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_ONSTACK | SA_RESTART;

    for (const int number : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        ::sigaction(number, &action, nullptr);
    }
}

#else

void writeAll(HANDLE file, const char* data, std::size_t size) noexcept {
    DWORD written = 0;
    WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
}

void writeText(HANDLE file, const char* text) noexcept {
    writeAll(file, text, std::strlen(text));
}

const char* exceptionName(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    default:
        return "exception";
    }
}

LONG WINAPI handleException(EXCEPTION_POINTERS* info) {
    const HANDLE file = CreateFileA(g_path.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        writeAll(file, g_header.data(), g_headerLength);
        writeText(file, "exception: ");
        writeText(file, info != nullptr && info->ExceptionRecord != nullptr
                            ? exceptionName(info->ExceptionRecord->ExceptionCode)
                            : "unknown");
        writeText(file, "\n\nbacktrace:\n");

        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);

        std::array<void*, 128> frames{};
        const USHORT depth =
            CaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);

        // SYMBOL_INFO carries the name past the end of the struct, so the
        // buffer has to be larger than the type.
        std::array<char, sizeof(SYMBOL_INFO) + (MAX_SYM_NAME * sizeof(char))> symbolStorage{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        for (USHORT i = 0; i < depth; ++i) {
            const auto address = reinterpret_cast<DWORD64>(frames.at(i));
            DWORD64 displacement = 0;
            std::array<char, 64> line{};

            if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
                writeText(file, "  ");
                writeText(file, symbol->Name);
            } else {
                writeText(file, "  ??");
            }

            // wsprintfA neither allocates nor takes the CRT's locks, which is
            // what makes it usable from here.
            wsprintfA(line.data(), " [0x%llx]\n", address);
            writeText(file, line.data());
        }

        CloseHandle(file);
    }

    // The OS reporter still runs, so a crash dump and a Windows Error
    // Reporting entry are produced exactly as they would have been.
    return EXCEPTION_CONTINUE_SEARCH;
}

void installPlatform() {
    SetUnhandledExceptionFilter(handleException);
}

#endif

} // namespace

void CrashHandler::install(const std::filesystem::path& directory, std::string_view program) {
    if (g_installed.exchange(true)) {
        return;
    }

    const std::filesystem::path file =
        directory / ("sweeppp-crash-" + formatWallClockCompact(wallClockNs()) + ".log");
    store(g_path, file.string());

    // One line, composed now, so the report says which build produced it. A
    // trace whose addresses cannot be tied to a binary is not much of a
    // report.
    const std::string header = std::string("sweeppp crash report\nprogram: ")
                                   .append(program)
                                   .append("\nversion: ")
                                   .append(buildString())
                                   .append("\ntime:    ")
                                   .append(formatWallClockIso8601(wallClockNs()))
                                   .append("\n\n");
    g_headerLength = std::min(header.size(), g_header.size());
    std::memcpy(g_header.data(), header.data(), g_headerLength);

    installPlatform();
}

std::string_view CrashHandler::reportPath() noexcept {
    return {g_path.data()};
}

} // namespace sweeppp
