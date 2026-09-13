// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// Dear ImGui build configuration for Sweep++ (IMGUI_USER_CONFIG).
//
// This file is compiled into every translation unit that sees imgui.h, host
// and plugin alike, so anything that changes struct layout MUST live here
// rather than on a single target's command line -- an ABI mismatch across the
// plugin boundary would be silent and fatal.
#pragma once

// A full-span sweep trace pushes far more than 65535 vertices into a single
// draw list. With 16-bit indices ImPlot would assert and truncate the trace.
#define ImDrawIdx unsigned int

// 32-bit codepoints, because the icon font lives above the BMP.
//
// Material Design Icons are mapped into plane 15 (U+F0001 upward), which a
// 16-bit ImWchar simply cannot represent -- every glyph would silently
// truncate to a different character. This changes the size of ImWchar and so
// belongs here with the other layout-affecting settings rather than on one
// target's command line.
#define IMGUI_USE_WCHAR32

// We never rely on removed APIs; keeping them out catches drift at compile
// time instead of at the next imgui bump.
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_OBSOLETE_KEYIO

// The GL 4.1 core renderer is the only backend we ship.
#define IMGUI_IMPL_OPENGL_LOADER_CUSTOM_DISABLE_NOTHING

// Convenient implicit conversions between ImVec2/ImVec4 and our own math
// types are deliberately NOT enabled: the UI layer converts explicitly so the
// unit boundary (screen pixels vs plot coordinates vs Hz) stays visible.
