# The `.sweeps` Session Container, Version 1

**Status:** Stable. Version 1.0 is frozen — no byte of it moves.
**Reference implementation:** libsweepsfile, the MIT library this document sits in.
**Media type:** `application/vnd.sweeps` (proposed, §13)
**Filename extension:** `.sweeps`

## Abstract

`.sweeps` is a self-describing container for recorded radio-spectrum
observations. It stores quantised spectrum levels as a two-dimensionally tiled,
time-decimated pyramid, alongside the acquisition configuration that produced
them and a stream of events describing everything that changed while recording.

The design targets four properties, and every structural decision here follows
from one of them:

- **Scrollback into a multi-hour session is cheap.** Tiles are blocked in time
  *and* frequency, and a level-of-detail pyramid means drawing three hours does
  not mean reading three hours of lines.
- **A truncated file is readable.** Every record carries its own length and a
  checksum, so a session ended by power loss recovers everything that reached
  the disk. This is a designed path, not a fallback.
- **A mid-session parameter change destroys nothing.** Each *segment* owns its
  own frequency grid and full acquisition configuration, so tiles written before
  a change stay interpretable at their original resolution forever.
- **Extraction is a copy.** Cutting a time and frequency range out of a session
  is a tile copy plus a new index — no decode, no re-quantisation, bit-identical
  output.

This document specifies the byte format completely enough to write an
independent reader or writer without consulting the reference implementation.

## Table of contents

