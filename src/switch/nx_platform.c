/*
 * nx_platform.c -- the Nintendo Switch host layer (SSX3NX).
 *
 * Ported from OpenTrickyNX (DevCrashDay/OpenTrickyNX, GPL-3.0-only):
 * port/src/posix/ot_switch_nx.c (settings file, clocks) and the Switch half of
 * port/src/posix/ot_posix.c (log devoptab, nxlink, abort screen, applet-mode
 * check, message box). The clock code follows flatout2-sw / fo2x by sp00nz
 * (MIT, https://github.com/sp00nznet/flatout2-sw, src/switch_nx.c).
 *
 * Paths: everything sits beside the .nro, sdmc:/switch/ssx3nx/ by default:
 *   SSX3NX.log          the log (stdout + stderr)
 *   SSX 3 env.txt       KEY=VALUE settings, loaded into the environment
 *   game/               the PAL rip (default.xbe + data/)
 *   Partition0-5.img    the emulated hard disk (created at first boot)
 *
 * Environment names: SSX3NX_*; the donor's OT_* names are accepted too.
 */
#if defined(__SWITCH__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/iosupport.h>
#include <netinet/in.h>   /* __nxlink_host */
#include <switch.h>

#include "nx_platform.h"

void xbox_switch_thread_core(int ideal);   /* xboxrecomp win32_compat.c */
void xbox_log_commit(void);                /* xboxrecomp kernel_thunks.c */

static char s_exe_dir[512] = SSX3NX_DEFAULT_DIR;
static const char *s_argv0 = "";

static const char *env2(const char *a, const char *b)
{
    const char *e = getenv(a);
    return (e && *e) ? e : getenv(b);
}

/* ── Arguments ──────────────────────────────────────────────────────── */

void nx_set_args(int argc, char **argv)
{
    char *slash;
    /* argv[0] is the .nro ("sdmc:/switch/ssx3nx/SSX3NX.nro") when the
     * Homebrew Menu gives one. */
    if (argc > 0 && argv[0] && argv[0][0]) {
        s_argv0 = argv[0];
        snprintf(s_exe_dir, sizeof s_exe_dir, "%s", argv[0]);
        for (slash = s_exe_dir; *slash; slash++)
            if (*slash == '\\') *slash = '/';
        slash = strrchr(s_exe_dir, '/');
        if (slash && slash > s_exe_dir && slash[-1] != ':')
            *slash = 0;
        else if (slash)
            slash[1] = 0;                          /* "sdmc:/" */
        else
            snprintf(s_exe_dir, sizeof s_exe_dir, "%s", SSX3NX_DEFAULT_DIR);
    }
}

const char *nx_exe_dir(void) { return s_exe_dir; }

static void exe_path(char *out, size_t n, const char *name)
{
    size_t l = strlen(s_exe_dir);
    snprintf(out, n, "%s%s%s", s_exe_dir, l && s_exe_dir[l - 1] == '/' ? "" : "/", name);
}

/* ── SSX 3 env.txt ──────────────────────────────────────────────────
 *
 * KEY=VALUE per line beside the .nro, '#' starts a comment. Each line becomes
 * an environment variable before anything reads its settings: RECOMP_GPU=sw,
 * SSX3NX_CPU_MHZ=1785 and the like, without rebuilding. */
