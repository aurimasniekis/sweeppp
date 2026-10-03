// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp/rf/Antenna.hpp"

#include "sweeppp/core/Paths.hpp"
#include "sweeppp/core/Toml.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace sweeppp {
namespace {

/// A frequency from a node that may be a number or a suffixed string, or
/// nullopt when the key is absent. Everything goes through the same parser as
/// every other config file, so "1.5 GHz", "1.5G" and 1500000000.0 are one
/// entry written three ways.
std::optional<double> frequencyAt(const ::toml::node_view<const ::toml::node>& node,
                                  std::string_view key) {
    auto parsed = toml_util::frequencyFrom(node, key);
    return parsed ? std::optional{*parsed} : std::nullopt;
}

} // namespace

bool Antenna::covers(double hz) const noexcept {
    return stopHz > startHz && hz >= startHz && hz <= stopHz;
}

bool Antenna::covers(double fromHz, double toHz) const noexcept {
    return stopHz > startHz && fromHz >= startHz && toHz <= stopHz;
}

std::string Antenna::describeRange() const {
    if (stopHz <= startHz) {
        return "no range";
    }
    return std::format("{} - {}", toml_util::formatFrequencyShort(startHz),
                       toml_util::formatFrequencyShort(stopHz));
}

Result<AntennaLibrary> AntennaLibrary::load(const std::filesystem::path& path,
                                            std::vector<std::string>* problems) {
    auto table = toml_util::load(path);
    if (!table) {
        return std::unexpected(table.error());
    }

    const auto report = [problems, &path](std::string text) {
        if (problems != nullptr) {
            problems->push_back(std::format("{}: {}", path.string(), text));
        }
    };

    const auto* rows = toml_util::at(*table, "antenna").as_array();
    if (rows == nullptr) {
        return fail<AntennaLibrary>(ErrorCode::ParseError, "{} defines no antennas", path.string());
    }

    AntennaLibrary library;

    for (const auto& node : *rows) {
        const auto* declared = node.as_table();
        if (declared == nullptr) {
            continue;
        }

        Antenna antenna;
        antenna.name = toml_util::getString(*declared, "name", "");
        if (antenna.name.empty()) {
            report("an antenna has no name");
            continue;
        }

        // The id may be left out of a hand-written file: what an operator
        // types is a name, and deriving the id from it is what stops every
        // entry needing a second unique string nothing displays.
        antenna.id = toml_util::getString(*declared, "id", "");
        if (antenna.id.empty()) {
            antenna.id = slugify(antenna.name);
        }
        if (antenna.id.empty()) {
            report(std::format("antenna '{}' yields no id", antenna.name));
            continue;
        }
        if (library.find(antenna.id) != nullptr) {
            report(std::format("antenna id '{}' is declared twice", antenna.id));
            continue;
        }

        const std::optional<double> start = frequencyAt((*declared)["start"], "start");
        const std::optional<double> stop = frequencyAt((*declared)["stop"], "stop");
        if (!start || !stop) {
            report(std::format("antenna '{}' gives no start and stop", antenna.name));
            continue;
        }
        if (*start <= 0.0 || *stop <= *start) {
            report(std::format("antenna '{}' spans nothing", antenna.name));
            continue;
        }

        antenna.startHz = *start;
        antenna.stopHz = *stop;
        antenna.category = toml_util::getString(*declared, "category", "");
        antenna.type = toml_util::getString(*declared, "type", "");
        antenna.gainDbi = toml_util::getDouble(*declared, "gain_dbi", 0.0);
        antenna.needsBiasT = toml_util::getBool(*declared, "bias_t", false);
        antenna.notes = toml_util::getString(*declared, "notes", "");

        library.m_entries.push_back(std::move(antenna));
    }

    library.sort();
    return library;
}

