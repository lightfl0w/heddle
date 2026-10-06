#include "version.h"

#include <stdio.h>

static char g_version[256];
static int  g_built;

const char *heddle_version_string(void) {
    if (!g_built) {
        snprintf(g_version, sizeof(g_version), "%s (%s, %s)", BUILD_VERSION, BUILD_TARGET,
                 BUILD_COMMIT);
        g_built = 1;
    }

    return g_version;
}
