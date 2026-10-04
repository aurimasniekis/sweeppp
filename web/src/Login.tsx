// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

import { type FormEvent, useState } from "react";

export function Login({ onLoggedIn }: { onLoggedIn: () => void }) {
  const [token, setToken] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError("");
    const response = await fetch("/login", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({ token }).toString(),
    });
    setBusy(false);
    if (response.ok) {
      onLoggedIn();
    } else {
      setError((await response.text()).trim());
    }
  };

  return (
    <form
      onSubmit={submit}
      className="mx-auto mt-24 flex max-w-sm flex-col gap-3 rounded-lg border border-border bg-panel p-6"
    >
      <h1 className="text-lg font-semibold">Sweep++</h1>
      <label className="text-sm text-dim" htmlFor="token">
        The server's token
      </label>
      <input
        id="token"
        type="password"
        autoComplete="current-password"
        value={token}
        onChange={(event) => setToken(event.target.value)}
        className="rounded border border-border bg-window px-3 py-2 outline-none focus:border-accent"
      />
      {error && <p className="text-sm text-warning">{error}</p>}
      <button
        type="submit"
        disabled={busy || token.length === 0}
        className="rounded bg-accent px-3 py-2 font-medium text-white disabled:opacity-50"
      >
        Log in
      </button>
    </form>
  );
}
