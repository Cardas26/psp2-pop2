#include <stdio.h>
#include <string.h>
#include "levers.h"
#ifndef LEVER_OPL_DEFAULT
#define LEVER_OPL_DEFAULT 1
#endif
#ifndef LEVER_PRESENT_DEFAULT
#define LEVER_PRESENT_DEFAULT 1
#endif
#ifndef LEVER_SLEEP_DEFAULT
#define LEVER_SLEEP_DEFAULT 1
#endif
#ifndef LEVER_VSYNC_DEFAULT
#define LEVER_VSYNC_DEFAULT 1
#endif
#ifndef LEVER_POLL_DEFAULT
#define LEVER_POLL_DEFAULT 1
#endif
int lever_cfg[LEVER_N] = { LEVER_OPL_DEFAULT, LEVER_PRESENT_DEFAULT, LEVER_SLEEP_DEFAULT, LEVER_VSYNC_DEFAULT, LEVER_POLL_DEFAULT };
int perf_arm = 1;
