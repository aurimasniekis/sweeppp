// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { type ReactNode, useState } from "react";

import { duration } from "../../../render/format";
import {
  cancelLearning,
  clearAutoSpurs,
  clearCorrections,
  type InstrumentView,
  setCorrectionSettings,
  startLearning,
} from "../../../state/instrument";
import { sessionStore } from "../../../state/session";
import type { CorrectionSettings } from "../../../protocol/wire";
import { Switch, SwitchRow } from "../../controls";
import { kLearnFrames, kLearnPasses } from "./model";

/** Inline rather than a dialog: a dialog's portal sits outside the popover,
 * and the first click in it would close the popover and the prompt with it. */
function Prompt({ title, children, actions }: { title: string; children: ReactNode; actions: ReactNode }) {
  return (
    <div className="mt-2 rounded border border-border bg-header p-2">
      <div className="mb-1 font-semibold">{title}</div>
      <div className="space-y-1.5">{children}</div>
      <div className="mt-2 flex flex-wrap gap-2">{actions}</div>
    </div>
  );
}

function Tip({ text, children }: { text: string; children: ReactNode }) {
  return <div title={text}>{children}</div>;
}

export function CorrectionsBlock({ instrument, disabled }: { instrument: InstrumentView; disabled: boolean }) {
  const serverName = useSelector(sessionStore, (s) => s.serverName);
  const [prompt, setPrompt] = useState<"learn" | "clear" | null>(null);
  const settings = instrument.corrections.settings;
  const learned = instrument.corrections.summary;
  const sweeping = instrument.run.sweeping;
  const running = instrument.run.running;
  const haveFloor = learned.floorPoints > 0;
  const stale = learned.floorStaleReason;
  const spurs = learned.spurs;
  const automatic = learned.automaticSpurs;

  const set = (change: Partial<CorrectionSettings>) => setCorrectionSettings({ ...settings, ...change });

  let status: string;
  if (!learned.present) {
    status = "nothing learned for this radio";
  } else {
    status = !haveFloor ? "no floor" : stale ? `floor stale (${stale} changed)` : "floor";
    status += `, ${spurs} spur${spurs === 1 ? "" : "s"}`;
    if (automatic > 0) {
      status += ` (${automatic} auto)`;
    }
    if (learned.learnedAt.length >= 10) {
      status += `, learned ${learned.learnedAt.slice(0, 10)}`;
    }
  }

  return (
    <div>
      <div className="section-title">Corrections</div>
      <SwitchRow>
        <Tip text="Subtracts each block's mean I/Q before the FFT, removing the LO leak at the centre of every step. A carrier exactly on the LO goes with it.">
          <Switch label="DC removal" checked={settings.dcRemoval} disabled={disabled} onChange={(on) => set({ dcRemoval: on })} />
        </Tip>
        <Tip
          text={
            !haveFloor
              ? "Subtracts the learned floor shape. Nothing is learned yet: press Learn."
              : stale
                ? "The learned floor is not applied: a setting differs from when it was learned. Learn again at this setting."
                : "Subtracts the learned floor shape, levelling the hump around each step's LO. A signal close to a step's LO can read low by up to the hump's height; Best stitching already prefers the measurement further from the LO."
          }
        >
          <Switch
            label="Flatten floor"
            checked={settings.flatten}
            disabled={disabled || !haveFloor || stale !== ""}
            onChange={(on) => set({ flatten: on })}
          />
        </Tip>
        <Tip
          text={
            spurs === 0
              ? "Replaces the learned spurs with a line between their neighbours. Nothing is learned yet: press Learn."
              : "Replaces the bins under each learned spur with a line between their neighbours."
          }
        >
          <Switch
            label="Spur mask"
            checked={settings.spurMask}
            disabled={disabled || spurs === 0}
            onChange={(on) => set({ spurMask: on })}
          />
        </Tip>
        {sweeping && (
          <Tip text="Keeps looking, pass by pass, for LO-offset spurs the mask does not cover yet, and adds them for this session. Never saved.">
            <Switch label="Auto spurs" checked={settings.autoSpurs} disabled={disabled} onChange={(on) => set({ autoSpurs: on })} />
          </Tip>
        )}
      </SwitchRow>

      <p className="caption">{status}</p>
      {serverName && <p className="caption">Stored on {serverName}</p>}

      {instrument.learning.active ? (
        <div className="row">
          <span>{instrument.learning.label}</span>
          <button className="btn" disabled={disabled} onClick={cancelLearning}>
            Cancel
          </button>
        </div>
      ) : prompt === "learn" ? (
        <Prompt
          title="Learn receiver corrections"
          actions={
            <>
              <button
                className="btn"
                disabled={disabled}
                onClick={() => {
                  startLearning();
                  setPrompt(null);
                }}
              >
                Learn
              </button>
              <button className="btn" onClick={() => setPrompt(null)}>
                Cancel
              </button>
            </>
          }
        >
          <p>
            Disconnect the antenna, or fit a 50-ohm load on the input, so the receiver measures only itself. Anything in the
            air while it learns is masked out of every sweep after.
          </p>
          <p>
            {sweeping
              ? `Takes about ${kLearnPasses} passes, roughly ${duration(instrument.schedule.estimatedPassSeconds * kLearnPasses)}.`
              : `Takes ${kLearnFrames} frames at this tuning.`}
          </p>
          <p className="caption">
            Use the gain, bandwidth and sample rate you will sweep with: the floor only applies while they match.
          </p>
        </Prompt>
      ) : prompt === "clear" ? (
        <Prompt
          title="Clear corrections?"
          actions={
            <>
              <button
                className="btn"
                disabled={disabled}
                onClick={() => {
                  clearCorrections();
                  setPrompt(null);
                }}
              >
                Clear
              </button>
              {automatic > 0 && (
                <button
                  className="btn"
                  disabled={disabled}
                  onClick={() => {
                    clearAutoSpurs();
                    setPrompt(null);
                  }}
                >
                  Only auto spurs
                </button>
              )}
              <button className="btn" onClick={() => setPrompt(null)}>
                Cancel
              </button>
            </>
          }
        >
          <p>Forget the learned floor and spurs for this radio? Its calibration file is deleted; Learn makes a new one.</p>
        </Prompt>
      ) : (
        <div className="mt-1 flex gap-2">
          <span
            title={
              running
                ? "Learn the floor and the spurs from what the receiver shows with nothing connected, and save them for this radio."
                : "Start acquisition first."
            }
          >
            <button className="btn" disabled={disabled || !running} onClick={() => setPrompt("learn")}>
              Learn
            </button>
          </span>
          <span title="Forget what was learned for this radio.">
            <button className="btn" disabled={disabled || !learned.present} onClick={() => setPrompt("clear")}>
              Clear
            </button>
          </span>
        </div>
      )}
    </div>
  );
}
