// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { ByteReader, ByteWriter, ProtocolError } from "./bytes";

export const RecordType = {
  Manifest: 1,
  SegmentOpen: 2,
  SegmentClose: 3,
  Event: 4,
  Tile: 5,
  Index: 6,
  EndOfStream: 7,
  Telemetry: 8,
  PluginData: 9,
} as const;
export type RecordTypeValue = (typeof RecordType)[keyof typeof RecordType];

export const kRecordHeaderBytes = 12;
export const kStreamHeaderBytes = 16;
const kMagic = [0x53, 0x57, 0x50, 0x50]; // "SWPP"
const kMajorVersion = 1;
const kMinorVersion = 0;
const kKnownFeatures = 0;

// ---- CRC-32 (IEEE 802.3) ----------------------------------------------------

const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; ++i) {
    let value = i;
    for (let bit = 0; bit < 8; ++bit) {
      value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    }
    table[i] = value >>> 0;
  }
  return table;
})();

export function crc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; ++i) {
    crc = (crcTable[(crc ^ data[i]!) & 0xff]! ^ (crc >>> 8)) >>> 0;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// ---- records -------------------------------------------------------------------

export interface StreamRecord {
  type: number;
  payload: Uint8Array;
}

/** Appends a whole record: header, then payload. */
export function appendRecord(out: ByteWriter, type: number, payload: Uint8Array): void {
  out.u16(type).u16(0).u32(payload.length).u32(crc32(payload)).bytes(payload);
}

export function encodeStreamHeader(out: ByteWriter): void {
  out.bytes(new Uint8Array(kMagic)).u32(kMajorVersion).u32(kMinorVersion).u32(0);
}

/** Checks a stream header by the file's own rules: the magic, a major version
 * no newer than this one, and no feature bits this end does not know. */
export function decodeStreamHeader(data: Uint8Array): void {
  if (data.length < kStreamHeaderBytes) {
    throw new ProtocolError("truncated stream header");
  }
  for (let i = 0; i < kMagic.length; ++i) {
    if (data[i] !== kMagic[i]) {
      throw new ProtocolError("not a .sweeps stream (bad magic)");
    }
  }
  const in_ = new ByteReader(data.subarray(4, kStreamHeaderBytes));
  const major = in_.u32();
  in_.u32();
  const features = in_.u32();
  if (major > kMajorVersion) {
    throw new ProtocolError(`the stream is .sweeps major version ${major}, newer than ${kMajorVersion}`);
  }
  if ((features & ~kKnownFeatures) !== 0) {
    throw new ProtocolError("the stream needs .sweeps features this page does not implement");
  }
}

/** Splits bytes into records however they arrive. A checksum mismatch or an
 * oversized record breaks the stream for good, as in C++: nothing after a
 * corrupt length can be trusted to start a record. */
export class RecordFramer {
  static readonly kDefaultMaxPayloadBytes = 16 * 1024 * 1024;

  private buffer = new Uint8Array(0);
  private read = 0;
  private broken: ProtocolError | null = null;

  constructor(private readonly maxPayloadBytes = RecordFramer.kDefaultMaxPayloadBytes) {}

  feed(data: Uint8Array): void {
    if (this.broken || data.length === 0) {
      return;
    }
    const kept = this.buffer.length - this.read;
    const joined = new Uint8Array(kept + data.length);
    joined.set(this.buffer.subarray(this.read));
    joined.set(data, kept);
    this.buffer = joined;
    this.read = 0;
  }

  get buffered(): number {
    return this.buffer.length - this.read;
  }

  /** The next whole record, or null until more bytes arrive. */
  next(): StreamRecord | null {
    if (this.broken) {
      throw this.broken;
    }
    const available = this.buffer.length - this.read;
    if (available < kRecordHeaderBytes) {
      return null;
    }
    const view = new DataView(this.buffer.buffer, this.buffer.byteOffset + this.read, kRecordHeaderBytes);
    const type = view.getUint16(0, true);
    const payloadBytes = view.getUint32(4, true);
    const checksum = view.getUint32(8, true);
    if (payloadBytes > this.maxPayloadBytes) {
      this.broken = new ProtocolError(
        `a record of ${payloadBytes} bytes exceeds the stream's limit of ${this.maxPayloadBytes}`,
      );
      throw this.broken;
    }
    if (available < kRecordHeaderBytes + payloadBytes) {
      return null;
    }
    const start = this.read + kRecordHeaderBytes;
    const payload = this.buffer.slice(start, start + payloadBytes);
    if (crc32(payload) !== checksum) {
      this.broken = new ProtocolError(`checksum mismatch in a record of type ${type}`);
      throw this.broken;
    }
    this.read = start + payloadBytes;
    return { type, payload };
  }
}
