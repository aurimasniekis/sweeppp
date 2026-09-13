// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <sweeps/FileFormat.hpp>

/// The `.sweeps` container, re-exported.
///
/// The format and its codec live in libsweepsfile -- an MIT-licensed, C++17,
/// one-dependency library that knows nothing about SDR drivers, FFT backends or
/// this application. What remains here is a set of `using` declarations, so the
/// ~120 call sites that say `session::SessionReader` or `session::TileKey`
/// carry on saying it.
namespace sweeppp::session {

using sweeps::kKnownFeatures;
using sweeps::kMagic;
using sweeps::kMajorVersion;
using sweeps::kMinorVersion;

using sweeps::dequantiseDb;
using sweeps::isMeasured;
using sweeps::kDbPerStep;
using sweeps::kQuantSpanDb;
using sweeps::kUnmeasuredByte;
using sweeps::quantiseDb;

using sweeps::FileHeader;
using sweeps::RecordHeader;
using sweeps::RecordType;

using sweeps::SegmentGrid;
using sweeps::SegmentInfo;

using sweeps::IndexEntry;
using sweeps::kLodLevels;
using sweeps::kTileBins;
using sweeps::kTileLines;
using sweeps::lodDecimation;
using sweeps::TileHeader;
using sweeps::TileKey;

using sweeps::AnnotationData;
using sweeps::decodeEvent;
using sweeps::DeviceErrorData;
using sweeps::encodeEvent;
using sweeps::EventBody;
using sweeps::eventKindFromString;
using sweeps::eventKindName;
using sweeps::MarkerData;
using sweeps::ParameterChangedData;
using sweeps::PluginEventData;
using sweeps::RetuneData;
using sweeps::SegmentBoundaryData;
using sweeps::SessionEvent;
using sweeps::SweepPassData;
using sweeps::ThrottleChangedData;
using sweeps::toString;
using sweeps::UnknownEventData;

using sweeps::Metadata;
using sweeps::Value;

using sweeps::ByteReader;
using sweeps::crc32;
using sweeps::writeBytes;
using sweeps::writeF32;
using sweeps::writeF64;
using sweeps::writeString;
using sweeps::writeU16;
using sweeps::writeU32;
using sweeps::writeU64;
using sweeps::writeU8;

} // namespace sweeppp::session
