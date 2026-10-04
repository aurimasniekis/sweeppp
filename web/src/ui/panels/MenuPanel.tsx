// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useSelector } from "@tanstack/react-store";
import { useEffect, useState } from "react";

import { sessionStore } from "../../state/session";
import { AntennasSection } from "./menu/AntennasSection";
import { ContributorsSection } from "./menu/ContributorsSection";
import { DisplaySection } from "./menu/DisplaySection";
import { ProfilesSection } from "./menu/ProfilesSection";
import { Section } from "./menu/Section";
import { ThemeSection } from "./menu/ThemeSection";

function ApplicationSection() {
  const serverName = useSelector(sessionStore, (s) => s.serverName);
  const [passwordRequired, setPasswordRequired] = useState(false);
  useEffect(() => {
    const controller = new AbortController();
    fetch("/api/auth", { signal: controller.signal })
      .then((r) => r.json() as Promise<{ required: boolean }>)
      .then((auth) => setPasswordRequired(auth.required))
      .catch(() => undefined);
    return () => controller.abort();
  }, []);
  const logOut = () => {
    void fetch("/logout", { method: "POST" })
      .catch(() => undefined)
      .finally(() => location.reload());
  };
  return (
    <div>
      {serverName && <p className="caption mb-1.5">Connected to {serverName}</p>}
      {passwordRequired && (
        <button className="btn w-full" onClick={logOut}>
          Log out
        </button>
      )}
    </div>
  );
}

/** The ☰ menu: how this page looks, its saved looks, the antenna library and
 * the page itself. */
export function MenuPanel() {
  return (
    <div>
      <Section title="Display" open>
        <DisplaySection />
      </Section>
      <Section title="Theme">
        <ThemeSection />
      </Section>
      <Section title="Profiles">
        <ProfilesSection />
      </Section>
      <Section title="Antennas">
        <AntennasSection />
      </Section>
      <Section title="Data contributors">
        <ContributorsSection />
      </Section>
      <Section title="Application">
        <ApplicationSection />
      </Section>
    </div>
  );
}
