// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";

import { decodeControl, section } from "../protocol/messages";
import { takeControl } from "../state/actions";
import { session, sessionStore } from "../state/session";

/** Under the bar while another client controls the radio. */
export function ControlBanner() {
  useSelector(sessionStore, (s) => s.sectionsVersion);
  if (!session.remote?.sections.has(section.control)) {
    return null;
  }
  const control = decodeControl(session.section(section.control));
  if (control.you) {
    return null;
  }
  return (
    <div className="flex shrink-0 items-center gap-3 border-b border-border bg-warning/15 px-3 py-1.5">
      <span>
        {control.held ? (
          <>
            Watching — <b>{control.controller}</b> ({control.controllerKind}) has control
          </>
        ) : (
          "Watching — nobody has control"
        )}
      </span>
      <button className="btn" onClick={takeControl}>
        Take control
      </button>
    </div>
  );
}
