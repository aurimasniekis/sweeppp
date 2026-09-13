// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

// A shared object that loads and is not a plugin.
//
// The most likely thing to find in a plugins directory that is not a plugin --
// a stray library, a half-installed package -- and the case where "listed with
// the reason" earns its keep: the host has to say *no entry point* rather than
// either crashing or silently showing an empty list.
extern "C" int sweepppTestNotAPlugin(void) {
    return 0;
}
