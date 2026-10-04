// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { useCallback, useEffect, useState } from "react";

import { loadThemes } from "./state/theme";
import { Login } from "./Login";
import { Workspace } from "./ui/Workspace";

type Auth = { required: boolean; authenticated: boolean };

export function App() {
  const [auth, setAuth] = useState<Auth | null>(null);
  const [error, setError] = useState("");

  const refresh = useCallback(() => {
    fetch("/api/auth")
      .then((response) => response.json() as Promise<Auth>)
      .then((a) => {
        setAuth(a);
        setError("");
      })
      .catch(() => setError("The server is not answering."));
  }, []);

  useEffect(() => {
    refresh();
    void loadThemes().catch(() => undefined);
  }, [refresh]);

  if (error) {
    return (
      <div className="p-6">
        <p className="text-warning">{error}</p>
        <button className="btn mt-3" onClick={refresh}>
          Try again
        </button>
      </div>
    );
  }
  if (!auth) {
    return <p className="p-6 text-dim">Connecting…</p>;
  }
  if (auth.required && !auth.authenticated) {
    return <Login onLoggedIn={refresh} />;
  }
  return <Workspace onLoggedOut={refresh} />;
}