void nx_env_load(void)
{
    char path[600], line[512];
    FILE *f;
    int n = 0;

    exe_path(path, sizeof path, "SSX 3 env.txt");
    f = fopen(path, "r");
    if (!f) {
        printf("[switch] no %s: default settings\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *k = line, *eq, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == '\r' || *k == '\n' || !*k) continue;
        if (!(eq = strchr(k, '='))) continue;
        *eq = '\0';
        for (e = eq; e > k && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = '\0';
        for (e = eq + 1; *e && *e != '\r' && *e != '\n'; e++) {}
        *e = '\0';
        if (!*k) continue;
        setenv(k, eq + 1, 1);
        printf("[switch] env %s=%s\n", k, eq + 1);
        n++;
    }
    fclose(f);
    printf("[switch] %d setting(s) from %s\n", n, path);
}

/* ── Clocks ─────────────────────────────────────────────────────────
 *
 * SSX3NX_CPU_MHZ / _GPU_MHZ / _MEM_MHZ (or OT_*): run the CPU, GPU and memory
 * at that clock (the closest the hardware lists, never above the request;
 * capped at 1785 / 921.6 / 1600 MHz). Unset: 1785 / 768 / 1600; 0 = the
 * system's. The translated game is CPU-bound, so the CPU clock matters most.
 *
 * The system puts its own clocks back on dock/undock, after sleep and when
 * the game comes back from HOME, so a thread applies them again every second
 * while the game has focus. When a clock manager runs (sys-clk, Horizon OC),
 * its profile rules and nothing is set here (SSX3NX_CLOCK_FORCE=1 overrides).
 * The old rates come back on exit. */
static struct {
    const char *env, *env_ot, *name;
    PcvModule mod;
    PcvModuleId id;
    u32 cap_hz, def_mhz, old_hz, set_hz;
    const char *env_dock;
    u32 dock_def_mhz;                      /* 0 = the handheld value */
    u32 hh_hz, dock_hz;                    /* targets (listed rates), 0 = not managed */
    int open;
    ClkrstSession s;
} s_clk[3] = {
    { "SSX3NX_CPU_MHZ", "OT_CPU_MHZ", "CPU",    PcvModule_CpuBus, PcvModuleId_CpuBus, 1785000000u, 1785, 0, 0,
      "SSX3NX_DOCK_CPU_MHZ", 0 },
    { "SSX3NX_GPU_MHZ", "OT_GPU_MHZ", "GPU",    PcvModule_GPU,    PcvModuleId_GPU,    921600000u,  768, 0, 0,
      "SSX3NX_DOCK_GPU_MHZ", 768 },
    { "SSX3NX_MEM_MHZ", "OT_MEM_MHZ", "memory", PcvModule_EMC,    PcvModuleId_EMC,    1600000000u, 1600, 0, 0,
      "SSX3NX_DOCK_MEM_MHZ", 1600 },
};
static int s_clk_docked = -1;              /* the mode the clocks are set for */
static int s_clk_rst = -1;                 /* 1 clkrst, 0 pcv, -1 not up */
static volatile int s_clk_stop;
static Thread s_clk_thread;
static int s_clk_thread_up;

static Result clk_get(int i, u32 *hz)
{
    return s_clk_rst ? clkrstGetClockRate(&s_clk[i].s, hz) : pcvGetClockRate(s_clk[i].mod, hz);
}

static Result clk_set(int i, u32 hz)
{
    return s_clk_rst ? clkrstSetClockRate(&s_clk[i].s, hz) : pcvSetClockRate(s_clk[i].mod, hz);
}

/* The highest listed rate not above want (want itself when there is no list). */
static u32 clk_pick(int i, u32 want)
{
    u32 rates[64], best = 0;
    s32 n = 0, k;
    PcvClockRatesListType type;
    Result rc = s_clk_rst
        ? clkrstGetPossibleClockRates(&s_clk[i].s, rates, 64, &type, &n)
        : pcvGetPossibleClockRates(s_clk[i].mod, rates, 64, &type, &n);
    if (R_FAILED(rc) || n <= 0 || type != PcvClockRatesListType_Discrete) return want;
    for (k = 0; k < n; k++)
        if (rates[k] <= want && rates[k] > best) best = rates[k];
    return best ? best : want;
}

/* Is another process serving this name? Registering it fails with "already
 * registered" when one is. */
static int clk_service_running(const char *name)
{
    Handle h;
    Result rc = smRegisterService(&h, smEncodeName(name), false, 1);
    if (R_SUCCEEDED(rc)) {
        smUnregisterService(smEncodeName(name));
        svcCloseHandle(h);
        return 0;
    }
    return rc == MAKERESULT(21, 4);
}

static void clk_restore(void)
{
    int i;
    if (s_clk_rst < 0) return;
    s_clk_stop = 1;
    if (s_clk_thread_up) {
        threadWaitForExit(&s_clk_thread);
        threadClose(&s_clk_thread);
        s_clk_thread_up = 0;
    }
    for (i = 0; i < 3; i++)
        if (s_clk[i].set_hz && s_clk[i].old_hz) clk_set(i, s_clk[i].old_hz);
    for (i = 0; i < 3; i++)
        if (s_clk[i].open) clkrstCloseSession(&s_clk[i].s);
    if (s_clk_rst) clkrstExit();
    else pcvExit();
    s_clk_rst = -1;
}

int nx_docked(void);

static void clk_keeper(void *arg)
{
    unsigned logged = 0;
    int i;
    u64 last = armGetSystemTick();
    (void)arg;
    while (!s_clk_stop) {
        svcSleepThread(1000000000LL);
        if (s_clk_stop || appletGetFocusState() != AppletFocusState_InFocus) continue;
        {
            /* back from sleep (a long gap): say what the clocks are now */
            u64 now = armGetSystemTick();
            if (armTicksToNs(now - last) > 10000000000ULL) {
                u32 hz[3] = { 0, 0, 0 };
                for (i = 0; i < 3; i++)
                    clk_get(i, &hz[i]);
                printf("[clock] back after %.0f s away: CPU %u, GPU %u, memory %u MHz\n",
                       (double)armTicksToNs(now - last) / 1e9, hz[0] / 1000000u,
                       hz[1] / 1000000u, hz[2] / 1000000u);
            }
            last = now;
        }
        {
            /* Docked / handheld: each its own clocks (SSX3NX_DOCK_*_MHZ).
             * On a change the new set is applied as it is, lower or higher;
             * otherwise a clock below its target is raised again (the system
             * resets clocks after sleep and HOME) and one above it -- a
             * system boost, another tool -- is left alone while it lasts. */
            int docked = nx_docked();
            if (docked != s_clk_docked) {
                s_clk_docked = docked;
                for (i = 0; i < 3; i++) {
                    u32 t = docked ? s_clk[i].dock_hz : s_clk[i].hh_hz, now = 0;
                    if (!t) continue;
                    clk_set(i, t);
                    s_clk[i].set_hz = t;
                    clk_get(i, &now);
                    printf("[clock] %s: %s %u MHz\n", docked ? "docked" : "handheld",
                           s_clk[i].name, now / 1000000u);
                }
                continue;
            }
        }
        for (i = 0; i < 3; i++) {
            u32 hz = 0, t = s_clk_docked ? s_clk[i].dock_hz : s_clk[i].hh_hz;
            if (!t || R_FAILED(clk_get(i, &hz)) || hz >= t) continue;
            clk_set(i, t);
            s_clk[i].set_hz = t;
            if (logged++ < 32)
                printf("[clock] %s was reset to %u MHz, back to %u MHz\n", s_clk[i].name,
                       hz / 1000000u, t / 1000000u);
        }
    }
}

int nx_docked(void)
{
    /* SSX3NX_DOCKED=1 / 0: act docked / handheld whatever the system says
     * (a Switch Lite can test the docked path), else the real mode */
    static int force = -2;
    if (force == -2) {
        const char *e = getenv("SSX3NX_DOCKED");
        force = (e && *e == '1') ? 1 : (e && *e == '0') ? 0 : -1;
        if (force >= 0)
            printf("[clock] SSX3NX_DOCKED=%d: %s mode forced\n", force, force ? "docked" : "handheld");
    }
    if (force >= 0)
        return force;
    return appletGetOperationMode() == AppletOperationMode_Console;
}

void nx_clocks(void)
{
    u32 want[3];
    int i, any = 0;
    for (i = 0; i < 3; i++) {
        const char *e = env2(s_clk[i].env, s_clk[i].env_ot);
        double mhz = (e && *e) ? atof(e) : s_clk[i].def_mhz;
        want[i] = mhz > 0 ? (u32)(mhz * 1e6 + 0.5) : 0;
        if (want[i] > s_clk[i].cap_hz) want[i] = s_clk[i].cap_hz;
        any |= want[i] != 0;
    }
    if (!any) {
        printf("[clock] SSX3NX_*_MHZ=0: the system's clocks\n");
        return;
    }
    {
        const char *f = env2("SSX3NX_CLOCK_FORCE", "OT_CLOCK_FORCE");
        const char *tool = clk_service_running("hoc:clk") ? "Horizon OC"
                         : clk_service_running("sys:clk") ? "sys-clk" : NULL;
        if (tool && !(f && *f == '1')) {
            /* Say what its profile gave us: with no profile for the title
             * that hosts the .nro (hbmenu runs inside another title), the
             * tool leaves the stock 1020 MHz CPU. */
            u32 hz[3] = { 0, 0, 0 };
            int up = 0;
            if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) {
                s_clk_rst = 1;
                for (i = 0; i < 3; i++)
                    if (R_SUCCEEDED(clkrstOpenSession(&s_clk[i].s, s_clk[i].id, 3))) {
                        clk_get(i, &hz[i]);
                        clkrstCloseSession(&s_clk[i].s);
                    }
                clkrstExit();
                up = 1;
            } else if (R_SUCCEEDED(pcvInitialize())) {
                s_clk_rst = 0;
                for (i = 0; i < 3; i++)
                    clk_get(i, &hz[i]);
                pcvExit();
                up = 1;
            }
            s_clk_rst = -1;
            printf("[clock] %s is running: clocks are left to its profile "
                   "(SSX3NX_CLOCK_FORCE=1 overrides)\n", tool);
            if (up)
                printf("[clock] now: CPU %u MHz, GPU %u MHz, memory %u MHz (asked %u / %u / %u)\n",
                       hz[0] / 1000000u, hz[1] / 1000000u, hz[2] / 1000000u,
                       want[0] / 1000000u, want[1] / 1000000u, want[2] / 1000000u);
            return;
        }
    }
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) s_clk_rst = 1;
    else if (R_SUCCEEDED(pcvInitialize())) s_clk_rst = 0;
    else {
        printf("[clock] neither clkrst nor pcv is available: clocks unchanged\n");
        return;
    }
    s_clk_docked = nx_docked();
    for (i = 0; i < 3; i++) {
        u32 hz, now = 0;
        Result rc;
        if (!want[i]) continue;
        if (s_clk_rst) {
            if (R_FAILED(rc = clkrstOpenSession(&s_clk[i].s, s_clk[i].id, 3))) {
                printf("[clock] %s: no clkrst session (0x%X)\n", s_clk[i].name, rc);
                continue;
            }
            s_clk[i].open = 1;
        }
        clk_get(i, &s_clk[i].old_hz);
        s_clk[i].hh_hz = clk_pick(i, want[i]);
        {
            /* docked: SSX3NX_DOCK_*_MHZ, default GPU 768 / memory 1600 (the
             * system's own docked clocks) and the handheld CPU */
            const char *d = getenv(s_clk[i].env_dock);
            double mhz = (d && *d) ? atof(d) : s_clk[i].dock_def_mhz;
            u32 dw = mhz > 0 ? (u32)(mhz * 1e6 + 0.5) : want[i];
            if (!(d && *d) && dw < want[i])     /* the default never lowers a handheld overclock */
                dw = want[i];
            if (dw > s_clk[i].cap_hz) dw = s_clk[i].cap_hz;
            s_clk[i].dock_hz = clk_pick(i, dw);
        }
        printf("[clock] %s targets: handheld %u MHz, docked %u MHz\n", s_clk[i].name,
               s_clk[i].hh_hz / 1000000u, s_clk[i].dock_hz / 1000000u);
        hz = s_clk_docked ? s_clk[i].dock_hz : s_clk[i].hh_hz;
        if (s_clk[i].old_hz >= hz) {
            /* Already there: still watched (set_hz), so a lower clock the
             * system puts back after sleep or HOME is raised again. Only a
             * clock already above the request is left entirely alone. */
            if (s_clk[i].old_hz == hz)
                s_clk[i].set_hz = hz;
            printf("[clock] %s already at %u MHz (asked %u): %s\n", s_clk[i].name,
                   s_clk[i].old_hz / 1000000u, want[i] / 1000000u,
                   s_clk[i].old_hz - hz < 2000000u ? "kept there"
                   : "left there while it lasts, raised again if it drops");
            continue;
        }
        rc = clk_set(i, hz);
        clk_get(i, &now);
        if (R_SUCCEEDED(rc)) s_clk[i].set_hz = now ? now : hz;
        printf("[clock] %s %u -> %u MHz (asked %u)%s\n", s_clk[i].name,
               s_clk[i].old_hz / 1000000u, now / 1000000u, want[i] / 1000000u,
               R_FAILED(rc) ? " -- refused" : "");
    }
    atexit(clk_restore);
    /* High priority: at the lowest, busy game threads on its core starve it. */
    if (R_SUCCEEDED(threadCreate(&s_clk_thread, clk_keeper, NULL, NULL, 0x4000, 0x2C, -2))
        && R_SUCCEEDED(threadStart(&s_clk_thread)))
        s_clk_thread_up = 1;
}

