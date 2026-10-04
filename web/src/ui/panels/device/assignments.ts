// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import type { AntennaAssignment, AntennaAssignments, RfPathInfo } from "../../../protocol/wire";

// The edits of rf/AntennaAssignments.cpp, made on a copy that is then sent whole.

const isPortEntry = (entry: AntennaAssignment) => entry.device !== "";

export function copyAssignments(a: AntennaAssignments): AntennaAssignments {
  return {
    entries: a.entries.map((e) => ({ ...e })),
    fallbackPorts: a.fallbackPorts.map(([device, port]) => [device, port]),
  };
}

export function rfPathKey(info: RfPathInfo): string {
  return `${info.driver}:${info.serial || info.id}`;
}

function portEntry(a: AntennaAssignments, device: string, port: string): number {
  return a.entries.findIndex((e) => e.device === device && e.port === port);
}

function inputEntry(a: AntennaAssignments, switcher: string, input: string): number {
  return a.entries.findIndex((e) => !isPortEntry(e) && e.switcher === switcher && e.input === input);
}

export function antennaFor(a: AntennaAssignments, device: string, port: string): string {
  return a.entries[portEntry(a, device, port)]?.antenna ?? "";
}

export function switcherFor(a: AntennaAssignments, device: string, port: string): string {
  return a.entries[portEntry(a, device, port)]?.switcher ?? "";
}

export function antennaOnInput(a: AntennaAssignments, switcher: string, input: string): string {
  return a.entries[inputEntry(a, switcher, input)]?.antenna ?? "";
}

export function portOfSwitcher(a: AntennaAssignments, switcher: string): [device: string, port: string] | null {
  const match = a.entries.find((e) => isPortEntry(e) && e.switcher === switcher);
  return match ? [match.device, match.port] : null;
}

export function fallbackPort(a: AntennaAssignments, device: string): string {
  return a.fallbackPorts.find(([d]) => d === device)?.[1] ?? "";
}

/** A connector carries one thing: an antenna replaces a switcher on it. */
export function assign(a: AntennaAssignments, device: string, port: string, antenna: string): void {
  const index = portEntry(a, device, port);
  const match = a.entries[index];
  if (!antenna) {
    if (match) {
      a.entries.splice(index, 1);
    }
    return;
  }
  if (match) {
    match.switcher = "";
    match.antenna = antenna;
    return;
  }
  a.entries.push({ device, port, switcher: "", input: "", antenna });
}

/** A switcher has one output, so it is detached from any other port first. */
export function assignSwitcher(a: AntennaAssignments, device: string, port: string, switcher: string): void {
  if (switcher) {
    for (const entry of a.entries) {
      if (entry.switcher === switcher && isPortEntry(entry) && !(entry.device === device && entry.port === port)) {
        entry.switcher = "";
      }
    }
    a.entries = a.entries.filter((e) => !(isPortEntry(e) && e.switcher === "" && e.antenna === ""));
  }
  const index = portEntry(a, device, port);
  const match = a.entries[index];
  if (!switcher) {
    if (match) {
      a.entries.splice(index, 1);
    }
    return;
  }
  if (match) {
    match.antenna = "";
    match.switcher = switcher;
    return;
  }
  a.entries.push({ device, port, switcher, input: "", antenna: "" });
}

export function assignInput(a: AntennaAssignments, switcher: string, input: string, antenna: string): void {
  const index = inputEntry(a, switcher, input);
  const match = a.entries[index];
  if (!antenna) {
    if (match) {
      a.entries.splice(index, 1);
    }
    return;
  }
  if (match) {
    match.antenna = antenna;
    return;
  }
  a.entries.push({ device: "", port: "", switcher, input, antenna });
}

export function setFallbackPort(a: AntennaAssignments, device: string, port: string): void {
  if (!device) {
    return;
  }
  const index = a.fallbackPorts.findIndex(([d]) => d === device);
  const match = a.fallbackPorts[index];
  if (!port) {
    if (match) {
      a.fallbackPorts.splice(index, 1);
    }
    return;
  }
  if (match) {
    match[1] = port;
    return;
  }
  a.fallbackPorts.push([device, port]);
}
