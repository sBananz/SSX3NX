/*
 * nx_platform.h -- the Nintendo Switch host layer (SSX3NX).
 *
 * Ported from OpenTrickyNX (port/src/posix/ot_switch_nx.c and the Switch half
 * of ot_posix.c, GPL-3.0-only); the clock code follows flatout2-sw by sp00nz
 * (MIT). See PORTING_PLAN.md §5.3.
 */
#ifndef SSX3NX_NX_PLATFORM_H
#define SSX3NX_NX_PLATFORM_H
#if defined(__SWITCH__)
#include <stddef.h>

/* The .nro's folder ("sdmc:/switch/ssx3nx"), without a trailing slash. */
#define SSX3NX_DEFAULT_DIR "sdmc:/switch/ssx3nx"

void nx_set_args(int argc, char **argv);
/* Log to <nro dir>/SSX3NX.log (or nxlink), "SSX 3 env.txt" into the
 * environment, abort handler, applet-mode check, clocks, the game thread on
 * core 0, romfs. Call first thing in main(). */
void nx_host_init(void);
/* The folder holding the .nro (no trailing slash). */
const char *nx_exe_dir(void);
void nx_display_size(int *w, int *h);
int nx_docked(void);               /* 1 on the TV (docked), 0 handheld */
void nx_env_load(void);
void nx_clocks(void);
#endif
#endif