1. [Introduction](#1-introduction)
2. [Conventions](#2-conventions)
3. [File structure](#3-file-structure)
4. [Records](#4-records)
5. [Segments and the frequency grid](#5-segments-and-the-frequency-grid)
6. [Tiles](#6-tiles)
7. [Quantisation](#7-quantisation)
8. [The LOD pyramid](#8-the-lod-pyramid)
9. [Integrity and recovery](#9-integrity-and-recovery)
10. [Extraction](#10-extraction)
11. [Versioning and compatibility](#11-versioning-and-compatibility)
12. [Security considerations](#12-security-considerations)
13. [IANA considerations](#13-iana-considerations)
- [Appendix A: Test vectors](#appendix-a-test-vectors)
- [Appendix B: Manifest profile](#appendix-b-manifest-profile)
- [Appendix C: Live streams](#appendix-c-live-streams)

---

## 1. Introduction

### 1.1. What a file contains

A `.sweeps` file is a fixed 32-byte header followed by a flat sequence of
length-prefixed, checksummed records. The records carry:

- one **manifest** — typed metadata about the session as a whole,
- one or more **segments**, each declaring a frequency grid and the acquisition
  configuration in force,
- **tiles** — the measurement data, as quantised levels,
- **events** — timestamped notes about what changed during recording,
- optionally, an **index** written at close,
- optionally, **plugin records** — a producer's own data, opaque to the
  container.

Records may appear in any order that satisfies the constraints in §4. In
practice a writer emits them in the order they occur, which is what makes the
file appendable and streamable.

### 1.2. What it is not

The format stores *display-resolution* spectra, not raw IQ and not full
sweep-resolution FFT output. Levels are quantised to 0.5 dB. It is a recording
of what was observed, at a fidelity chosen so that a multi-hour session is a few
hundred megabytes rather than tens of gigabytes.

### 1.3. Relationship to the network protocol

The record encoding in §4 is shared with the streaming protocol: streaming live
means emitting the same records over a socket instead of to a file. A remote
reader is therefore the same decoder as a file reader. The `Telemetry` record
(§4.9) exists only on that path and never appears in a file written by a
conforming v1 writer. Appendix C defines what differs on a stream.

---

## 2. Conventions

### 2.1. Requirements language

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**,
**SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY** and **OPTIONAL** in this
document are to be interpreted as described in BCP 14 (RFC 2119, RFC 8174) when,
and only when, they appear in all capitals.

### 2.2. Byte order and primitive types

All multi-byte scalars are **little-endian**, on every platform. A conforming
writer MUST NOT emit a struct by memory copy; layout is a compiler's business,
not a file format's.

| Name   | Size    | Encoding                                                       |
|--------|---------|----------------------------------------------------------------|
| `u8`   | 1       | unsigned integer                                               |
| `u16`  | 2       | unsigned integer, little-endian                                |
| `u32`  | 4       | unsigned integer, little-endian                                |
| `u64`  | 8       | unsigned integer, little-endian                                |
| `i64`  | 8       | signed integer, **two's complement**, little-endian            |
| `f32`  | 4       | IEEE 754 binary32, little-endian byte order of the bit pattern |
| `f64`  | 8       | IEEE 754 binary64, little-endian byte order of the bit pattern |
| `u8[]` | *n*     | raw bytes                                                      |
| `str`  | 4 + *n* | a `u32` byte count *n*, followed by *n* bytes of UTF-8         |

`str` is **not** NUL-terminated and its length counts bytes, not code points. An
empty string is a `u32` zero and nothing more. Readers MUST validate the length
against the bytes actually remaining before allocating (§12.2).

`i64` is the bit pattern of the `u64` with the same object representation:
`-1` is eight bytes of `0xFF`, and `INT64_MIN` is `00 00 00 00 00 00 00 80`. It
is **not** sign-and-magnitude and **not** zig-zag encoded. A reader implements it
as a `u64` read followed by a two's-complement reinterpretation; a writer does
the reverse. Stating this is not pedantry — it is the one primitive an
independent implementer is likely to guess wrong, and guessing wrong is silent
until a negative value appears.

A boolean is a `u8`. Writers MUST write `0` or `1`; readers MUST treat any
non-zero value as true.

### 2.3. Time

Two clocks appear, and they are not interchangeable.

- **Monotonic nanoseconds** (`monotonicNs`, `hostTimeNs`, `firstLineNs`, …)
  measure *duration* from an arbitrary, per-boot origin. Every time axis, LOD
  calculation and seek in this format is expressed in this clock, because it
  cannot jump backwards when the system clock is corrected.
- **Wall-clock nanoseconds** (`wallNs`, `createdWallNs`, `startWallNs`) are
  nanoseconds since the Unix epoch (1970-01-01T00:00:00Z), ignoring leap
  seconds. They exist so a human can be told *when* something happened. Readers
  MUST NOT compute intervals from them.

The two origins are unrelated. A file records both so that a session can be
placed on a calendar *and* measured internally.

### 2.4. Frequency and level units

Frequencies are hertz, as `f64`. Levels are decibels; the reference is carried
in the acquisition configuration (`dbfsToDbmOffset`) rather than baked into the
stored data, so a later calibration correction can be applied retroactively to
an old session.

### 2.5. Packet diagrams

Diagrams show byte offsets relative to the start of the structure, with each row
covering four bytes:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          field (u32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Bit 0 is the *first* bit transmitted of the *first* byte. Because scalars are
little-endian, the first byte of a `u32` carries its least significant eight
bits.

---

## 3. File structure

```
+--------------------------------------------------+  offset 0
| File header                            32 bytes  |
+--------------------------------------------------+  offset 32
| Record 0   (12-byte header + payload)            |
+--------------------------------------------------+
| Record 1                                         |
+--------------------------------------------------+
| ...                                              |
+--------------------------------------------------+
| Index record             (optional; see §9.3)    |  <- indexOffset
+--------------------------------------------------+
| End-of-stream record     (optional)              |
+--------------------------------------------------+  end of file
```

There is no padding between records and none at the end. A record begins
immediately after the previous record's payload.

### 3.1. File header

Exactly 32 bytes, at offset 0.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     'S'       |     'W'       |     'P'       |     'P'       |   0
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     majorVersion (u32)                        |   4
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |   8
+                      indexOffset (u64)                        +
|                                                               |  12
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |  16
+                    createdWallNs (u64)                        +
|                                                               |  20
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     minorVersion (u32)                        |  24
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 incompatibleFeatures (u32)                    |  28
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Offset | Size | Field                                                           | v1.0 value       |
|--------|------|-----------------------------------------------------------------|------------------|
| 0      | 4    | `magic` — the ASCII bytes `SWPP` (`0x53 0x57 0x50 0x50`)        | —                |
| 4      | 4    | `majorVersion` (`u32`)                                          | `1`              |
| 8      | 8    | `indexOffset` (`u64`) — file offset of the index record, or `0` | patched at close |
| 16     | 8    | `createdWallNs` (`u64`)                                         | —                |
| 24     | 4    | `minorVersion` (`u32`)                                          | `0`              |
| 28     | 4    | `incompatibleFeatures` (`u32`) — feature bitmask                | `0`              |

`indexOffset` is `0` when the file was not closed cleanly. **Zero is not an
error.** It means "recover by scanning" (§9.2), which is a designed path.

A writer MUST write `indexOffset` as `0` when it creates the file and MUST patch
it in place only after the index record has been written and flushed. Until that
instant the file correctly reads as "not cleanly closed" — which is exactly
right if the writer dies before reaching it.

`minorVersion` and `incompatibleFeatures` are specified in §11.

### 3.2. Record header

Every record is preceded by exactly 12 bytes.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          type (u16)           |         flags (u16)           |   0
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     payloadBytes (u32)                        |   4
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       checksum (u32)                          |   8
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field          | Type  | Meaning                                                            |
|----------------|-------|--------------------------------------------------------------------|
| `type`         | `u16` | Record type, from §4.1                                             |
| `flags`        | `u16` | Reserved. Writers MUST write `0`; readers MUST ignore unknown bits |
| `payloadBytes` | `u32` | Payload length, excluding this header                              |
| `checksum`     | `u32` | CRC-32/ISO-HDLC of the payload bytes (§9.1)                        |

Self-describing length plus a checksum is what makes truncation recoverable: a
reader can walk records until one does not fit or fails its checksum, and
everything before that point is intact.

`flags` is deliberately inert in v1. Anything a reader could not safely ignore
is signalled by a header feature bit (§11.3) instead, because a reader that
skipped an unknown *record flag* would be silently misreading a record it
believed it understood.

---

## 4. Records

### 4.1. Record types

| Value | Name           | Payload | Cardinality                             |
|-------|----------------|---------|-----------------------------------------|
| 1     | `Manifest`     | §4.2    | Exactly one, and it SHOULD be first     |
| 2     | `SegmentOpen`  | §4.3    | One or more                             |
| 3     | `SegmentClose` | §4.4    | At most one per segment                 |
| 4     | `Event`        | §4.5    | Any number                              |
| 5     | `Tile`         | §4.6    | Any number                              |
| 6     | `Index`        | §4.7    | At most one; last but for `EndOfStream` |
| 7     | `EndOfStream`  | empty   | At most one, and last                   |
| 8     | `Telemetry`    | §4.9    | Never in a file                         |
| 9     | `PluginData`   | §4.10   | Any number                              |

Values 0 and 10 upward are unassigned, except that `0xFF00`–`0xFFFF` are
reserved for private and experimental use (§11.7).

**Readers MUST skip records whose `type` they do not recognise**, using
`payloadBytes` to find the next record. This is what makes adding a record type a
minor version bump (§11.4) rather than a breaking change.

### 4.2. Manifest

```
<hash-body>            session metadata, §4.2.1
```

The payload is a typed key/value object, encoded directly — there is no
enclosing `str` and no embedded document. The keys a writer emits are listed in
Appendix B; readers MUST tolerate keys they do not recognise, and MUST tolerate
a key whose type is not the one they expect.

The manifest is metadata, not structure. A reader MUST NOT depend on it to
locate or interpret any other record. In particular, the keys `tile_bins`,
`tile_lines`, `lod_levels` and `db_per_step` are **informational**: a v1 reader
MUST use the fixed constants in §6 and §7 regardless of what the manifest says.

> A future major version MAY make these keys authoritative; v1 does not, and a
> v1 file whose geometry keys disagree with the constants is malformed rather
> than differently shaped.

#### 4.2.1. Typed metadata

The same encoding carries the manifest (§4.2) and a plugin event's fields
(§4.5.11). It is a closed, self-describing shape: an independent reader needs no
parser for any other language to read it.

```
<hash-body>  =  u32 count
                count x { str key ; <value> }

<value>      =  u8 type ; <body>

  type  name     body
  ----  -------  --------------------------------------------------------
     1  String   str
     2  Int      i64, two's complement, little-endian
     3  Float    f64
     4  Bool     u8
     5  Bytes    u32 byteCount, then byteCount raw bytes
     6  Hash     <hash-body>
     7  Array    u8 elementType ; u32 count ; count x <body>
```

Type values 0 and 8 upward are unassigned. A reader encountering one MUST refuse
the object rather than guess: unlike a record or an event, a value has no length
of its own, so an unknown type cannot be skipped — the reader does not know
where it ends.

**Keys MUST be emitted in ascending byte order** of the key, compared as
unsigned bytes. That is what makes the encoding a function of the content alone,
and therefore what makes a byte-frozen reference file possible. A reader MUST
NOT depend on the order, and MUST tolerate duplicate keys by taking the last.

**Bool** is written as `0` or `1`. A reader MUST treat any non-zero byte as true.

**Int** is `i64` in two's complement, written as though it were the `u64` with
the same bit pattern — so `-1` is eight bytes of `0xFF`. It is *not* a magnitude
with a sign bit, and it is not zig-zag encoded. A reader implements it as a `u64`
read followed by a two's-complement reinterpretation.

**Arrays are homogeneous.** The element type is stated once and the elements
carry no tag of their own, so an array cannot mix types. `elementType` may itself
be `Hash` or `Array`, which is how an array of objects is expressed; §12.6 caps
the resulting recursion.

An empty array still states an element type. A reader MUST NOT infer the element
type from the elements.

Both count fields are attacker-controlled sizes and MUST be validated before
anything is allocated — see §12.5.

### 4.3. SegmentOpen

```
u32    id                  segment identifier
f64    startHz             frequency of the first stored bin
f64    binWidthHz          spacing between stored bins
u32    binCount            stored bins per line
u64    startWallNs
u64    startMonotonicNs
str    reason              why this segment was opened
--- acquisition configuration ---
f64    centerHz
f64    spanHz
f64    sampleRate
u32    fftSize
u32    window              window function, §4.3.1
f64    windowBeta          Kaiser beta; meaningless for other windows
f64    windowEnbw          equivalent noise bandwidth, in bins
f64    overlap             0.0 .. 1.0
f64    rbwHz               sampleRate * windowEnbw / fftSize
f64    referenceLevelDbm
f64    dbfsToDbmOffset
str    deviceId
str    deviceLabel
u32    gainCount
  gainCount x {
    str  key
    f64  value
  }
```

`id` is the segment's identity, not its position. Readers MUST look segments up
by `id` and MUST NOT assume `id` equals the segment's ordinal position in the
file — an extracted file (§10) legitimately contains segments whose ids do not
start at zero and are not contiguous.

`reason` is free text describing what changed, for display in a timeline:
`"session start"`, `"FFT size 4096 -> 8192"`, `"sample rate 20 MHz -> 40 MHz"`,
`"frequency range -> 90 MHz .. 110 MHz"`. Readers MUST NOT parse it.

The acquisition configuration is carried in full so that old tiles remain
interpretable without reference to anything outside the file. Readers MUST NOT
require the payload to end after `gainCount` entries; see §11.5.

#### 4.3.1. Window function values

| Value | Window          |
|-------|-----------------|
| 0     | Rectangular     |
| 1     | Hann            |
| 2     | Hamming         |
| 3     | Blackman-Harris |
| 4     | Flat top        |
| 5     | Kaiser          |

Values 6 upward are unassigned. A reader encountering one SHOULD surface it as
an opaque number rather than refusing the file; the window affects how levels
should be *interpreted*, not whether they can be read, and `windowEnbw` — which
is what an interpretation actually needs — is carried explicitly for exactly
this reason.

### 4.4. SegmentClose

```
u32    id
u64    endMonotonicNs      timestamp of the segment's last line
u64    lineCount           lines written at LOD 0
```

A segment with no `SegmentClose` record was still open when the file ended.
Readers MUST treat that as ordinary — see §9.2 for how its extent is recovered.

`endMonotonicNs` is the last *frame's* timestamp, not the wall time at which the
writer happened to stop. A segment's extent is defined by its data.

### 4.5. Event

```
u16    kind                §4.5.1
u64    monotonicNs
u64    wallNs
u32    segmentId
<kind-specific body>       §4.5.2 .. §4.5.11
```

Events are what make replay *faithful*. Frames alone reproduce a waterfall; the
events reproduce everything around it, so on playback the RBW, FFT size, gain and
span readouts change at the same moments they did live.

**The kind determines the body**, exactly as a record's `type` determines its
payload. There is no field whose meaning varies with the kind.

`segmentId` identifies the segment in force when the event was written. A live
writer stamps events with the segment open at the moment of serialisation, which
may differ from a segment id the caller supplied; an extraction preserves
whatever value it decoded. Readers MUST treat `segmentId` as a hint and MUST NOT
assume a segment with that id exists in the file.

#### 4.5.1. Event kinds

| Value | Kind             | Name in text form | Body                 |
|-------|------------------|-------------------|----------------------|
| 1     | Retune           | `retune`          | §4.5.2               |
| 2     | ParameterChanged | `parameter`       | §4.5.3               |
| 3     | SweepPass        | `sweep-pass`      | §4.5.4               |
| 4     | Marker           | `marker`          | §4.5.5               |
| 5     | Annotation       | `annotation`      | §4.5.6               |
| 6     | Alert            | `alert`           | **Reserved**, §4.5.7 |
| 7     | SegmentBoundary  | `segment`         | §4.5.8               |
| 8     | ThrottleChanged  | `throttle`        | §4.5.9               |
| 9     | DeviceError      | `device-error`    | §4.5.10              |
| 10    | Plugin           | `plugin`          | §4.5.11              |

Values 0 and 11 upward are unassigned, except that `0xFF00`–`0xFFFF` are
reserved for private and experimental use (§11.7).

**Readers MUST keep an event whose kind they do not recognise.** The record
header carries `payloadBytes`, so the body's extent is known even when its shape
is not: a reader retains the raw `u16` kind and the undecoded body bytes, and
surfaces the numeric kind rather than discarding the event or forcing it into a
neighbouring meaning. This is the §11.5 guarantee for record types, extended to
event ids — and it is what an extraction relies on (§10.3).

#### 4.5.2. Retune

```
f64    centerHz
u32    stepIndex
```

#### 4.5.3. ParameterChanged

```
str    key
str    value
u8     gridAffecting            0 or 1
u8     calibrationAffecting     0 or 1
```

`gridAffecting` is true when the change redefines the frequency grid and
therefore closes the current segment (§5.3). `calibrationAffecting` is true when
it leaves the grid intact but moves the noise floor — gain, reference level — so
that later analysis of tiles on either side of it must know it happened.

Readers MUST treat any non-zero byte as true.

#### 4.5.4. SweepPass

```
u64    passId
f64    startHz
f64    stopHz
f64    durationSeconds
```

#### 4.5.5. Marker

```
str    label
f64    frequencyHz
f64    levelDbm
```

#### 4.5.6. Annotation

```
str    text
f64    startHz
f64    stopHz
```

#### 4.5.7. Alert

**Reserved.** The body is undefined and a conforming v1 writer MUST NOT emit an
event of this kind. Readers MUST treat it as they treat an unrecognised kind:
keep the event and keep the body bytes.

The kind is named rather than dropped because it is already assigned; defining a
body for a feature that does not exist would pin a shape nothing has tested.

#### 4.5.8. SegmentBoundary

```
str    reason
```

The same free text as the corresponding `SegmentOpen` record's `reason` (§4.3).
Readers MUST NOT parse it.

#### 4.5.9. ThrottleChanged

```
str    reason
f64    processedFraction        0.0 .. 1.0
```

#### 4.5.10. DeviceError

```
str    deviceId
str    message
```

#### 4.5.11. Plugin

```
str          pluginId       reverse-DNS, 1..128 bytes
str          eventName      plugin-defined
<hash-body>  fields         §4.2.1
```

A plugin's own event. It is an event kind rather than a record type because it
*is* an event: it has a moment, it belongs on a timeline, and replay walks it in
order with every other one.

`pluginId` is reverse-DNS — `org.sweeppp.bandplan` — which is what keeps two
plugins apart with no registry to consult. Readers MUST NOT parse it beyond
comparing it for equality, and MUST tolerate one that is not reverse-DNS at all.

`fields` carries whatever the plugin wants to say, in the same typed encoding as
the manifest, so a reader that knows nothing about the plugin can still print
its contents.

### 4.6. Tile

```
u32    segmentId
u32    lod                 level of detail, §8
u32    timeBlock           row block index within (segment, lod)
u32    freqBlock           column block index within the segment grid
u32    lines               rows actually present, 1 .. 256
u32    bins                columns actually present, 1 .. 1024
f32    originDb            dB value stored byte 1 sits half a step above, §7.1
u64    firstLineNs         timestamp of row 0
u64    lastLineNs          timestamp of row (lines - 1)
u8[lines * bins]  data     row-major quantised levels
```

The header is 44 bytes; the payload is `44 + lines * bins` bytes. A reader MUST
verify that `payloadBytes` is at least `44 + lines * bins` before reading `data`
(§12.2).

Only the rows actually filled are stored. A partial tile at the end of a segment
MUST NOT be padded to 256 rows — that would append a block of fabricated silence
to the recording.

`data` is row-major: row *r*, column *c* is at byte offset `r * bins + c`. This
is byte-identical to the waterfall texture format the reference renderer uploads,
so drawing a tile is a `memcpy` with no conversion path to get wrong.

`firstLineNs` and `lastLineNs` are carried so that a time query does not have to
reconstruct timestamps by line arithmetic. A tile with `lines == 1` has
`firstLineNs == lastLineNs`.

### 4.7. Index

```
u32    segmentCount
  segmentCount x {
    u32  id
    u64  lineCount
    u64  startMonotonicNs
    u64  endMonotonicNs
  }
u32    tileCount
  tileCount x {
    u32  segmentId
    u32  lod
    u32  timeBlock
    u32  freqBlock
    u64  offset          file offset of the tile's *record header*
    u32  length          RecordHeader::kBytes + payloadBytes
    u64  firstLineNs
    u64  lastLineNs
  }
```

The index is an optimisation, never the authority. Everything in it is
derivable by scanning, which is precisely why a damaged index costs nothing
(§9.3).

`offset` points at the tile's 12-byte record header, not at its payload.
`length` covers the header and the payload together, so `offset + length` is the
first byte after the tile.

The segment table MAY describe segments for which no `SegmentOpen` record exists
in the file; readers MUST ignore such entries rather than treating them as
corruption. (An extraction writes a segment table sized to all of the *source's*
segments — see §10.4.)

### 4.8. EndOfStream

Empty payload; `payloadBytes` is 0 and `checksum` is the CRC of zero bytes,
which is `0x00000000`.

Its presence means the writer finished deliberately. Its absence means nothing
on its own, since `indexOffset` is the authoritative signal of a clean close.

### 4.9. Telemetry

```
<hash-body>            the sender's counters, §4.2.1
```

Carried only on a live stream (Appendix C), where it reports the far end's
state: samples delivered and dropped, transforms computed, the link's own
counters. A conforming v1 file writer MUST NOT emit it. A file reader MUST skip
it, as it would any unrecognised record.

The keys are the producer's own, nested as hashes by subject. A receiver MUST
ignore keys it does not recognise and MUST tolerate a key of an unexpected
type, exactly as for the manifest (§4.2).

### 4.10. PluginData

```
str    pluginId          reverse-DNS, 1..128 bytes
str    recordName        plugin-defined; names what this record is
u32    schemaVersion     the plugin's own versioning of its body
u64    monotonicNs       0 when the record is not tied to a moment
u32    bodyBytes
u8[bodyBytes]  body      opaque to the container
```

A producer's own record. The container does not interpret `body` and imposes no
structure on it; a reader matches on `pluginId` and `recordName` and skips what
it does not recognise.

One record type serves every producer. `pluginId` already names an arbitrary
one, so an application with no plugins uses its own reverse-DNS identifier rather
than a second, non-plugin "custom data" type — one mechanism, not two.

`schemaVersion` is the plugin's business. The container never reads it, and the
version rules in §11 say nothing about it.

`bodyBytes` is explicit rather than "the rest of the payload". That is what keeps
§11.5's guarantee true here too: a future minor version can append a field after
`body`, and a reader that stops at `bodyBytes` walks past it correctly, where a
reader that consumed the remainder would swallow it as data.

A reader MUST validate `bodyBytes` against the bytes remaining in the payload
before allocating (§12.2).

`monotonicNs` of 0 means the record is not tied to a moment. An extraction (§10)
keeps every such record whatever range was asked for, because no time range can
exclude a record that has no time.

---

## 5. Segments and the frequency grid

### 5.1. What a segment is

A segment is one acquisition configuration's worth of session: a frequency grid
plus the full configuration that produced it, valid from its `SegmentOpen` until
its `SegmentClose` or the end of the file.

Segments are what make a mid-session parameter change harmless. A change closes
the current segment and opens a new one; it never mutates or reinterprets what
is already written. Tiles recorded under any past configuration therefore stay
readable at their original resolution and extent forever. A single global grid
would make every old tile uninterpretable the moment an operator changed span.

### 5.2. The stored grid

```
binCount   stored bins per line
startHz    absolute frequency of the first stored bin
binWidthHz spacing between stored bins
stopHz  =  startHz + binWidthHz * binCount
```

The stored grid is generally *coarser* than the acquisition: a writer decimates
each incoming frame onto it. The reference writer resamples by taking the
**maximum** within each destination bin, never the mean — a narrow carrier
falling between stored bins must survive, and averaging is exactly what would
dilute it into the noise floor.

Nothing in the format requires that particular resampling; it is a writer's
choice. A reader cannot tell, and MUST NOT try.

### 5.3. When a new segment is opened

A writer MUST open a new segment when the stored grid changes — that is, when
`startHz`, `binWidthHz` or `binCount` would differ from the open segment's.

The reference writer additionally opens one when the acquisition configuration's
*grid-affecting* fields change: `centerHz`, `spanHz`, `sampleRate`, `fftSize`,
`window`, `overlap` or `deviceId`. Gain and reference level are deliberately
excluded: they shift the noise floor but leave the frequency grid intact, so
they are recorded as events rather than forcing a new segment.

### 5.4. Segment identifiers

`id` values in a file written from a live session start at 0 and increase by one
per segment. Nothing in this specification requires that, and §10 produces files
where it does not hold.

**Readers MUST index segments by `id`.** A reader that uses a segment's position
in the file as its identity will mis-attribute grids in any extracted file whose
range excluded the first segment, and will do so silently — every tile would be
drawn at another segment's frequencies.

---

## 6. Tiles

### 6.1. Geometry

```
kTileBins  = 1024      columns per tile
kTileLines = 256       rows per tile
```

These are fixed for version 1. A v1 reader MUST use them and MUST NOT take them
from the manifest (§4.2).

Tiles are blocked in frequency as well as time. This is what makes "extract the
last thirty minutes of that band" a tile copy rather than a re-encode: full-width
lines would force reading the entire span to extract one narrow band, and would
be the wrong shape for a remote or web tile endpoint too.

### 6.2. Addressing

A tile is identified by the 4-tuple `(segmentId, lod, timeBlock, freqBlock)`.
Within a file this tuple is unique.

```
freqBlock covers stored bins [freqBlock * 1024, freqBlock * 1024 + bins)
tile startHz  = segment.startHz + segment.binWidthHz * freqBlock * 1024
tile stopHz   = tileStartHz + segment.binWidthHz * bins
timeBlock covers LOD-relative line indices
                 [timeBlock * 256, timeBlock * 256 + lines)
```

The number of frequency blocks in a segment is
`ceil(binCount / 1024)`, and the last block's `bins` is
`binCount - freqBlock * 1024`, which may be less than 1024.

A tile's frequency extent is derived from **its segment's grid**, not from
anything in the tile itself. This is the second place where indexing segments by
position rather than by id produces silently wrong output.

### 6.3. Ordering

Records need not be ordered. When a writer emits several tiles at once — at a
segment close, for instance — the reference implementation emits them in
ascending `(segmentId, lod, timeBlock, freqBlock)` order. Readers MUST NOT
depend on this.

---

## 7. Quantisation

### 7.1. The scale

```
kDbPerStep   = 0.5
kQuantSpanDb = 255 * 0.5 = 127.5
```

A stored byte is an offset above the tile's own `originDb`:

```
dequantise(0,     originDb) = unmeasured                (§7.2)
dequantise(value, originDb) = originDb + value * 0.5    for value >= 1
```

`uint8` at 0.5 dB per step spans 127.5 dB, far beyond the ~50–70 dB of usable
dynamic range these radios have — so quantisation is never the limiting factor.
The decisive property is that the stored bytes are *byte-identical to a
waterfall texture*, so uploading a tile to a GPU and exporting it are both a
`memcpy`.

### 7.2. Byte 0 is coverage, not a level

**Byte 0 means no measurement was ever made in that bin**, and readers MUST NOT
render or reduce it as a level. Measured levels occupy `1 .. 255`.

Coverage and level are different facts and the format does not let them share a
spelling. A sweep of two disjoint spans never looks at the frequencies between
them; if those bins were stored at the bottom of the scale, no reader could tell
them from spectrum that was looked at and found quiet. They would paint as the
darkest colour of a colormap rather than as background, and any reducer that
averaged or interpolated across the gap would fabricate a signal joining the two
sides of it.

The float form of the same fact is `-200 dB`, and a level is treated as a
reading when it is above `-190 dB` — a floor rather than an equality, because a
resample or an average that touches one unmeasured bin lands near the sentinel
rather than on it.

A writer MUST store as byte 0 any bin that carries no measurement: an
unmeasured level, a non-finite value, or a destination bin no source bin
reached. It MUST NOT fill such a bin from a neighbour.

### 7.3. Quantising

```
if db is not a reading (not > -190, or non-finite)  -> 0
steps = (db - originDb) / 0.5
if steps <= 1     -> 1
if steps >= 255   -> 255
otherwise         -> (uint8) (steps + 0.5)
```

The `+ 0.5` rounds to nearest. Truncation would bias every stored level low by
up to a quarter of a dB, which accumulates visibly in a max-hold trace.

Out-of-range values **saturate**; they MUST NOT wrap. A measured value below the
window must clamp to 1, never to 0 and never alias to the top of the scale.

### 7.4. Choosing the origin

`originDb` is per tile rather than per file, so a quiet band and a loud one each
get the full range of the scale.

A writer computes it after the tile's true range is known. Deriving it from the
first line instead would mean a burst arriving later saturates at the top of the
scale and is recorded 40 dB low.

The reference writer's rule, over the *measured* values in the filled region —
an unmeasured bin is not a level and must not pull the origin down:

```
peak  = max(measured values), or 0.0 if there are none
floor = min(measured values), or 0.0 if there are none
span  = peak - floor

if span <= 119.5:  originDb = peak + 8.0 - 127.5     (computed in double)
else:              originDb = peak - 127.5 + 8.0     (computed in float)
```

Anchoring 8 dB above the peak leaves headroom without wasting the scale. When
the range exceeds what 255 steps can express, anchoring on the peak is the right
sacrifice: losing the top of the scale matters far more than losing the bottom
of the noise floor.

> **The two branches are not redundant, and readers MUST NOT assume either.**
> They compute the same quantity in different precisions and can differ in the
> last bit of the stored `f32`. `originDb` is what the file records; how a writer
> arrived at it is not observable and not specified. A writer MAY use any rule.

Non-finite values in the source (a bin with no data) are not measurements and
MUST be stored as byte 0, per §7.2.

---

## 8. The LOD pyramid

### 8.1. Levels

```
kLodLevels = 3
decimation(level) = 8^level   ->  1, 8, 64
```

Level 0 stores every line. Level *n*+1 stores one line per eight lines of level
*n*. Without a pyramid, drawing three hours of waterfall means reading three
hours of lines. The storage overhead is 1/8 + 1/64 ≈ 14%.

### 8.2. Decimation is max-hold

Each level *n*+1 line is the **element-wise maximum** of the eight level-*n*
lines that fed it. Its timestamp is that of the last contributing line.

Max-hold, never mean. A 200 ms burst must still be visible at the /64 level;
averaging is precisely what would erase it, and finding transients is the whole
reason scrollback exists.

Nothing in the format lets a reader verify this, and nothing requires it — but a
writer that averages produces a pyramid whose coarse levels contradict its fine
ones, and this specification states max-hold so that readers can rely on a
coarse level being an upper bound on the level below it.

### 8.3. Line indices

Each `(segment, lod)` pair has its own line counter, starting at 0 at the
segment's `SegmentOpen`. `timeBlock = lineIndex / 256`.

Level *n*+1's line indices are therefore not level *n*'s divided by eight in any
way a reader should compute: at a segment close, a partially accumulated coarse
line is flushed with however many source lines it happened to receive. Readers
MUST use each tile's `firstLineNs`/`lastLineNs` for time, never line arithmetic
across levels.

### 8.4. Choosing a level

A reader serving a query SHOULD pick the coarsest level whose line spacing still
fills the requested budget:

```
lineIntervalNs = segmentSpanNs / segment.lineCount
linesAtNative  = requestedRangeNs / lineIntervalNs
ideal          = the smallest lod with linesAtNative / decimation(lod) <= maxLines
```

**A reader MUST NOT return a level for which no tile exists.** Short sessions
never fill a coarse tile, and a session truncated by power loss loses the coarse
levels first, because they are flushed last. Returning an empty level would
answer a perfectly good query with nothing. The reference reader falls back
downward from `ideal`, then upward.

---

## 9. Integrity and recovery

### 9.1. Checksum

`checksum` is **CRC-32/ISO-HDLC**, also known as CRC-32/IEEE 802.3 — the
polynomial `0xEDB88320` in reversed form, initial value `0xFFFFFFFF`, reflected
in and out, final XOR `0xFFFFFFFF`. It covers the payload only, never the record
header.

It is computed over zero bytes for an empty payload, which yields `0x00000000`.

The checksum's job is not to detect bit rot on a healthy disk. It is to
distinguish a genuinely complete record from a partially written one whose
length field happens to be plausible — which is exactly what a power-loss
truncation leaves behind.

### 9.2. Recovery by scan

A reader MUST be able to reconstruct everything except the index by walking
records from offset 32:

```
1. If fewer than 12 bytes remain, stop.
2. Read the record header.
3. If payloadOffset + payloadBytes > fileSize, stop: the file is truncated
   here. Everything before this point is intact.
4. If CRC(payload) != checksum, stop: the record is damaged. Everything
   before this point is intact.
5. Interpret the record if its type is known; skip it otherwise.
6. Advance to payloadOffset + payloadBytes and repeat.
```

Stopping at the first failure, rather than resynchronising forward, is
deliberate: a record boundary cannot be located reliably in damaged data, and
guessing produces plausible garbage rather than an honest short read.

A reader SHOULD report how many bytes it discarded, and MUST surface that the
file was recovered rather than read cleanly. An operator should know a session
ended abruptly.

When a segment has no `SegmentClose`, its line count MAY be inferred from the
LOD-0 tiles that survived:

```
lineCount = max over LOD-0 tiles of (timeBlock * 256 + lines)
```

### 9.3. Using the index

```
1. If indexOffset is 0 or >= fileSize, recover by scan (§9.2).
2. Otherwise read the record at indexOffset. It MUST be a well-formed,
   checksum-valid record of type Index; if it is not, recover by scan.
3. Every index entry MUST satisfy offset + length <= fileSize; if any does
   not, the index is unusable — recover by scan.
```

A damaged index is not a damaged file. Tiles are self-describing, so falling
back to a scan recovers everything that was written. A reader MUST NOT refuse a
file because its index is bad.

### 9.4. Verifying a tile reached through the index

When a tile is reached through the index, the scan's checks were never applied
to it. A reader MUST therefore, before trusting a byte of the tile:

1. bounds-check `offset` and `offset + length` against the file size;
2. confirm the record's `type` is `Tile`;
3. verify the payload checksum.

Without this, a corrupt tile is handed back as data and drawn as if it were a
measurement. A reader SHOULD skip a tile that fails and continue with the rest
of the query rather than failing the whole query.

---

## 10. Extraction

Extraction copies a time and frequency range into a new, standalone `.sweeps`.

### 10.1. Tiles are copied, not re-encoded

An extracted tile's payload MUST be a byte-for-byte copy of the source payload.
No decode, no re-quantisation: the output tiles are bit-identical to the input
ones. That is what blocking tiles in frequency as well as time buys.

### 10.2. Granularity

The unit of extraction is the tile. A range that clips a tile keeps the **whole**
tile, so an extract generally covers slightly more than was asked for. An
implementation SHOULD say so, rather than let an operator read the output as an
exact cut.

### 10.3. What is preserved

- `SegmentOpen` records are re-encoded verbatim for every segment overlapping
  the range, **keeping their original ids** (§5.4).
- `SegmentClose` records carry the extract's own line count, not the source's —
  otherwise a thirty-second cut from a three-hour session would claim to be
  three hours long.
- **Event payloads within the time range are copied byte for byte**, the way tile
  payloads are, and MUST NOT be re-encoded field by field. A re-encode drops
  whatever the extracting build does not understand — an event kind added by a
  newer minor version, a field appended to a body it does know — and an
  extraction is precisely where such content must survive, because the extract
  may outlive the build that cut it. Copying also preserves the decoded
  `segmentId` rather than restamping it.
- `PluginData` records whose `monotonicNs` falls in the range are copied byte for
  byte, as is **every** `PluginData` record with a `monotonicNs` of 0 (§4.10).
- The manifest is regenerated, not copied (Appendix B.3).

### 10.4. The index of an extract

The extracted file's index segment table MAY be sized to all of the source's
segments, including ones no `SegmentOpen` record describes. Readers MUST
tolerate this (§4.7). It is harmless — such an entry updates nothing — and
requiring writers to filter it would be a compatibility break for no gain.

---

## 11. Versioning and compatibility

This section is the one the rest of the document turns on. Everything else
describes the format as it is; this is what lets it change without breaking.

### 11.1. The version triple

A file carries three independent version quantities in its header:

| Field                  | Offset | Meaning                                                                                                  |
|------------------------|--------|----------------------------------------------------------------------------------------------------------|
| `majorVersion`         | 4      | Structural generation. A reader that does not know it cannot read the file at all.                       |
| `minorVersion`         | 24     | Additive revision within a major. A reader that does not know it can still read everything it does know. |
| `incompatibleFeatures` | 28     | Bitmask of features a reader MUST understand to read the file correctly.                                 |

Version 1.0 defines **no** feature bits: `KNOWN_FEATURES` is `0`.

Splitting "what changed" from "can I still read it" is the point. A single
version number is all-or-nothing: any additive change — one new record type, one
new manifest key — must bump it, and bumping it makes every existing reader
refuse the file. The bitmask exists for the middle case: a change that is
structurally additive but that a naive reader would *misread* rather than skip.

### 11.2. Reader decision procedure

A reader MUST perform these steps, in order, before interpreting any record.

```
1. magic != "SWPP"
       -> reject: not a .sweeps file

2. majorVersion > MAX_SUPPORTED_MAJOR
       -> reject: written by a newer major version.
          The message MUST name both versions.

3. incompatibleFeatures & ~KNOWN_FEATURES  != 0
       -> reject: unknown required features.
          The message MUST name the unrecognised bits.

4. minorVersion > MAX_KNOWN_MINOR
       -> proceed. The reader MAY warn that additive content will be
          skipped. It MUST NOT reject.

5. otherwise
       -> proceed with a full read.
```

Step 4 is the whole reason for the split. A reader built against 1.0 opens a 1.7
file and reads every record it understands, skipping the rest.

### 11.3. Feature bits

A feature bit is set when a file contains something structurally additive that a
reader unaware of it would misinterpret rather than skip — a compressed tile
payload, say, or a second quantisation scale. Such a change also bumps the minor
version.

Setting a bit is a strong statement: it makes the file unreadable to every
reader that does not know the bit, which is exactly the intent when reading it
wrong would be worse than not reading it.

`incompatibleFeatures` is scoped to a major version. Bit *n* under major 1 and
bit *n* under major 2 are unrelated.

### 11.4. Writer rules

| Change                                                             | Required bump           |
|--------------------------------------------------------------------|-------------------------|
| A new record type                                                  | minor                   |
| A new event kind                                                   | minor                   |
| A new manifest key                                                 | minor                   |
| A new metadata value type (§4.2.1)                                 | **major**               |
| **Appending** a field to an existing record payload                | minor                   |
| **Appending** a field to an existing event body                    | minor                   |
| A structurally additive change a naive reader would misread        | **feature bit** + minor |
| Changing the meaning, order or width of an existing field          | **major**               |
| Removing a record type                                             | **major**               |
| Altering tile geometry, the LOD schedule or the quantisation scale | **major**               |

A writer MUST NOT set a feature bit it does not need. A writer MUST write
`minorVersion` as the highest minor whose features it actually used, not as the
highest it knows.

### 11.5. The two extensibility guarantees

Minor bumps are only safe because of two properties.

**Unknown record types MUST be skipped.** Every record carries `payloadBytes`,
so a reader can always find the next one. A reader MUST NOT abort, and MUST NOT
treat an unknown type as corruption. The same applies to an unrecognised event
kind (§4.5.1) — with the stronger requirement that the event be *kept*, since a
reader that skipped it would silently delete it from any extraction it wrote.

**Readers MUST NOT require a record payload to be fully consumed.** A decoder
reads the fields it knows and ignores any tail. This is what makes appending a
field to `SegmentOpen`, or to an event body, invisible to an older reader.

Conversely, a writer MUST NOT add a field anywhere but the end of a payload, and
MUST NOT reuse a field's meaning.

Neither guarantee reaches inside a metadata value (§4.2.1): a value carries no
length, so an unknown type tag cannot be skipped. That is why a new value type is
a major bump in the table above, and it is the one place in this format where
extension is not free.

### 11.6. A limitation, stated plainly

The feature bitmask protects only readers that implement it. A reader that
ignores bytes 24–31 accepts a file with an incompatible feature bit set and
misreads it, and no bit this specification defines can stop it.

The only lever against such a reader is the major version. This is why the
scheme is defined before third-party readers exist rather than after: it works
from the first reader onward, and cannot be retrofitted into one already written.

### 11.7. Private and experimental use

`RecordType` values `0xFF00`–`0xFFFF` and event kinds `0xFF00`–`0xFFFF` are
reserved for private and experimental use. They will never be assigned by this
specification, and a conforming writer MUST NOT emit them in a file intended for
interchange.

Two producers using this range may mean entirely different things by the same
number; a reader MUST NOT attribute meaning to one it did not itself write.

The range exists so that experimenting with a new record or event costs nothing
and tempts nobody into squatting on a low, assignable number that a later
version of this document will hand to somebody else. A producer that wants an
identifier it can share instead of one it must keep to itself uses
`PluginData` (§4.10) or the `Plugin` event kind (§4.5.11), where `pluginId`
makes the namespace its own.

---

## 12. Security considerations

A `.sweeps` file is untrusted input. It arrives from a filesystem, a download or
a network peer, and a reader that maps it and indexes into it is operating
directly on attacker-controllable bytes.

### 12.1. Memory mapping

A reader that maps the file MUST treat the mapping as bounded by the size it
measured, and MUST bounds-check every offset against that size. On platforms
where a mapping can outlive the file's length — if the file is truncated while
mapped — touching a page past the new end raises a signal rather than returning
an error. A reader that must tolerate concurrent truncation SHOULD read rather
than map.

Mapping a file that another process is still writing is explicitly supported
(it is how a running session is inspected), which is why every read is
bounds-checked against the size captured at open rather than against a length
believed from the header.

### 12.2. Every length field is an allocation

`str`, `payloadBytes`, `gainCount`, `tileCount`, `segmentCount`, `bodyBytes`,
the metadata `count` fields (§12.5) and `lines * bins` are all
attacker-controlled sizes that a naive reader turns straight into an allocation.

**A reader MUST validate a length against the bytes actually remaining before
allocating.** A `str` claiming 4 GB inside a 4-byte buffer MUST be rejected as
corrupt, not attempted. The same applies to a tile whose `lines * bins` exceeds
its own payload, and to a count prefix larger than the remaining payload could
possibly hold.

`lines * bins` MUST be computed in a width that cannot overflow: both are `u32`,
and their product does not fit in 32 bits.

### 12.3. Arithmetic

`offset + length`, `payloadOffset + payloadBytes` and `timeBlock * 256 + lines`
MUST be computed so that they cannot wrap. Promoting to `u64` before adding is
sufficient for all of them.

### 12.4. Resource exhaustion

A file may declare an arbitrary number of records, segments, events and tiles.
A reader that accumulates all events or all index entries in memory has an
unbounded allocation driven by file content. Bounding it, or bounding the file
size accepted, is the reader's responsibility; this specification imposes no
limit because legitimate multi-hour sessions genuinely contain millions of tiles.

### 12.5. Metadata counts MUST be validated before they are reserved

A hash's `count` and an array's `count` (§4.2.1) each precede the entries they
describe, so a reader that reserves before it validates is one four-byte field
away from a four-billion-element allocation in a twenty-byte payload.

**A reader MUST reject a count that the remaining bytes could not possibly
hold**, before reserving anything:

```
minimum body bytes, by type:
  String 4    Int 8    Float 8    Bool 1
  Bytes  4    Hash 4   Array 5

array:   reject if count > remaining / minimumBodyBytes(elementType)
hash:    reject if count > remaining / 6
```

Six is the smallest a hash entry can encode to: a `u32` key length of zero, no
key bytes, a one-byte type tag, and the one-byte body of a `Bool`.

The bound MUST be computed by **division**, not by multiplying the count and
comparing — `count * 6` is exactly the overflow a hostile count is written to
cause.

It must also be the *true* minimum. A larger figure chosen to feel safe rejects
legitimate files: a hash of a hundred booleans encodes to six hundred bytes, and
any per-entry estimate above six refuses it.

### 12.6. Metadata nesting depth MUST be capped

A `Hash` value contains a hash, and an `Array` may have `Hash` or `Array` as its
element type, so decoding is recursive on attacker-controlled input. An empty
hash encodes to four bytes and a nesting level costs nine, which puts a nesting
depth of tens of thousands inside a payload of a few hundred kilobytes — and a
stack overflow is not something a `Result` can report.

**A reader MUST cap nesting depth and refuse anything deeper**, rather than
descending and hoping. The reference implementation caps it at 32, which is far
above anything a writer produces and far below anything that threatens a stack.

The cap is a reader's limit, not a writer's: this specification does not fix the
number, and a reader MAY choose a different one. A writer SHOULD stay well
inside 32 so that its files are readable by any conforming implementation.

### 12.7. What the format does not provide

There is no authentication, no signature and no encryption. A checksum detects
accidental damage, not tampering: an attacker who can modify the bytes can
recompute the CRC. A file whose provenance matters MUST be protected by
something outside this format.

The same holds for a live stream (Appendix C). Its peer is as untrusted as a
downloaded file, every rule above applies to what it sends, and anything that
must be authenticated or kept private is the business of the protocol carrying
the stream.

---

## 13. IANA considerations

This section proposes registrations; it does not claim they have been made.

### 13.1. Media type

```
Type name:               application
Subtype name:            vnd.sweeps
Required parameters:     none
Optional parameters:     none
Encoding considerations: binary
Security considerations: See §12 of this document.
Interoperability considerations:
    The first four bytes are the ASCII string "SWPP". Bytes 4-7 carry a
    little-endian major version; a reader that does not recognise the major
    version must refuse the file rather than attempt to read it. See §11.
Published specification:  This document.
Applications that use this media type:
    Spectrum monitoring and SDR recording software.
Fragment identifier considerations: none
Additional information:
    Magic number(s):      53 57 50 50  ("SWPP") at offset 0
    File extension(s):    .sweeps
    Macintosh file type code(s): none
Person & email address to contact for further information:
    See the reference implementation's repository.
Intended usage:          COMMON
Restrictions on usage:   none
Author/Change controller: The reference implementation's maintainers.
```

### 13.2. Magic number

The four-byte sequence `53 57 50 50` at offset 0, followed by a little-endian
`u32` version at offset 4, is sufficient to identify the format for
content-sniffing purposes such as `file(1)`:

```
0    string    SWPP    Sweep++ session container
>4   lelong    x       \b, version %d
```

---

## Appendix A: Test vectors

### A.1. CRC-32

The check value for the standard input `"123456789"` (nine ASCII bytes) is:

```
0xCBF43926
```

A CRC over zero bytes is `0x00000000`.

### A.2. Scalar encoding

| Value                | Type  | Bytes (hex, in file order)   |
|----------------------|-------|------------------------------|
| `0xBEEF`             | `u16` | `EF BE`                      |
| `0xDEADBEEF`         | `u32` | `EF BE AD DE`                |
| `0x0123456789ABCDEF` | `u64` | `EF CD AB 89 67 45 23 01`    |
| `-1`                 | `i64` | `FF FF FF FF FF FF FF FF`    |
| `INT64_MIN`          | `i64` | `00 00 00 00 00 00 00 80`    |
| `INT64_MAX`          | `i64` | `FF FF FF FF FF FF FF 7F`    |
| `3.5f`               | `f32` | `00 00 60 40`                |
| `-2.718281828459045` | `f64` | `69 57 14 8B 0A BF 05 C0`    |
| `"hello"`            | `str` | `05 00 00 00 68 65 6C 6C 6F` |
| `""`                 | `str` | `00 00 00 00`                |

### A.3. Quantisation

With `originDb = -140.0`:

| dB in   | Stored byte | dB out                                      |
|---------|-------------|---------------------------------------------|
| −140.0  | 1           | −139.5 (clamped to the bottom of the scale) |
| −139.75 | 1           | −139.5                                      |
| −139.5  | 1           | −139.5                                      |
| −100.0  | 80          | −100.0                                      |
| −12.5   | 255         | −12.5                                       |
| −160.0  | 1           | −139.5 (saturated)                          |
| +1000.0 | 255         | −12.5 (saturated)                           |
| −200.0  | 0           | unmeasured                                  |
| −inf    | 0           | unmeasured                                  |
| NaN     | 0           | unmeasured                                  |

Every value in the representable window `[originDb + 0.5, originDb + 127.5]`
round-trips to within `kDbPerStep / 2` = 0.25 dB.

### A.4. A minimal file

The smallest structurally valid file is a 32-byte header followed by a manifest
record. With `createdWallNs = 0`, an empty manifest, and no index:

```
offset  bytes
------  ----------------------------------------------------------------
000000  53 57 50 50  01 00 00 00                 magic, majorVersion = 1
000008  00 00 00 00 00 00 00 00                  indexOffset = 0
000010  00 00 00 00 00 00 00 00                  createdWallNs = 0
000018  00 00 00 00                              minorVersion = 0
00001C  00 00 00 00                              incompatibleFeatures = 0
000020  01 00  00 00                             type = Manifest, flags = 0
000024  04 00 00 00                              payloadBytes = 4
000028  1C DF 44 21                              CRC32 of "00 00 00 00"
00002C  00 00 00 00                              hash: count = 0
```

Such a file opens, reports zero segments and zero tiles, and is flagged as
recovered by scan because `indexOffset` is zero.

### A.5. The golden file

`tests/data/v1-golden.sweeps` is a byte-frozen v1.0 file
written with a fixed `createdWallNs` and a fixed application version string. It
covers two segments with different grids, a segment wide enough to need two
frequency blocks with the second one partial, enough lines for the pyramid to
reach LOD 2, events, an index and an end-of-stream record.

The conformance suite asserts that the writer reproduces it **byte for byte**.
That is what says a change to the writer, the manifest encoder, the quantiser or
the tile ordering has not silently moved the format — a version number cannot say
it, because the whole failure mode is a change nobody thought to bump a version
for.

It is also the input for the compatibility tests in §11: patching its header
produces the major-bump, feature-bit and minor-bump cases without hand-authoring
a file for each.

### A.6. Typed metadata

A manifest containing one key of each scalar type, in the order a writer emits
them:

```
04 00 00 00                          count = 4

03 00 00 00 66 6C 67                 key "flg"
04                                   type Bool
01                                   true

03 00 00 00 6E 65 67                 key "neg"
02                                   type Int
FF FF FF FF FF FF FF FF              -1, two's complement

03 00 00 00 72 61 77                 key "raw"
05                                   type Bytes
02 00 00 00 DE AD                     2 bytes

03 00 00 00 73 74 72                 key "str"
01                                   type String
02 00 00 00 68 69                    "hi"
```

Keys are `flg`, `neg`, `raw`, `str` — ascending byte order, whatever order they
were set in.

An empty array of floats, as a value:

```
07                                   type Array
03                                   elementType Float
00 00 00 00                          count = 0
```

An array of two hashes, each with one key:

```
07                                   type Array
06                                   elementType Hash
02 00 00 00                          count = 2
01 00 00 00                          [0]: count = 1
01 00 00 00 61 01 00 00 00 00          key "a", String, ""
00 00 00 00                          [1]: count = 0
```

The elements carry no type tag of their own; `elementType` is stated once.

---

## Appendix B: Manifest profile

### B.1. The general rule

The manifest is a typed key/value object (§4.2.1) encoded directly as the
`Manifest` record's payload. **Readers MUST tolerate unknown keys, and MUST
tolerate a known key carrying a type other than the one listed below** — falling
back to a default rather than refusing the file. The manifest is metadata; a
session that cannot be named is still a session that can be read.

Writers emit the keys below. The list is not closed: adding one is a minor
version bump (§11.4).

### B.2. Keys written by a live session

| Key              | Type   | Meaning                                                                  |
|------------------|--------|--------------------------------------------------------------------------|
| `name`           | String | Operator-supplied session name, or `session-YYYYMMDD-hhmmss`             |
| `created`        | String | `createdWallNs` as RFC 3339 UTC, e.g. `2026-08-11T20:56:53Z`             |
| `app_version`    | String | The writing application's version                                        |
| `format_version` | Int    | The container major version (informational; the header is authoritative) |
| `bins_per_line`  | Int    | The writer's configured stored bin count                                 |
| `tile_bins`      | Int    | 1024 — informational (§4.2)                                              |
| `tile_lines`     | Int    | 256 — informational                                                      |
| `lod_levels`     | Int    | 3 — informational                                                        |
| `db_per_step`    | Float  | 0.5 — informational                                                      |
| `notes`          | String | Operator-supplied free text; omitted when empty                          |

`created` is a **string**, not an epoch integer, and deliberately so: running
`strings` on a session file and seeing when it was recorded is a design goal that
an integer would quietly cost. `createdWallNs` in the file header (§3.1) is the
machine-readable form, and it is the authoritative one.

`db_per_step` is a `Float` even though its value is exactly representable as an
integer. The type is part of the key's contract, not a consequence of the value
that happened to be written.

### B.3. Keys written by an extraction

| Key               | Type   | Meaning                                 |
|-------------------|--------|-----------------------------------------|
| `name`            | String | `<source name>-extract`                 |
| `created`         | String | RFC 3339 UTC, at the time of extraction |
| `app_version`     | String | The extracting application's version    |
| `format_version`  | Int    | The container major version             |
| `extracted_from`  | String | The source file's name                  |
| `extract_from_ns` | Int    | Requested range start, monotonic ns     |
| `extract_to_ns`   | Int    | Requested range end, monotonic ns       |
| `extract_from_hz` | Float  | Requested range start                   |
| `extract_to_hz`   | Float  | Requested range end                     |
| `tile_bins`       | Int    | 1024                                    |
| `tile_lines`      | Int    | 256                                     |
| `db_per_step`     | Float  | 0.5                                     |

An extraction does not write `lod_levels`, `bins_per_line` or `notes`.

`extract_to_ns` is an `Int`, and an unbounded range is stored as the two's
complement of `UINT64_MAX`, which reads back as `-1`. Readers interpreting these
two keys as a time range SHOULD treat a negative value as "unbounded" rather than
as an instant before the session began.

### B.4. A worked example

The manifest of a session named `cli-record`, rendered as JSON:

```json
{
  "app_version": "0.1.0",
  "bins_per_line": 2048,
  "created": "2026-08-11T20:56:53Z",
  "db_per_step": 0.5,
  "format_version": 1,
  "lod_levels": 3,
  "name": "cli-record",
  "tile_bins": 1024,
  "tile_lines": 256
}
```

and as bytes, in the order they appear in the record payload:

```
09 00 00 00                    count = 9 entries

0B 00 00 00 61 70 70 5F ...    key "app_version"
01                             type String
05 00 00 00 30 2E 31 2E 30     "0.1.0"

0D 00 00 00 62 69 6E 73 ...    key "bins_per_line"
02                             type Int
00 08 00 00 00 00 00 00        2048

...                            and so on, in ascending key order
```

Keys appear in ascending byte order — `app_version`, `bins_per_line`, `created`,
`db_per_step`, `format_version`, `lod_levels`, `name`, `tile_bins`, `tile_lines`
— which is the order a writer MUST emit them in (§4.2.1), not merely the order
this example happens to show.

---

## Appendix C: Live streams

### C.1. Scope

A live stream carries the records of §4 over a reliable, ordered byte stream —
a TCP connection, typically — as they are produced, for a receiver to display
rather than to store. This appendix defines how a stream differs from a file.
Whatever an application layers on top of it — a handshake, authentication,
commands — travels in `PluginData` records (§4.10) under that application's
own `pluginId`, and is outside this specification.

### C.2. The stream header

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     magic "SWPP" (4 x u8)                     |   0
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      majorVersion (u32)                       |   4
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      minorVersion (u32)                       |   8
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  incompatibleFeatures (u32)                   |  12
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Each side sends one header before anything else, and checks the other's by the
decision procedure of §11.2: a wrong magic, a newer major version or an unknown
feature bit ends the stream. The file header's `indexOffset` and
`createdWallNs` mean nothing here and are not carried.

### C.3. Records on a stream

Records are framed exactly as in §3.2. Two things are different from a file:

- **A damaged record ends the stream.** A file recovers from a bad record by
  scanning on (§9.2); on a stream nothing after a bad length can be trusted to
  start a record, so a checksum mismatch is fatal rather than skipped.
- **A receiver MUST bound `payloadBytes` before buffering the payload**, and end
  the stream when a record exceeds that bound. 16 MiB is RECOMMENDED: the
  largest legitimate record on a stream is a `SegmentOpen` or a tile, both far
  smaller.

Unrecognised record types are skipped, as in a file. `Index` never appears.
`Manifest` MAY appear and carries what it does in a file.

### C.4. Segments

A `SegmentOpen` declares the grid and the acquisition configuration in force
from that point on, superseding any earlier one. Segment ids are unique within
a stream. Every bin of a newly opened segment is unmeasured until a tile
carries it. A `SegmentClose` says the sender has stopped producing lines for
that segment — acquisition stopped, typically.

### C.5. Tiles as updates

On a stream a tile is the latest value of one frequency block, not a block of
history:

| Field       | On a stream                                                     |
|-------------|-----------------------------------------------------------------|
| `segmentId` | the most recently opened segment                                |
| `lod`       | 0                                                               |
| `timeBlock` | the sender's line number, modulo 2³², shared by one line's tiles |
| `freqBlock` | as §6.2                                                         |
| `lines`     | 1                                                               |
| `bins`      | as §6.2                                                         |

A receiver applies each tile to its copy of the segment's current line; blocks
not sent keep the values they last had. A sender SHOULD send only the blocks
whose values changed, which on a sweep is the part of the span measured since
the previous line. A tile for any segment but the most recently opened one
MUST be ignored. Quantisation is §7's, with an origin per tile.

What marks a line as complete and ready to display is the application's to
define, typically a `PluginData` record after the line's tiles.

### C.6. Events and telemetry

`Event` records (§4.5) carry what they do in a file, timestamped on the
sender's clocks; a receiver that compares them with its own must map between
the two. `Telemetry` (§4.9) is sent periodically by the side producing data.

### C.7. Transport

The stream is the bytes of this appendix, whatever carries them. An
application MAY carry it inside an encrypted, authenticated channel, in which
case the stream header and records are the channel's plaintext and nothing
here changes.

### C.8. Ending a stream

`EndOfStream` (§4.8) says the sender is finished deliberately; the receiver
SHOULD close the connection. A connection that simply ends is the stream's
equivalent of a truncated file, and everything received before it remains
valid.

