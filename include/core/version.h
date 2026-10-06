#ifndef HEDDLE_VERSION_H
#define HEDDLE_VERSION_H

#ifndef BUILD_VERSION
#define BUILD_VERSION "0.0.0-dev"
#endif

#ifndef BUILD_COMMIT
#define BUILD_COMMIT "unknown"
#endif

#ifndef BUILD_TARGET
#define BUILD_TARGET "unknown"
#endif

const char *heddle_version_string(void);

#endif