AntennaLibrary AntennaLibrary::discover(std::span<const std::filesystem::path> directories,
                                        std::vector<std::string>* problems) {
    AntennaLibrary library;

    // Decreasing precedence, so the first directory to name an id wins and
    // later ones only contribute what nobody has claimed yet. `builtin` is set
    // from the directory's position rather than from anything in the file: a
    // user file cannot declare itself unmodifiable, and a shipped one cannot
    // declare itself editable into a directory the installer owns.
    bool userDirectory = true;

    for (const std::filesystem::path& directory : directories) {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            userDirectory = false;
            continue;
        }

        // Sorted, so what an operator sees does not depend on the order the
        // filesystem happens to hand entries back in.
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".toml") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);

        for (const std::filesystem::path& file : files) {
            auto loaded = AntennaLibrary::load(file, problems);
            if (!loaded) {
                // Reported and skipped: one unparseable file must not cost the
                // operator every other antenna they own.
                if (problems != nullptr) {
                    problems->push_back(loaded.error().describe());
                }
                continue;
            }

            for (Antenna& antenna : loaded->m_entries) {
                if (library.find(antenna.id) != nullptr) {
                    continue;
                }
                antenna.builtin = !userDirectory;
                library.m_entries.push_back(std::move(antenna));
            }
        }

        userDirectory = false;
    }

    library.sort();
    return library;
}

const Antenna* AntennaLibrary::find(std::string_view id) const {
    const auto match = std::ranges::find_if(
        m_entries, [id](const Antenna& candidate) { return candidate.id == id; });
    return match != m_entries.end() ? &*match : nullptr;
}

AntennaLibrary AntennaLibrary::of(std::vector<Antenna> entries) {
    AntennaLibrary library;
    library.m_entries = std::move(entries);
    library.sort();
    return library;
}

void AntennaLibrary::add(Antenna antenna) {
    antenna.builtin = false;

    const auto existing = std::ranges::find_if(
        m_entries, [&antenna](const Antenna& candidate) { return candidate.id == antenna.id; });
    if (existing != m_entries.end()) {
        *existing = std::move(antenna);
    } else {
        m_entries.push_back(std::move(antenna));
    }
    sort();
}

void AntennaLibrary::remove(std::string_view id) {
    std::erase_if(m_entries, [id](const Antenna& candidate) {
        return !candidate.builtin && candidate.id == id;
    });
}

Status AntennaLibrary::saveUserFile(const std::filesystem::path& path) const {
    ::toml::table root;
    ::toml::array rows;

    for (const Antenna& antenna : m_entries) {
        if (antenna.builtin) {
            continue;
        }

        ::toml::table entry;
        entry.insert_or_assign("id", antenna.id);
        entry.insert_or_assign("name", antenna.name);
        if (!antenna.category.empty()) {
            entry.insert_or_assign("category", antenna.category);
        }
        if (!antenna.type.empty()) {
            entry.insert_or_assign("type", antenna.type);
        }
        // Raw Hz, not a formatted "2.4 GHz": the formatter rounds to six
        // decimals, so writing what it produced would move the operator's
        // number a little further every time the file is saved.
        entry.insert_or_assign("start", antenna.startHz);
        entry.insert_or_assign("stop", antenna.stopHz);
        entry.insert_or_assign("gain_dbi", antenna.gainDbi);
        entry.insert_or_assign("bias_t", antenna.needsBiasT);
        if (!antenna.notes.empty()) {
            entry.insert_or_assign("notes", antenna.notes);
        }
        rows.push_back(std::move(entry));
    }

    root.insert_or_assign("antenna", std::move(rows));
    return toml_util::save(path, root, "Sweep++ antennas");
}

std::string AntennaLibrary::makeId(std::string_view name) const {
    const std::string base = slugify(name).empty() ? std::string("antenna") : slugify(name);
    if (find(base) == nullptr) {
        return base;
    }

    for (int suffix = 2; suffix < 1000; ++suffix) {
        std::string candidate = std::format("{}-{}", base, suffix);
        if (find(candidate) == nullptr) {
            return candidate;
        }
    }
    return base;
}

void AntennaLibrary::sort() {
    std::ranges::stable_sort(m_entries,
                             [](const Antenna& a, const Antenna& b) { return a.name < b.name; });
}

} // namespace sweeppp
