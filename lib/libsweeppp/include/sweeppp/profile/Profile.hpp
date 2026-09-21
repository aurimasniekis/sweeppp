// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sweeppp/core/Result.hpp"
#include "sweeppp/correction/Corrections.hpp"
#include "sweeppp/pipeline/Pipeline.hpp"
#include "sweeppp/sdr/SdrParameter.hpp"
#include "sweeppp/sweep/SweepPlan.hpp"
#include "sweeppp/ui/ViewSettings.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace sweeppp {

/// Everything needed to put the application back exactly as it was.
///
/// One structure rather than several files, because the parts are only
/// meaningful together: an RBW without the sample rate that produced it is not
/// a resolution, and a gradient range without the colour map it indexes is not
/// a picture. Restoring half of a setup is worse than restoring none of it.
///
/// This is also what the application saves on exit and reloads on start. A
/// session and a named profile are the same thing stored in different places,
/// which keeps one code path instead of two that drift.
struct Profile {
    std::string name;

    /// Which radio, and every parameter value it was set to.
    ///
    /// Stored as the generic key/value model rather than named fields, so a
    /// driver that gains a parameter gets it saved without touching this --
    /// the same property that lets one panel render any device.
    std::string deviceDriver;
    std::string deviceId;
    std::string deviceLabel;
    std::vector<std::pair<std::string, SdrValue>> deviceParameters;

    SweepPlan sweepPlan;
    bool sweeping = true;

    PipelineConfig pipeline;

    /// The correction switches, beside the pipeline config rather than in it:
    /// a `PipelineConfig` change restarts acquisition, and flipping the spur
    /// mask must not.
    CorrectionSettings corrections;

    ui::ViewSettings view;

    /// Split between the spectrum and the waterfall, as a fraction.
    float waterfallFraction = 0.45F;

    /// What plugins contributed, under `plugins.<plugin id>.<key>`.
    ///
    /// Carried opaquely and written back verbatim, which is the property that
    /// matters: a profile saved on a machine with a plugin still loads on one
    /// without it, keeps the keys, and hands them back if the plugin returns.
    /// Losing them would make a profile silently lossy in a way nothing on
    /// screen could show.
    std::vector<std::pair<std::string, SdrValue>> pluginValues;

    [[nodiscard]] static Result<Profile> load(const std::filesystem::path& path);
    [[nodiscard]] Status save(const std::filesystem::path& path) const;

    /// The profile flattened to the dotted keys it is saved under --
    /// "sweep.start", "display.grid", "device.driver" -- with each value
    /// keeping its type.
    ///
    /// For anything that has to read the configuration without knowing this
    /// struct: the plugin host serves it to plugins through
    /// `sweeppp_host_api_t::profile_get`, in the same shape a session manifest
    /// has. Built from the same table `save()` writes, so a key a plugin reads
    /// is a key the operator can find in their own settings.toml.
    ///
    /// Arrays are absent: the sweep plan's segments are the only ones, and a
    /// consumer that wants them wants ranges rather than "sweep.segments.0.start".
    [[nodiscard]] std::vector<std::pair<std::string, SdrValue>> entries() const;
};

} // namespace sweeppp
