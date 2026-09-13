// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: MIT

#include "Commands.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace sweeps;

/// Reads `--name value` out of the argument list.
///
/// Deliberately tiny: this binary exists to make the format reachable without
/// a C++ toolchain, and an argument parser with a dependency of its own would
/// undercut that.
class Arguments {
public:
    Arguments(int argc, char** argv) {
        for (int i = 0; i < argc; ++i) {
            m_items.emplace_back(argv[i]);
        }
    }

    [[nodiscard]] const std::string& at(std::size_t index) const {
        static const std::string empty;
        return index < m_items.size() ? m_items[index] : empty;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }

    /// The first argument that is not an option or an option's value.
    [[nodiscard]] std::string positional(std::size_t which) const {
        std::size_t seen = 0;
        for (std::size_t i = 2; i < m_items.size(); ++i) {
            if (m_items[i].rfind("--", 0) == 0) {
                if (!isFlag(m_items[i])) {
                    ++i; // skip its value
                }
                continue;
            }
            if (seen++ == which) {
                return m_items[i];
            }
        }
        return {};
    }

    [[nodiscard]] bool has(std::string_view name) const {
        for (const std::string& item : m_items) {
            if (item == name) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string value(std::string_view name, std::string fallback = {}) const {
        for (std::size_t i = 0; i + 1 < m_items.size(); ++i) {
            if (m_items[i] == name) {
                return m_items[i + 1];
            }
        }
        return fallback;
    }

    [[nodiscard]] double number(std::string_view name, double fallback) const {
        const std::string text = value(name);
        if (text.empty()) {
            return fallback;
        }
        try {
            return std::stod(text);
        } catch (const std::exception&) {
            return fallback;
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> index(std::string_view name) const {
        const std::string text = value(name);
        if (text.empty()) {
            return std::nullopt;
        }
        try {
            return static_cast<std::uint32_t>(std::stoul(text));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

private:
    [[nodiscard]] static bool isFlag(const std::string& name) {
        return name == "--quiet" || name == "--help";
    }

    std::vector<std::string> m_items;
};

} // namespace

int main(int argc, char** argv) {
    const Arguments args(argc, argv);
    const std::string command = args.at(1);

    if (command.empty() || command == "help" || command == "--help" || command == "-h") {
        cli::printUsage(std::cout);
        return command.empty() ? 1 : 0;
    }

    if (command == "version" || command == "--version") {
        return cli::runVersion(std::cout);
    }

    const std::string path = args.positional(0);
    if (path.empty()) {
        std::cerr << "sweeps: " << command << " needs a .sweeps file\n";
        return 1;
    }

    if (command == "info") {
        cli::InfoOptions options;
        options.path = path;
        options.maxEvents = static_cast<std::size_t>(args.number("--events", 20.0));
        return cli::runInfo(options, std::cout, std::cerr);
    }

    if (command == "verify") {
        cli::VerifyOptions options;
        options.path = path;
        options.quiet = args.has("--quiet");
        return cli::runVerify(options, std::cout, std::cerr);
    }

    if (command == "extract") {
        cli::ExtractOptions options;
        options.input = path;
        options.output = args.value("-o", args.value("--output"));
        options.fromSeconds = args.number("--from", 0.0);
        options.toSeconds = args.number("--to", 0.0);
        options.startHz = args.number("--start", 0.0);
        options.stopHz = args.number("--stop", 0.0);
        return cli::runExtract(options, std::cout, std::cerr);
    }

    if (command == "events") {
        cli::EventsOptions options;
        options.path = path;
        options.kind = args.value("--kind");
        return cli::runEvents(options, std::cout, std::cerr);
    }

    if (command == "manifest") {
        cli::ManifestOptions options;
        options.path = path;
        return cli::runManifest(options, std::cout, std::cerr);
    }

    if (command == "plugins") {
        cli::PluginsOptions options;
        options.path = path;
        options.pluginId = args.value("--plugin");
        return cli::runPlugins(options, std::cout, std::cerr);
    }

    if (command == "dump") {
        cli::DumpOptions options;
        options.path = path;
        options.segmentId = args.index("--segment");
        options.lod = args.index("--lod");
        options.fromSeconds = args.number("--from", 0.0);
        options.toSeconds = args.number("--to", 0.0);
        options.startHz = args.number("--start", 0.0);
        options.stopHz = args.number("--stop", 0.0);
        options.maxLines = static_cast<std::uint64_t>(args.number("--max-lines", 0.0));
        return cli::runDump(options, std::cout, std::cerr);
    }

    std::cerr << "sweeps: unknown command '" << command << "'\n";
    cli::printUsage(std::cerr);
    return 1;
}
