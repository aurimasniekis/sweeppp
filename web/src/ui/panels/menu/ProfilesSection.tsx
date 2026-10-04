// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useState } from "react";

import { NoticeKind } from "../../../protocol/messages";
import { defaultView, type ViewState, viewStore } from "../../../state/view";
import { toast } from "../../toasts";
import { ConfirmButton } from "./Section";
import { Icon } from "../../Icon";
import { icon } from "../../icons";

const kStorageKey = "sweeppp.profiles";

/** Everything this page draws with, less the markers: those mark a
 * measurement, not a way of looking at one. */
type Profile = Partial<Omit<ViewState, "markers">>;

function readProfiles(): Record<string, Profile> {
  try {
    const parsed: unknown = JSON.parse(localStorage.getItem(kStorageKey) ?? "{}");
    return parsed && typeof parsed === "object" && !Array.isArray(parsed) ? (parsed as Record<string, Profile>) : {};
  } catch {
    return {};
  }
}

function writeProfiles(profiles: Record<string, Profile>): boolean {
  try {
    localStorage.setItem(kStorageKey, JSON.stringify(profiles));
    return true;
  } catch {
    toast(NoticeKind.Error, "this browser would not store the profile");
    return false;
  }
}

function capture(): Profile {
  const { markers: _markers, ...rest } = viewStore.state;
  return rest;
}

/** Laid over the defaults, as the saved view is, so a profile from before a
 * setting existed still loads. */
function apply(profile: Profile): void {
  const defaults = defaultView();
  viewStore.setState((current) => ({
    ...defaults,
    ...profile,
    layout: { ...defaults.layout, ...profile.layout },
    markers: current.markers,
  }));
}

export function ProfilesSection() {
  const [profiles, setProfiles] = useState(readProfiles);
  const [name, setName] = useState("profile");
  const trimmed = name.trim();
  const exists = Object.hasOwn(profiles, trimmed);
  const names = Object.keys(profiles).sort((a, b) => a.localeCompare(b));

  const save = () => {
    const next = { ...readProfiles(), [trimmed]: capture() };
    if (writeProfiles(next)) {
      setProfiles(next);
      toast(NoticeKind.Success, `profile '${trimmed}' saved`);
    }
  };

  const remove = (doomed: string) => {
    const next = { ...readProfiles() };
    delete next[doomed];
    if (writeProfiles(next)) {
      setProfiles(next);
      toast(NoticeKind.Success, `profile '${doomed}' deleted`);
    }
  };

  return (
    <div>
      <p className="caption mb-1.5">This browser's display settings only. The radio's setup lives on the server.</p>
      <div className="flex gap-1">
        <input
          className="field"
          value={name}
          onChange={(e) => setName(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter" && trimmed && !exists) {
              save();
            }
          }}
        />
        {exists ? (
          <ConfirmButton prompt="Replace?" onConfirm={save}>
            Save
          </ConfirmButton>
        ) : (
          <button className="btn" disabled={!trimmed} onClick={save}>
            Save
          </button>
        )}
      </div>
      <div className="section-title">Saved</div>
      {names.length === 0 && <p className="caption">None saved yet.</p>}
      {names.map((profileName) => (
        <div key={profileName} className="flex items-center gap-1 py-0.5">
          <button
            className="btn min-w-0 flex-1 justify-start"
            title={`Load '${profileName}'`}
            onClick={() => {
              const profile = profiles[profileName];
              if (profile) {
                apply(profile);
                setName(profileName);
                toast(NoticeKind.Success, `profile '${profileName}' loaded`);
              }
            }}
          >
            <span className="truncate">{profileName}</span>
          </button>
          <ConfirmButton
            className="btn-icon"
            title={`Delete '${profileName}'`}
            prompt="Delete?"
            onConfirm={() => remove(profileName)}
          >
            <Icon path={icon.delete} />
          </ConfirmButton>
        </div>
      ))}
    </div>
  );
}
