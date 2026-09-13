// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace sweeppp::ui {

/// The operating system's own save dialog.
///
/// A text field is not a substitute. It cannot browse, cannot say whether a
/// directory exists, will not warn before overwriting, and does not know the
/// places the operator has bookmarked -- all of which the platform dialog does
/// for free and everyone already knows how to drive.
///
/// Runs modally: the render loop stops until the dialog is dismissed, which is
/// what a save dialog is supposed to do. Must be called from the thread that
/// owns the window.
///
/// Returns the chosen path, or nothing if the operator cancelled. A cancel and
/// a failure are deliberately not distinguished at the call site -- neither
/// should write a file -- but a failure is logged.
[[nodiscard]] std::optional<std::filesystem::path>
saveFileDialog(const std::filesystem::path& defaultDirectory, const std::string& defaultName,
               const std::string& filterLabel, const std::string& filterExtension);

/// The operating system's own open dialog, with the same contract.
[[nodiscard]] std::optional<std::filesystem::path>
openFileDialog(const std::filesystem::path& defaultDirectory, const std::string& filterLabel,
               const std::string& filterExtension);

/// Shows `path` in the desktop's file manager: Finder, Explorer, whatever the
/// session provides.
///
/// A file is revealed with its parent open around it; a directory is opened.
/// Returns false when there is nothing on this machine that can do it, which
/// is a real case -- a headless session, a minimal container -- and one the
/// caller should be able to fall back from rather than pretend succeeded.
///
/// Does not block: it hands the request to the desktop and returns. Must be
/// called from the thread that owns the window.
bool revealInFileManager(const std::filesystem::path& path);

/// Opens `url` in whatever the session uses for the web.
///
/// The same mechanism as revealing a file, and deliberately the same contract:
/// it hands the request to the desktop and returns, and false means nothing on
/// this machine could take it -- a real case on a minimal container, and one
/// the caller should fall back from by showing the address instead.
///
/// http and https only. A URL reaching a desktop opener runs whatever handler
/// is registered for its scheme, and this one is fed from the network.
bool openInBrowser(const std::string& url);

} // namespace sweeppp::ui
