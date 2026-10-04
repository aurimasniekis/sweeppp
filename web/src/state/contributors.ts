// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { Store } from "@tanstack/react-store";

import { op } from "../protocol/messages";
import { Metadata, value, ValueType } from "../protocol/metadata";
import type { Overlay } from "../render/spectrum";
import { send } from "./actions";

/** One row of a contributor's tick tree, as `/api/contributors` lists it. */
export interface TreeRow {
  depth: number;
  key: string;
  name: string;
  detail: string;
  description: string;
  color: [number, number, number, number];
  anyOn: boolean;
  allOn: boolean;
}

/** One of the server's data contributors: the band plan, the channel lists. */
export interface Contributor {
  id: string;
  name: string;
  caption: string;
  shown: boolean;
  datasets: { name: string; description: string }[];
  activeDataset: number;
  tree: TreeRow[];
}

export interface ContributorsState {
  /** The server's counter these were read at; -1 before the first read. */
  generation: number;
  contributors: Contributor[];
}

export const contributorsStore = new Store<ContributorsState>({ generation: -1, contributors: [] });

let fetching: Promise<void> | null = null;
let wanted = -1;

/** Reads the list again when the server's counter has moved past it. */
export function refreshContributors(generation: number): void {
  wanted = Math.max(wanted, generation);
  if (fetching || contributorsStore.state.generation >= wanted) {
    return;
  }
  fetching = fetch("/api/contributors")
    .then((r) => (r.ok ? (r.json() as Promise<{ generation: number; contributors: Contributor[] }>) : null))
    .then((body) => {
      if (body) {
        contributorsStore.setState(() => ({ generation: body.generation, contributors: body.contributors }));
      }
    })
    .catch(() => undefined)
    .finally(() => {
      fetching = null;
      // Something changed again while that was in flight.
      if (wanted > contributorsStore.state.generation && contributorsStore.state.generation >= 0) {
        refreshContributors(wanted);
      }
    });
}

export function setContributorShown(id: string, shown: boolean): void {
  send(op.setContributorShown, new Metadata().setString("id", id).setBool("shown", shown));
}

export function setContributorOrder(ids: string[]): void {
  const list = value.array(
    ValueType.String,
    ids.map((id) => value.string(id)),
  );
  send(op.setContributorOrder, new Metadata().set("ids", list));
}

export function selectContributorDataset(id: string, index: number): void {
  send(op.selectContributorDataset, new Metadata().setString("id", id).setInt("index", index));
}

export function toggleContributorRow(id: string, key: string): void {
  send(op.toggleContributorRow, new Metadata().setString("id", id).setString("key", key));
}

/** Asks the contributor to stop offering one entry -- ctrl-click on the plot. */
export function hideContribution(overlay: Overlay): void {
  send(
    op.hideContribution,
    new Metadata()
      .setString("id", overlay.plugin)
      .setString("name", overlay.name)
      .setString("category", overlay.category)
      .setFloat("startHz", overlay.startHz)
      .setFloat("stopHz", overlay.stopHz),
  );
}
