#pragma once
enum { LEVER_OPL, LEVER_PRESENT, LEVER_SLEEP, LEVER_VSYNC, LEVER_POLL, LEVER_N };
extern int lever_cfg[LEVER_N];
extern int perf_arm;
static inline int lever_on(int l) { int c = lever_cfg[l]; return c == 2 ? perf_arm : c; }