/* ── Display, message box ───────────────────────────────────────────── */

void nx_display_size(int *w, int *h)
{
    *w = 1920;
    *h = 1080;
    if (appletGetOperationMode() == AppletOperationMode_Handheld) { *w = 1280; *h = 720; }
}

/* A message the player has to read: a text screen until a button is pressed
 * (only meaningful before the game draws: the presenter owns the screen
 * after that). Called by xboxrecomp's MessageBoxA on the Switch. */
void xbox_switch_message_box(const char *text, const char *caption)
{
    PadState pad;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("\n  %s\n\n", caption ? caption : "SSX 3");
    printf("%s\n\n  Press + to close.\n", text ? text : "");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_A | HidNpadButton_B)) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

/* ── Log ────────────────────────────────────────────────────────────
 *
 * stdout and stderr both write to SSX3NX.log beside the .nro. A file on the
 * SD card only grows on the card when its data is committed, and a program
 * the system stops leaves nothing it has not committed: the first lines (the
 * start, where a port fails first) are written and committed one by one.
 * After that, lines collect in memory and a thread writes and commits them
 * four times a second, so the game never waits on the card. */
static int s_log_fd = -1;
static volatile unsigned s_log_writes;
#define LOG_SYNC_EACH 1500
#define LOG_BUF_SIZE (256 * 1024)
static char s_log_buf[LOG_BUF_SIZE];
static size_t s_log_len;
static Mutex s_log_mx;

