// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace sweeppp::ui {

/// Icon glyphs, as UTF-8 literals for the merged icon font.
///
/// The codepoints live in plane 15 (U+F0001 upward), which is why ImWchar is
/// widened to 32 bits in the imgui config -- a 16-bit one would truncate every
/// one of these to a different character.
///
/// Named for what they *mean* here rather than for the shape the font calls
/// them, so swapping a glyph for a better one is a one-line change and every
/// call site keeps reading correctly. The font's own name is in the comment
/// beside each so the mapping can be checked against its cheat sheet.
namespace icon {

inline constexpr const char* kMenu = "\xF3\xB0\x8D\x9C";        // menu           F035C
inline constexpr const char* kDevice = "\xF3\xB1\x84\x99";      // antenna        F1119
inline constexpr const char* kRange = "\xF3\xB0\x98\xAE";       // tune           F062E
inline constexpr const char* kAnalysis = "\xF3\xB0\xB1\x90";    // chart-bell-curve F0C50
inline constexpr const char* kMarker = "\xF3\xB0\x8D\x8E";      // map-marker      F034E
inline constexpr const char* kChart = "\xF3\xB0\x84\xAA";       // chart-line     F012A
inline constexpr const char* kWaterfall = "\xF3\xB1\xA1\x89";   // waterfall      F1849
inline constexpr const char* kZoomIn = "\xF3\xB0\x9B\xAD";      // magnify-plus-outline  F06ED
inline constexpr const char* kZoomOut = "\xF3\xB0\x9B\xAC";     // magnify-minus-outline F06EC
inline constexpr const char* kResetZoom = "\xF3\xB0\x81\xAF";   // backup-restore F006F
inline constexpr const char* kRecord = "\xF3\xB0\xBB\x82";      // record-circle F0EC2
inline constexpr const char* kSnapshot = "\xF3\xB0\xB5\x9D";    // camera-outline F0D5D
inline constexpr const char* kHistory = "\xF3\xB0\x8B\x9A";     // history F02DA
inline constexpr const char* kPerformance = "\xF3\xB0\x93\x85"; // speedometer  F04C5
inline constexpr const char* kPlay = "\xF3\xB0\x90\x8A";        // play F040A
inline constexpr const char* kStop = "\xF3\xB0\x93\x9B";        // stop F04DB
inline constexpr const char* kPause = "\xF3\xB0\x8F\xA4";       // pause F03E4
inline constexpr const char* kSkipStart = "\xF3\xB0\x92\xAE";   // skip-previous F04AE
inline constexpr const char* kSkipEnd = "\xF3\xB0\x92\xAD";     // skip-next F04AD
inline constexpr const char* kRewind = "\xF3\xB0\x91\x9F";      // rewind F045F
inline constexpr const char* kForward = "\xF3\xB0\x88\x91";     // fast-forward F0211
inline constexpr const char* kOpen = "\xF3\xB0\x9D\xB0";        // folder-open F0770
inline constexpr const char* kReload = "\xF3\xB0\x91\x90";      // refresh F0450
inline constexpr const char* kClose = "\xF3\xB0\x85\x96";       // close F0156
inline constexpr const char* kDownload = "\xF3\xB0\x87\x9A";    // download F01DA
inline constexpr const char* kDelete = "\xF3\xB0\x86\xB4";      // delete F01B4
inline constexpr const char* kSettings = "\xF3\xB0\x92\x93";    // cog F0493
inline constexpr const char* kAdd = "\xF3\xB0\x90\x95";         // plus         F0415
inline constexpr const char* kEdit = "\xF3\xB0\x8F\xAB";        // pencil       F03EB
inline constexpr const char* kStar = "\xF3\xB0\x93\x8E";        // star         F04CE
inline constexpr const char* kStarOff = "\xF3\xB0\x93\x92";     // star-outline F04D2
inline constexpr const char* kDetach = "\xF3\xB0\x8F\x8C";      // open-in-new  F03CC

// One per panel arrangement.
inline constexpr const char* kLayoutSingle = "\xF3\xB0\xB9\x9F";  // rectangle-outline     F0E5F
inline constexpr const char* kLayoutColumns = "\xF3\xB0\xAF\x8C"; // view-split-vertical   F0BCC
inline constexpr const char* kLayoutRows = "\xF3\xB0\xAF\x8B";    // view-split-horizontal F0BCB
inline constexpr const char* kLayoutThree = "\xF3\xB1\x92\x8E";   // view-quilt-outline    F148E
inline constexpr const char* kLayoutGrid = "\xF3\xB1\x87\x99";    // view-grid-outline     F11D9
inline constexpr const char* kLayoutSix = "\xF3\xB1\x92\x8C";     // view-module-outline   F148C
inline constexpr const char* kLayoutNine = "\xF3\xB0\x8B\x81";    // grid                  F02C1

// One per toast severity.
inline constexpr const char* kInfo = "\xF3\xB0\x8B\xBC";    // information  F02FC
inline constexpr const char* kSuccess = "\xF3\xB0\x97\xA0"; // check-circle F05E0
inline constexpr const char* kWarning = "\xF3\xB0\x80\xA6"; // alert        F0026
inline constexpr const char* kError = "\xF3\xB0\x80\xA8";   // alert-circle F0028

/// Whether the icon font was successfully merged into the atlas.
///
/// A process-wide fact about the font, set once at start-up and read from
/// every panel. Without the check, a build whose font failed to load would
/// draw a toolbar of empty boxes with no way to tell what any button does.
[[nodiscard]] bool available() noexcept;
void setAvailable(bool value) noexcept;

/// The glyph when icons are available, the words when they are not.
[[nodiscard]] std::string glyphOr(const char* glyph, const char* fallback);

/// Lowest and highest codepoints the font is asked to provide.
///
/// A range rather than an explicit list: the atlas in ImGui 1.92 loads glyphs
/// on demand, so covering the block costs nothing until a glyph is actually
/// drawn, and adding an icon later needs no change here.
inline constexpr unsigned int kFirstCodepoint = 0xF0001;
inline constexpr unsigned int kLastCodepoint = 0xF1FFF;

} // namespace icon
} // namespace sweeppp::ui
