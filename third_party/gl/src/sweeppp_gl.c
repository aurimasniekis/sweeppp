// SPDX-FileCopyrightText: 2026 Aurimas Niekis <aurimas@niekis.lt>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweeppp_gl.h"

#include <stddef.h>

#define SWEEPPP_GL_DEFINE(ret, name, params) PFN_sweeppp_##name sweeppp_##name = NULL;
SWEEPPP_GL_FUNCTIONS(SWEEPPP_GL_DEFINE)
#undef SWEEPPP_GL_DEFINE

static const char* g_firstMissing = NULL;

int sweeppp_gl_load(SweepppGlGetProcAddress getProcAddress) {
    int missing = 0;
    g_firstMissing = NULL;

    if (getProcAddress == NULL) {
        g_firstMissing = "<null loader>";
        return 1;
    }

#define SWEEPPP_GL_RESOLVE(ret, name, params)                                                      \
    sweeppp_##name = (PFN_sweeppp_##name)getProcAddress(#name);                                    \
    if (sweeppp_##name == NULL) {                                                                  \
        ++missing;                                                                                 \
        if (g_firstMissing == NULL) {                                                              \
            g_firstMissing = #name;                                                                \
        }                                                                                          \
    }
    SWEEPPP_GL_FUNCTIONS(SWEEPPP_GL_RESOLVE)
#undef SWEEPPP_GL_RESOLVE

    return missing;
}

const char* sweeppp_gl_first_missing(void) {
    return g_firstMissing;
}