static void log_flush(void)
{
    mutexLock(&s_log_mx);
    if (s_log_len && s_log_fd >= 0) write(s_log_fd, s_log_buf, s_log_len);
    s_log_len = 0;
    mutexUnlock(&s_log_mx);
}

static ssize_t log_write_r(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r; (void)fd;
    if (s_log_writes < LOG_SYNC_EACH) {
        ssize_t n = write(s_log_fd, ptr, len);
        if (s_log_writes++ < LOG_SYNC_EACH) fsync(s_log_fd);
        return n < 0 ? 0 : n;
    }
    mutexLock(&s_log_mx);
    if (s_log_len + len > LOG_BUF_SIZE) {      /* full: write it out here */
        if (s_log_len) write(s_log_fd, s_log_buf, s_log_len);
        s_log_len = 0;
    }
    if (len > LOG_BUF_SIZE) write(s_log_fd, ptr, len);
    else { memcpy(s_log_buf + s_log_len, ptr, len); s_log_len += len; }
    mutexUnlock(&s_log_mx);
    return (ssize_t)len;
}

/* Also the crash report's commit: if the crashed thread holds the lock, the
 * lines in memory are lost rather than the report waiting forever. */
static int log_fsync_r(struct _reent *r, void *fd)
{
    int tries;
    (void)r; (void)fd;
    for (tries = 0; tries < 50; tries++) {
        if (mutexTryLock(&s_log_mx)) {
            if (s_log_len && s_log_fd >= 0) write(s_log_fd, s_log_buf, s_log_len);
            s_log_len = 0;
            mutexUnlock(&s_log_mx);
            break;
        }
        svcSleepThread(1000000LL);
    }
    return s_log_fd >= 0 ? fsync(s_log_fd) : 0;
}

