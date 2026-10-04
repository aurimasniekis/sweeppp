// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

/** Why a stream or a record could not be read. Fatal for the stream. */
export class ProtocolError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ProtocolError";
  }
}

const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: false });

export function utf8(text: string): Uint8Array {
  return encoder.encode(text);
}

/** Little-endian, growing as it is written; the `.sweeps` byte order. */
export class ByteWriter {
  private buffer = new Uint8Array(256);
  private view = new DataView(this.buffer.buffer);
  private length = 0;

  get size(): number {
    return this.length;
  }

  private reserve(bytes: number): void {
    if (this.length + bytes <= this.buffer.length) {
      return;
    }
    let capacity = this.buffer.length * 2;
    while (capacity < this.length + bytes) {
      capacity *= 2;
    }
    const grown = new Uint8Array(capacity);
    grown.set(this.buffer.subarray(0, this.length));
    this.buffer = grown;
    this.view = new DataView(grown.buffer);
  }

  u8(value: number): this {
    this.reserve(1);
    this.view.setUint8(this.length, value);
    this.length += 1;
    return this;
  }

  u16(value: number): this {
    this.reserve(2);
    this.view.setUint16(this.length, value, true);
    this.length += 2;
    return this;
  }

  u32(value: number): this {
    this.reserve(4);
    this.view.setUint32(this.length, value >>> 0, true);
    this.length += 4;
    return this;
  }

  u64(value: bigint): this {
    this.reserve(8);
    this.view.setBigUint64(this.length, BigInt.asUintN(64, value), true);
    this.length += 8;
    return this;
  }

  i64(value: bigint): this {
    this.reserve(8);
    this.view.setBigInt64(this.length, BigInt.asIntN(64, value), true);
    this.length += 8;
    return this;
  }

  f32(value: number): this {
    this.reserve(4);
    this.view.setFloat32(this.length, value, true);
    this.length += 4;
    return this;
  }

  f64(value: number): this {
    this.reserve(8);
    this.view.setFloat64(this.length, value, true);
    this.length += 8;
    return this;
  }

  bytes(data: Uint8Array): this {
    this.reserve(data.length);
    this.buffer.set(data, this.length);
    this.length += data.length;
    return this;
  }

  /** A u32 length and the UTF-8 bytes. */
  string(text: string): this {
    const data = utf8(text);
    return this.u32(data.length).bytes(data);
  }

  finish(): Uint8Array {
    return this.buffer.slice(0, this.length);
  }
}

/** A bounds-checked cursor: every read can fail, because these bytes may come
 * from a truncated stream or a hostile peer. */
export class ByteReader {
  private readonly view: DataView;
  offset = 0;

  constructor(private readonly data: Uint8Array) {
    this.view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  }

  get remaining(): number {
    return this.data.length - this.offset;
  }

  private need(bytes: number): void {
    if (this.remaining < bytes) {
      throw new ProtocolError(`truncated at offset ${this.offset}`);
    }
  }

  u8(): number {
    this.need(1);
    return this.view.getUint8(this.offset++);
  }

  u16(): number {
    this.need(2);
    const value = this.view.getUint16(this.offset, true);
    this.offset += 2;
    return value;
  }

  u32(): number {
    this.need(4);
    const value = this.view.getUint32(this.offset, true);
    this.offset += 4;
    return value;
  }

  u64(): bigint {
    this.need(8);
    const value = this.view.getBigUint64(this.offset, true);
    this.offset += 8;
    return value;
  }

  i64(): bigint {
    this.need(8);
    const value = this.view.getBigInt64(this.offset, true);
    this.offset += 8;
    return value;
  }

  f32(): number {
    this.need(4);
    const value = this.view.getFloat32(this.offset, true);
    this.offset += 4;
    return value;
  }

  f64(): number {
    this.need(8);
    const value = this.view.getFloat64(this.offset, true);
    this.offset += 8;
    return value;
  }

  bytes(count: number): Uint8Array {
    this.need(count);
    const value = this.data.subarray(this.offset, this.offset + count);
    this.offset += count;
    return value;
  }

  string(): string {
    const length = this.u32();
    if (length > this.remaining) {
      throw new ProtocolError(
        `string of ${length} bytes at offset ${this.offset} exceeds the ${this.remaining} remaining`,
      );
    }
    return decoder.decode(this.bytes(length));
  }
}
