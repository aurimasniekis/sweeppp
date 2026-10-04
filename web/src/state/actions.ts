// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { op, section } from "../protocol/messages";
import { Metadata } from "../protocol/metadata";
import { NoticeKind } from "../protocol/messages";
import { toast } from "../ui/toasts";
import { session } from "./session";

/** A command to the server, refused here while watching: nothing is changed
 * locally or asked of a server that would only say no. */
export function send(name: string, args = new Metadata(), local?: (sections: Map<string, Metadata>) => void): boolean {
  const remote = session.remote;
  if (!remote || remote.closed) {
    return false;
  }
  if (!remote.canControl && name !== op.takeControl && name !== op.releaseControl && name !== op.setLinkResolution) {
    const control = remote.control;
    toast(
      NoticeKind.Error,
      `${control.held ? `${control.controller} has control` : "Nobody has control"}; take control to change this`,
    );
    return false;
  }
  remote.command(name, args, local);
  return true;
}

/** A copy of a section with one field changed, for an optimistic edit. */
export function patched(name: string, change: (m: Metadata) => void): (sections: Map<string, Metadata>) => void {
  return (sections) => {
    const copy = Metadata.fromBytes(session.section(name).toBytes());
    change(copy);
    sections.set(name, copy);
  };
}

export function start(): void {
  send(op.start, new Metadata(), patched(section.run, (m) => m.setBool("running", true)));
}

export function stop(): void {
  send(op.stop, new Metadata(), patched(section.run, (m) => m.setBool("running", false)));
}

export function takeControl(): void {
  send(
    op.takeControl,
    new Metadata(),
    patched(section.control, (m) => m.setBool("you", true).setBool("held", true)),
  );
}

export function releaseControl(): void {
  send(op.releaseControl, new Metadata(), patched(section.control, (m) => m.setBool("you", false)));
}

let sentLinkBins = -1;

/** Bins the server reduces frames to for this page; zero sends them whole. */
export function setLinkResolution(bins: number): void {
  if (bins === sentLinkBins) {
    return;
  }
  sentLinkBins = bins;
  send(op.setLinkResolution, new Metadata().setInt("maxBins", bins));
}

export function forgetLinkResolution(): void {
  sentLinkBins = -1;
}