static const devoptab_t s_log_dev = { .name = "log", .write_r = log_write_r, .fsync_r = log_fsync_r };

static void on_exit_log(void)
{
    printf("[SSX3NX] exit\n");
    log_flush();
    if (s_log_fd >= 0) fsync(s_log_fd);
    xbox_log_commit();
}

/* libnx calls this first in __appExit -- on exit() and _exit() alike -- before
 * it unmounts the SD card. The game's threads are still running: let the file
 * calls in flight finish and stop new ones (kernel_bridge.c), then commit the
 * log (an _exit() skips on_exit_log, and its last lines were lost). */
void xbox_switch_fs_quiesce(unsigned max_ms);   /* xboxrecomp kernel_bridge.c */
static volatile int s_log_stop;
void userAppExit(void)
{
    int fd;
    xbox_switch_fs_quiesce(500);
    s_log_stop = 1;                     /* log_sync_thread: no more SD writes */
    xbox_log_commit();
    mutexLock(&s_log_mx);
    if (s_log_len && s_log_fd >= 0) write(s_log_fd, s_log_buf, s_log_len);
    s_log_len = 0;
    fd = s_log_fd;
    s_log_fd = -1;                      /* later lines: dropped, not a write to a gone device */
    mutexUnlock(&s_log_mx);
    if (fd >= 0) { fsync(fd); close(fd); }
}

