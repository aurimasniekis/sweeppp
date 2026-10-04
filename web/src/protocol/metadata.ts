// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { ByteReader, ByteWriter, ProtocolError, utf8 } from "./bytes";

/** The typed metadata hash of the `.sweeps` format (§4.2), as C++ has it. */

export const ValueType = {
  String: 1,
  Int: 2,
  Float: 3,
  Bool: 4,
  Bytes: 5,
  Hash: 6,
  Array: 7,
} as const;
export type ValueTypeId = (typeof ValueType)[keyof typeof ValueType];

export type Value =
  | { type: typeof ValueType.String; value: string }
  | { type: typeof ValueType.Int; value: bigint }
  | { type: typeof ValueType.Float; value: number }
  | { type: typeof ValueType.Bool; value: boolean }
  | { type: typeof ValueType.Bytes; value: Uint8Array }
  | { type: typeof ValueType.Hash; value: Metadata }
  | { type: typeof ValueType.Array; elementType: ValueTypeId; value: Value[] };

export const value = {
  string: (v: string): Value => ({ type: ValueType.String, value: v }),
  int: (v: number | bigint): Value => ({ type: ValueType.Int, value: BigInt(v) }),
  float: (v: number): Value => ({ type: ValueType.Float, value: v }),
  bool: (v: boolean): Value => ({ type: ValueType.Bool, value: v }),
  bytes: (v: Uint8Array): Value => ({ type: ValueType.Bytes, value: v }),
  hash: (v: Metadata): Value => ({ type: ValueType.Hash, value: v }),
  array: (elementType: ValueTypeId, v: Value[]): Value => ({
    type: ValueType.Array,
    elementType,
    value: v,
  }),
};

/** The nesting a protocol message may use; the protocol itself needs four. */
export const kMaxMetadataDepth = 8;

/** The smallest body each type encodes to, to refuse a count before it is
 * believed. */
function minimumBodyBytes(type: ValueTypeId): number {
  switch (type) {
    case ValueType.Int:
    case ValueType.Float:
      return 8;
    case ValueType.Bool:
      return 1;
    case ValueType.Array:
      return 5;
    default:
      return 4;
  }
}

/** A u32 key length, no key, a type tag and a Bool's one byte. */
const kMinEntryBytes = 6;

function isKnownType(tag: number): tag is ValueTypeId {
  return tag >= ValueType.String && tag <= ValueType.Array;
}

/** Byte order of the UTF-8 encodings, which is std::map's order in C++. */
function compareKeys(a: string, b: string): number {
  const x = utf8(a);
  const y = utf8(b);
  const n = Math.min(x.length, y.length);
  for (let i = 0; i < n; ++i) {
    if (x[i] !== y[i]) {
      return x[i]! - y[i]!;
    }
  }
  return x.length - y.length;
}

export class Metadata {
  readonly entries = new Map<string, Value>();

  static of(fields: Record<string, Value>): Metadata {
    const out = new Metadata();
    for (const [key, v] of Object.entries(fields)) {
      out.set(key, v);
    }
    return out;
  }

  set(key: string, v: Value): this {
    this.entries.set(key, v);
    return this;
  }
  setString(key: string, v: string): this {
    return this.set(key, value.string(v));
  }
  setInt(key: string, v: number | bigint): this {
    return this.set(key, value.int(v));
  }
  setFloat(key: string, v: number): this {
    return this.set(key, value.float(v));
  }
  setBool(key: string, v: boolean): this {
    return this.set(key, value.bool(v));
  }
  setHash(key: string, v: Metadata): this {
    return this.set(key, value.hash(v));
  }

  find(key: string): Value | undefined {
    return this.entries.get(key);
  }
  has(key: string): boolean {
    return this.entries.has(key);
  }
  get size(): number {
    return this.entries.size;
  }

  // A value of another type reads as the fallback, never converted: the same
  // rule as C++, so both ends agree on what a field is.
  getString(key: string, fallback = ""): string {
    const v = this.find(key);
    return v?.type === ValueType.String ? v.value : fallback;
  }
  getBigInt(key: string, fallback = 0n): bigint {
    const v = this.find(key);
    return v?.type === ValueType.Int ? v.value : fallback;
  }
  getInt(key: string, fallback = 0): number {
    const v = this.find(key);
    return v?.type === ValueType.Int ? Number(v.value) : fallback;
  }
  getFloat(key: string, fallback = 0): number {
    const v = this.find(key);
    return v?.type === ValueType.Float ? v.value : fallback;
  }
  getBool(key: string, fallback = false): boolean {
    const v = this.find(key);
    return v?.type === ValueType.Bool ? v.value : fallback;
  }
  getBytes(key: string): Uint8Array | undefined {
    const v = this.find(key);
    return v?.type === ValueType.Bytes ? v.value : undefined;
  }
  /** The nested hash, or an empty one: what C++'s `hashAt` answers. */
  getHash(key: string): Metadata {
    const v = this.find(key);
    return v?.type === ValueType.Hash ? v.value : new Metadata();
  }
  getArray(key: string): Value[] {
    const v = this.find(key);
    return v?.type === ValueType.Array ? v.value : [];
  }
  /** The hashes of an array of hashes. */
  getHashes(key: string): Metadata[] {
    return this.getArray(key).flatMap((v) => (v.type === ValueType.Hash ? [v.value] : []));
  }

