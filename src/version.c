#include "version.h"
#include "build_time.h"     /* generated: FW_BUILD_TIME */

#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif

const char FW_VERSION_STR[] = FW_VERSION;
const char FW_BUILD_STR[] = FW_BUILD_TIME;
