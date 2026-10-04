// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import type { ReactNode } from "react";

import { NoticeKind } from "../../protocol/messages";
import type { SdrDeviceInfo, VersionReport } from "../../protocol/wire";
import { useInstrument } from "../../state/instrument";
import { sessionStore } from "../../state/session";
import { Readout } from "../controls";
import { toast } from "../toasts";
import { Antennas } from "./device/Antennas";
import { Parameters } from "./device/Parameters";
import { Clients, Recordings } from "./device/Server";

/** The ends of a long serial, which is what a comparison against a label
 * reads; a radio padding its id out with zeros has them stripped first. */
function shortSerial(serial: string): string {
  const keep = 8;
  let begin = serial.search(/[^0]/);
  if (begin < 0 || serial.length - begin < keep) {
    begin = 0;
  }
  const trimmed = serial.slice(begin);
  return trimmed.length <= keep * 2 + 3 ? trimmed : `${trimmed.slice(0, keep)}...${trimmed.slice(-keep)}`;
}

function copySerial(serial: string): void {
  const copied = () => toast(NoticeKind.Info, "serial copied to the clipboard");
  // The clipboard API exists only on secure pages, and a server on the bench
  // is usually plain http.
  if (navigator.clipboard) {
    navigator.clipboard.writeText(serial).then(copied, () => toast(NoticeKind.Error, "could not copy the serial"));
    return;
  }
  const area = document.createElement("textarea");
  area.value = serial;
  document.body.append(area);
  area.select();
  const ok = document.execCommand("copy");
  area.remove();
  if (ok) {
    copied();
  } else {
    toast(NoticeKind.Error, "could not copy the serial");
  }
}

function version(report: VersionReport): ReactNode {
  if (report.aheadOfDriver) {
    return (
      <span className="text-warning" title={`Newer than this driver supports (${report.knownLatest})`}>
        {report.version}
      </span>
    );
  }
  if (!report.knownLatest) {
    return report.version;
  }
  return (
    <>
      {report.version}{" "}
      <span className={report.knownLatest === report.version ? "text-dim" : "text-accent"}>
        (supported: {report.knownLatest})
      </span>
    </>
  );
}

function Identity({ info, server }: { info: SdrDeviceInfo; server: string }) {
  const rows: [string, ReactNode, string?][] = [];
  if (server) {
    rows.push(["Server", server, location.host]);
  }
  if (info.serial) {
    rows.push([
      "Serial",
      <button
        className="cursor-copy tabular-nums hover:text-accent"
        onClick={() => copySerial(info.serial)}
      >
        {shortSerial(info.serial)}
      </button>,
      `${info.serial}\n\nClick to copy.`,
    ]);
  }
  if (info.firmware.version) {
    rows.push(["Firmware", version(info.firmware)]);
  }
  if (info.fpga.version) {
    rows.push(["FPGA", version(info.fpga)]);
  }
  if (info.linkDescription) {
    rows.push(["Link", info.linkDescription]);
  }
  return rows.length > 0 ? <Readout rows={rows} /> : null;
}

/** The server's radio, its connectors and the server itself. The radio is the
 * server's to open and close, so there is no device chooser here. */
export function DevicePanel() {
  const instrument = useInstrument();
  const server = useSelector(sessionStore, (s) => s.serverName);
  const descriptor = instrument.device.descriptor;

  return (
    <div>
      {descriptor ? (
        <>
          <Identity info={descriptor.info} server={server} />
          <Parameters instrument={instrument} />
          <Antennas instrument={instrument} />
        </>
      ) : (
        <p className="caption">No radio is open on {server || "the server"}.</p>
      )}
      <Recordings instrument={instrument} server={server || "the server"} />
      {instrument.control.shared && <Clients instrument={instrument} server={server || "the server"} />}
    </div>
  );
}
