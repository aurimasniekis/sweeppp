// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import {
  contributorsStore,
  selectContributorDataset,
  setContributorOrder,
  setContributorShown,
} from "../../../state/contributors";
import { useInstrument } from "../../../state/instrument";
import { Select } from "../../controls";
import { Icon } from "../../Icon";
import { icon } from "../../icons";

/** Who answers "what is at this frequency", and in what order: the server's
 * contributors, the first of which titles the marker. */
export function ContributorsSection() {
  const contributors = useSelector(contributorsStore, (c) => c.contributors);
  const disabled = !useInstrument().canControl;
  if (contributors.length === 0) {
    return <p className="caption">Nothing on the server is contributing frequency data.</p>;
  }

  const move = (from: number, to: number) => {
    const ids = contributors.map((c) => c.id);
    const [moved] = ids.splice(from, 1);
    ids.splice(to, 0, moved!);
    setContributorOrder(ids);
  };

  return (
    <div>
      <p className="caption mb-1">The order decides which one titles the marker.</p>
      {contributors.map((c, i) => (
        <div key={c.id} className="border-b border-separator py-1.5 last:border-b-0">
          <div className="flex items-center gap-1.5">
            <input
              type="checkbox"
              className="size-3.5 accent-[var(--color-accent)]"
              checked={c.shown}
              disabled={disabled}
              title="Show on the spectrum"
              onChange={(e) => setContributorShown(c.id, e.target.checked)}
            />
            <span className="truncate font-medium">{c.name}</span>
            <span className="ml-auto flex gap-0.5">
              <button className="btn-icon h-6 w-6" disabled={disabled || i === 0} title="Higher" onClick={() => move(i, i - 1)}>
                <Icon path={icon.up} />
              </button>
              <button
                className="btn-icon h-6 w-6"
                disabled={disabled || i === contributors.length - 1}
                title="Lower"
                onClick={() => move(i, i + 1)}
              >
                <Icon path={icon.down} />
              </button>
            </span>
          </div>
          {c.caption && <p className="caption ml-5">{c.caption}</p>}
          {c.datasets.length > 1 && (
            <div className="ml-5 mt-1">
              <Select
                value={String(c.activeDataset)}
                disabled={disabled}
                choices={c.datasets.map((d, index) => ({ value: String(index), label: d.name, title: d.description }))}
                onChange={(v) => selectContributorDataset(c.id, Number(v))}
              />
            </div>
          )}
        </div>
      ))}
    </div>
  );
}