  encode(out: ByteWriter): void {
    const keys = [...this.entries.keys()].sort(compareKeys);
    out.u32(keys.length);
    for (const key of keys) {
      out.string(key);
      encodeValue(out, this.entries.get(key)!);
    }
  }

  toBytes(): Uint8Array {
    const out = new ByteWriter();
    this.encode(out);
    return out.finish();
  }

  static decode(in_: ByteReader, depthBudget = kMaxMetadataDepth): Metadata {
    if (depthBudget === 0) {
      throw new ProtocolError(`metadata nested deeper than allowed at offset ${in_.offset}`);
    }
    const count = in_.u32();
    if (count > Math.floor(in_.remaining / kMinEntryBytes)) {
      throw new ProtocolError(
        `metadata claims ${count} entries at offset ${in_.offset}, which cannot fit in ${in_.remaining} bytes`,
      );
    }
    const out = new Metadata();
    for (let i = 0; i < count; ++i) {
      const key = in_.string();
      out.set(key, decodeValue(in_, depthBudget));
    }
    return out;
  }

  static fromBytes(data: Uint8Array, depthBudget = kMaxMetadataDepth): Metadata {
    return Metadata.decode(new ByteReader(data), depthBudget);
  }

  /** As plain JavaScript, for display and debugging: ints as numbers when
   * they fit, bytes as their length. */
  toPlain(): Record<string, unknown> {
    const out: Record<string, unknown> = {};
    for (const [key, v] of this.entries) {
      out[key] = plain(v);
    }
    return out;
  }
}

function plain(v: Value): unknown {
  switch (v.type) {
    case ValueType.Int:
      return Number.isSafeInteger(Number(v.value)) ? Number(v.value) : v.value;
    case ValueType.Bytes:
      return { bytes: v.value.length };
    case ValueType.Hash:
      return v.value.toPlain();
    case ValueType.Array:
      return v.value.map(plain);
    default:
      return v.value;
  }
}

export function encodeValue(out: ByteWriter, v: Value): void {
  out.u8(v.type);
  encodeBody(out, v);
}

function encodeBody(out: ByteWriter, v: Value): void {
  switch (v.type) {
    case ValueType.String:
      out.string(v.value);
      break;
    case ValueType.Int:
      out.i64(v.value);
      break;
    case ValueType.Float:
      out.f64(v.value);
      break;
    case ValueType.Bool:
      out.u8(v.value ? 1 : 0);
      break;
    case ValueType.Bytes:
      out.u32(v.value.length).bytes(v.value);
      break;
    case ValueType.Hash:
      v.value.encode(out);
      break;
    case ValueType.Array:
      out.u8(v.elementType).u32(v.value.length);
      for (const element of v.value) {
        encodeBody(out, element);
      }
      break;
  }
}

function decodeValue(in_: ByteReader, depthBudget: number): Value {
  const tag = in_.u8();
  if (!isKnownType(tag)) {
    throw new ProtocolError(`metadata value type ${tag} is not defined`);
  }
  return decodeBody(in_, tag, depthBudget);
}

function decodeBody(in_: ByteReader, type: ValueTypeId, depthBudget: number): Value {
  switch (type) {
    case ValueType.String:
      return value.string(in_.string());
    case ValueType.Int:
      return { type: ValueType.Int, value: in_.i64() };
    case ValueType.Float:
      return value.float(in_.f64());
    case ValueType.Bool:
      return value.bool(in_.u8() !== 0);
    case ValueType.Bytes: {
      const length = in_.u32();
      if (length > in_.remaining) {
        throw new ProtocolError(`metadata bytes of length ${length} exceed the ${in_.remaining} remaining`);
      }
      return value.bytes(in_.bytes(length).slice());
    }
    case ValueType.Hash:
      if (depthBudget === 0) {
        throw new ProtocolError("metadata nested too deep");
      }
      return value.hash(Metadata.decode(in_, depthBudget - 1));
    case ValueType.Array: {
      if (depthBudget === 0) {
        throw new ProtocolError("metadata nested too deep");
      }
      const elementType = in_.u8();
      if (!isKnownType(elementType)) {
        throw new ProtocolError(`metadata array element type ${elementType} is not defined`);
      }
      const count = in_.u32();
      if (count > Math.floor(in_.remaining / minimumBodyBytes(elementType))) {
        throw new ProtocolError(`metadata array of ${count} elements cannot fit in ${in_.remaining} bytes`);
      }
      const elements: Value[] = [];
      for (let i = 0; i < count; ++i) {
        elements.push(decodeBody(in_, elementType, depthBudget - 1));
      }
      return value.array(elementType, elements);
    }
  }
}
