// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <sweeppp/core/Result.hpp>
#include <sweeppp/ui/ColorMap.hpp>
#include <vector>

namespace bandplan {

using sweeppp::Result;
using sweeppp::ui::Color;

/// What kind of claim one entry makes, and so how the host draws it.
///
/// A plan is mostly allocations, but the same file is the natural home for the
/// handful of entries that are not -- a named channel inside an allocation, or
/// a single beacon frequency. Optional in the TOML: an entry that says nothing
/// is a band, which is what almost all of them are.
enum class ContributionKind : std::uint8_t { Band, Channel, Spot };

/// One allocated range, as it should appear under the spectrum.
struct Band {
    std::string name;

    /// A sentence about it, shown on hover. Present on a handful of the
    /// shipped entries and empty on the rest.
    std::string description;

    double startHz = 0.0;
    double stopHz = 0.0;

    /// Service category -- broadcast, amateur, ism and so on. Bands are
    /// coloured by it, so a glance says what *kind* of thing a signal is
    /// before its name has been read.
    std::string group;

    /// The same category, as an index into `BandPlan::groups()`. What the
    /// settings tree branches on, and what makes "is this band on" an array
    /// lookup rather than a string compare per band per frame.
    std::size_t groupIndex = 0;

    ContributionKind kind = ContributionKind::Band;
    Color color;

    [[nodiscard]] double widthHz() const noexcept { return stopHz - startHz; }

    /// Half-open, so abutting allocations do not both claim their shared edge.
    /// A spot has no width and is closed instead, or it would contain nothing.
    [[nodiscard]] bool contains(double hz) const noexcept {
        return stopHz > startHz ? (hz >= startHz && hz < stopHz) : hz == startHz;
    }
};

/// A named set of allocations, loaded from TOML.
///
/// A navigation aid, not a legal reference: allocations differ by country and
/// the shipped plans are simplified for display. That is why a user file of
/// the same name in the config directory replaces the built-in one outright
/// rather than merging -- someone correcting their region's plan wants their
/// version, not their version plus ours.
///
/// Lives in the plugin rather than in libsweeppp, and takes its directories as
/// arguments rather than asking `Paths`. Both follow from the same rule: a
/// plugin links libsweeppp for values and never touches a singleton, and
/// `Paths::instance()` inside a plugin is a second copy that would ignore
/// `--config-dir` and every test's override.
class BandPlan {
public:
    [[nodiscard]] static Result<BandPlan> load(const std::filesystem::path& path);

    /// Every plan found under `directories`, in order, a later directory's
    /// file replacing an earlier one of the same *plan name*.
    ///
    /// Never fails: a plan that will not parse is appended to `problems` and
    /// skipped, because a broken file in a directory should not stop the
    /// application drawing the rest. `problems` is an out-parameter rather
    /// than a log call so that this stays free of the host's logging -- the
    /// caller has a `Host` and this does not.
    [[nodiscard]] static std::vector<BandPlan>
    discover(std::span<const std::filesystem::path> directories,
             std::vector<std::string>* problems = nullptr);

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] const std::string& description() const noexcept { return m_description; }
    [[nodiscard]] const std::vector<Band>& bands() const noexcept { return m_bands; }

    /// The service categories this plan uses, sorted. The branches of the
    /// settings tree.
    [[nodiscard]] const std::vector<std::string>& groups() const noexcept { return m_groups; }

    /// One byte per group: 1 when it is not in `disabled`.
    ///
    /// A disabled-id set rather than an enabled one, the same idiom as
    /// `plugins.disabled`: a service category that appears in a later version
    /// of a plan file is on rather than hidden until someone finds the file.
    [[nodiscard]] std::vector<std::uint8_t>
    resolveEnabled(const std::set<std::string, std::less<>>& disabled) const;

    /// One byte per *band*: 1 when its group is on and its own key is not in
    /// `disabled`.
    ///
    /// The one mask both queries take, so unticking a row leaves the plot and
    /// the marker's chip together rather than by two rules that can drift.
    [[nodiscard]] std::vector<std::uint8_t>
    resolveBands(std::span<const std::uint8_t> groupOn,
                 const std::set<std::string, std::less<>>& disabled) const;

    /// The persistence key for one band: its group, a '#', its name.
    [[nodiscard]] static std::string bandKey(std::string_view group, std::string_view name);

    /// Every band containing a frequency, narrowest first.
    ///
    /// All of them rather than only the narrowest, because allocations nest:
    /// 2.4 GHz Wi-Fi sits inside a wider ISM allocation, and both are true at
    /// once. Narrowest leads because it is the more specific answer and the
    /// host reads the first entry as the title -- but the nesting is now
    /// something the operator can see rather than something this hides.
    ///
    /// `bandOn` is `resolveBands`'s answer, or empty for "every band".
    [[nodiscard]] std::vector<const Band*> bandsAt(double hz,
                                                   std::span<const std::uint8_t> bandOn = {}) const;

    /// Bands overlapping a range, for drawing. Sorted by start frequency.
    [[nodiscard]] std::vector<const Band*> bandsIn(double fromHz, double toHz,
                                                   std::span<const std::uint8_t> bandOn = {}) const;

private:
    std::string m_name;
    std::string m_description;
    std::vector<std::string> m_groups;
    std::vector<Band> m_bands;
};

} // namespace bandplan
