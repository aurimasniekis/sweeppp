// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

/** Maps the server's monotonic clock onto this page's, from ping round trips:
 * the offset taken from the fastest of the last 32, which is the one least
 * skewed by queueing. A port of remote::ClockMap. */
export class ClockMap {
  static readonly kWindow = 32;

  private samples: { roundTripNs: bigint; offsetNs: bigint }[] = [];
  private next = 0;
  private lastMappedNs = 0n;
  lastRoundTripNs = 0n;

  get calibrated(): boolean {
    return this.samples.length > 0;
  }

  observe(clientSentNs: bigint, serverNs: bigint, clientReceivedNs: bigint): void {
    if (clientReceivedNs < clientSentNs) {
      return;
    }
    const roundTripNs = clientReceivedNs - clientSentNs;
    const midpoint = clientSentNs + roundTripNs / 2n;
    const sample = { roundTripNs, offsetNs: midpoint - serverNs };
    if (this.samples.length < ClockMap.kWindow) {
      this.samples.push(sample);
    } else {
      this.samples[this.next] = sample;
    }
    this.next = (this.next + 1) % ClockMap.kWindow;
    this.lastRoundTripNs = roundTripNs;
  }

  private best(): { roundTripNs: bigint; offsetNs: bigint } {
    let fastest = this.samples[0]!;
    for (const sample of this.samples) {
      if (sample.roundTripNs < fastest.roundTripNs) {
        fastest = sample;
      }
    }
    return fastest;
  }

  get bestRoundTripNs(): bigint {
    return this.calibrated ? this.best().roundTripNs : 0n;
  }

  /** A server time on this clock: never after now, and never before what it
   * last answered, so live times only move forward. */
  toClient(serverNs: bigint, nowNs: bigint): bigint {
    let mapped = nowNs;
    if (this.calibrated) {
      const candidate = serverNs + this.best().offsetNs;
      mapped = candidate < nowNs ? candidate : nowNs;
    }
    if (mapped > this.lastMappedNs) {
      this.lastMappedNs = mapped;
    }
    return this.lastMappedNs;
  }

  reset(): void {
    this.samples = [];
    this.next = 0;
    this.lastMappedNs = 0n;
    this.lastRoundTripNs = 0n;
  }
}