/* Above the game's threads: a pthread gets the lowest priority, and with the
 * game keeping cores 0-2 busy it may never run. It sleeps nearly always. */
static void log_sync_thread(void *arg)
{
    (void)arg;
    for (;;) {
        svcSleepThread(250000000LL);
        if (s_log_stop)
            continue;
        log_flush();
        if (s_log_fd >= 0) fsync(s_log_fd);
        xbox_log_commit();
    }
}

/* An abort (a failed check, a stub, the C library giving up) would otherwise
 * just drop back to the Homebrew Menu: say so on screen first. */
static void on_abort(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[SSX3NX] stopped (abort)\n");
    fflush(stderr);
    fsync(STDERR_FILENO);
    xbox_switch_message_box("The game stopped with an error.\n\n"
                            "  SSX3NX.log, in the same folder as the game, says where.", "SSX 3");
    signal(SIGABRT, SIG_DFL);
    _exit(1);
}

/* ── Start ───────────────────────────────────────────────────────────── */

void nx_host_init(void)
{
    char path[600];

    /* Output to a PC when started with nxlink (nxlink -s); otherwise to
     * SSX3NX.log beside the game from the very start, unbuffered, so a run
     * that stops early still leaves a trace. */
    mutexInit(&s_log_mx);
    if (__nxlink_host.s_addr) {
        socketInitializeDefault();
        nxlinkStdio();
    } else {
        /* the previous run's log is kept as SSX3NX.prev.log: a crash is
         * usually followed by a boot that would overwrite its log */
        {
            char prev[600];
            exe_path(path, sizeof path, "SSX3NX.log");
            exe_path(prev, sizeof prev, "SSX3NX.prev.log");
            remove(prev);
            rename(path, prev);
        }
        s_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
        if (s_log_fd >= 0) {
            devoptab_list[STD_OUT] = &s_log_dev;
            devoptab_list[STD_ERR] = &s_log_dev;
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
        }
    }
    printf("SSX 3 (SSX3NX) starting: %s\n", s_argv0);
    nx_env_load();
    {
        static Thread t;
        if (R_SUCCEEDED(threadCreate(&t, log_sync_thread, NULL, NULL, 0x4000, 0x2A, 2)))
            threadStart(&t);
    }
    signal(SIGABRT, on_abort);
    atexit(on_exit_log);

    /* Started from the album, homebrew gets a few hundred MB at most: not
     * enough for the Xbox's memory plus the renderer's. */
    {
        AppletType t = appletGetAppletType();
        printf("[switch] applet type %d\n", (int)t);
        if (t != AppletType_Application && t != AppletType_SystemApplication) {
            xbox_switch_message_box("SSX 3 needs more memory than the album gives.\n\n"
                                    "  Start the Homebrew Menu over a game: hold R while starting any\n"
                                    "  installed game, then start SSX3NX from it.", "SSX 3");
            exit(1);
        }
    }
    /* No hardware TSO on the Cortex-A57 (PORTING_PLAN.md F10). */
    printf("[BOOT] hardware TSO unavailable (Cortex-A57); ordering relies on barriers\n");

    /* Defaults for the paths the game resolves (an env.txt line wins). */
    exe_path(path, sizeof path, "game");
    setenv("RECOMP_GAME_DIR", path, 0);
    setenv("RECOMP_SYSTEM_DIR", s_exe_dir, 0);
    exe_path(path, sizeof path, "tmp");
    mkdir(path, 0755);

    nx_clocks();
    xbox_switch_thread_core(0);         /* the game's thread */
    {
        extern void xbox_nx_thread_name(const char *name);   /* SSX3NX_CPU_STATS */
        xbox_nx_thread_name("GAME");
    }
    romfsInit();
}
#endif /* __SWITCH__ */
