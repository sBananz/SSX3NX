/*
 * SSX3NX guest profiler ([GPROF], SSX3NX_GUEST_PROF=1): where the game
 * thread's time goes in a race, by function, split by the game's update
 * (sub_000CC8F0) and render (sub_000D3880).
 *
 * A sampler thread pauses the GAME thread SSX3NX_GUEST_PROF_HZ times a second
 * (default 500) while in game, copies its context (svcGetThreadContext3) and
 * lets it run again -- only then does it look at anything (the paused thread
 * may hold the heap or stderr lock). Per sample:
 *   self       the host function the pc is in, named from SSX3NX.sym beside
 *              the .nro (nm output: address [size] type name, _start = 0):
 *              a lifted sub_XXXXXXXX(_gen), one of our overrides, or a runtime
 *              helper (memcpy, the kernel bridge, x87 helpers ...)
 *   inclusive  every lifted guest function with a return address on the
 *              guest stack (x25 = esp, XBOX_GUEST_REGS), plus the one the pc
 *              is in: a heuristic -- a stale word on the stack can add a
 *              caller that is no longer active -- but the callers that cost
 *              the most stand out
 * Every SSX3NX_GUEST_PROF_SEC seconds of race (default 10): one summary line
 * and the top 25 of each list with their update / render shares. Counts are
 * then cleared, so each block is that stretch of the race.
 *
 * Cost: a pause + context copy is some tens of microseconds; at 500 Hz about
 * 1-2% of the game thread. Switch only.
 */
#if defined(__SWITCH__)
#include <switch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int ssx3_InGame(void);
extern volatile int g_ssx3_prof_phase;          /* 0 other, 1 update, 2 render */
extern void *xbox_GuestToHost(uint32_t va);
extern void _start(void);

#define NPH 3

typedef struct { uint64_t addr, size; const char *name; uint32_t gva; } Sym;

static Sym *s_sym;                              /* text symbols, by address */
static int s_nsym;
static uint32_t (*s_self)[NPH];                 /* per symbol */
static uint32_t *s_gfunc;                       /* guest function VAs, sorted */
static int s_ngfunc;
static uint32_t (*s_incl)[NPH];                 /* per guest function */
static uint32_t s_glo, s_ghi;                   /* guest code range */
static Handle s_game;
static volatile int s_started;
/* The pushbuffer executor (thread "EXEC"), sampled alongside: host code only,
 * so just the function at its pc. */
extern uint32_t xbox_nx_thread_handle(const char *name);
static Handle s_exec;
static uint32_t *s_xself;                       /* per symbol */
static uint32_t s_xtotal;
/* EXEC call chains: (function at pc, its caller, the caller's caller), from
 * lr and the frame-record chain (x29), read while the thread is paused. */
#define XCH_N 4096
static struct { int32_t s, c1, c2; uint32_t n; } s_xch[XCH_N];

static void xch_add(int s, int c1, int c2)
{
    uint32_t h = ((uint32_t)s * 2654435761u ^ (uint32_t)c1 * 40503u ^ (uint32_t)c2 * 97u) & (XCH_N - 1);
    int k;
    for (k = 0; k < 64; k++, h = (h + 1) & (XCH_N - 1)) {
        if (s_xch[h].n && s_xch[h].s == s && s_xch[h].c1 == c1 && s_xch[h].c2 == c2) {
            s_xch[h].n++;
            return;
        }
        if (!s_xch[h].n) {
            s_xch[h].s = s; s_xch[h].c1 = c1; s_xch[h].c2 = c2; s_xch[h].n = 1;
            return;
        }
    }
}

/* Two more host threads that share the cores with GAME and EXEC, sampled the
 * same way (own time per function, top call chains): the kernel timer thread
 * (vblank ticks, software methods, timer DPCs -- guest code among them) and
 * the Vulkan present thread. [GPROF] T / TC and P / PC lines. */
#define OT_N 2
#define OCH_N 1024
static struct {
    const char *name, *tag;
    Handle h;
    uint32_t *self, total;
    struct { int32_t s, c1, c2; uint32_t n; } ch[OCH_N];
} s_ot[OT_N] = { { "TIMER", "T" }, { "PRESENT", "P" } };

static void och_add(int k, int s, int c1, int c2)
{
    uint32_t h = ((uint32_t)s * 2654435761u ^ (uint32_t)c1 * 40503u ^ (uint32_t)c2 * 97u) & (OCH_N - 1);
    int j;
    for (j = 0; j < 32; j++, h = (h + 1) & (OCH_N - 1)) {
        if (s_ot[k].ch[h].n && s_ot[k].ch[h].s == s && s_ot[k].ch[h].c1 == c1 && s_ot[k].ch[h].c2 == c2) {
            s_ot[k].ch[h].n++;
            return;
        }
        if (!s_ot[k].ch[h].n) {
            s_ot[k].ch[h].s = s; s_ot[k].ch[h].c1 = c1; s_ot[k].ch[h].c2 = c2; s_ot[k].ch[h].n = 1;
            return;
        }
    }
}

/* A frame record at fp inside the paused thread's stack: [fp] = previous fp,
 * [fp + 8] = return address. */
static int fp_ok(uint64_t fp, uint64_t sp)
{
    MemoryInfo mi;
    u32 pi;
    if (!fp || (fp & 7) || fp < sp || fp >= sp + 0x100000u)
        return 0;
    /* x29 need not be a frame pointer (-fomit-frame-pointer): only read
     * readable memory that holds the whole record */
    if (R_FAILED(svcQueryMemory(&mi, &pi, fp)) || !(mi.perm & Perm_R)
        || fp + 16 > mi.addr + mi.size)
        return 0;
    return 1;
}

static int sym_cmp(const void *a, const void *b)
{
    uint64_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* "sub_XXXXXXXX" or "sub_XXXXXXXX_gen" -> the guest VA, else 0 */
static uint32_t sub_va(const char *n)
{
    char *end;
    unsigned long v;
    if (strncmp(n, "sub_", 4) != 0)
        return 0;
    v = strtoul(n + 4, &end, 16);
    if (end != n + 12 || (*end && strcmp(end, "_gen") != 0))
        return 0;
    /* the XBE image (SSX 3's code ends near 0x0024xxxx); odd names outside
     * it (sub_301B1902 ...) are not lifted functions */
    if (v < 0x00010000ul || v >= 0x00400000ul)
        return 0;
    return (uint32_t)v;
}

static int load_syms(void)
{
    const char *dir = getenv("RECOMP_SYSTEM_DIR");
    char path[512];
    FILE *f;
    char line[512];
    int cap = 65536, i, ng = 0;
    snprintf(path, sizeof path, "%s/SSX3NX.sym", dir && *dir ? dir : "sdmc:/switch/ssx3nx");
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[GPROF] no %s: cannot name functions, profiler off\n", path);
        return 0;
    }
    s_sym = (Sym *)malloc(sizeof *s_sym * (size_t)cap);
    while (s_sym && fgets(line, sizeof line, f)) {
        char *p = line, *tok[4];
        int nt = 0;
        while (nt < 4) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p || *p == '\n' || *p == '\r') break;
            tok[nt++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
            if (*p) *p++ = 0;
        }
        if (nt < 3)
            continue;
        {
            int has_size = nt == 4;
            const char *type = tok[has_size ? 2 : 1], *name = tok[has_size ? 3 : 2];
            if (!(type[0] == 'T' || type[0] == 't' || type[0] == 'W' || type[0] == 'w') || type[1])
                continue;
            if (s_nsym == cap) {
                Sym *n2 = (Sym *)realloc(s_sym, sizeof *s_sym * (size_t)cap * 2);
                if (!n2) break;
                s_sym = n2; cap *= 2;
            }
            s_sym[s_nsym].addr = strtoull(tok[0], NULL, 16);
            s_sym[s_nsym].size = has_size ? strtoull(tok[1], NULL, 16) : 0;
            s_sym[s_nsym].name = strdup(name);
            s_sym[s_nsym].gva = sub_va(name);
            if (s_sym[s_nsym].gva) ng++;
            s_nsym++;
        }
    }
    fclose(f);
    if (!s_nsym)
        return 0;
    qsort(s_sym, (size_t)s_nsym, sizeof *s_sym, sym_cmp);
    s_gfunc = (uint32_t *)malloc(sizeof *s_gfunc * (size_t)(ng ? ng : 1));
    for (i = 0; s_gfunc && i < s_nsym; i++)
        if (s_sym[i].gva)
            s_gfunc[s_ngfunc++] = s_sym[i].gva;
    if (s_gfunc) {
        int j = 0;
        qsort(s_gfunc, (size_t)s_ngfunc, sizeof *s_gfunc, u32_cmp);
        for (i = 0; i < s_ngfunc; i++)          /* sub_X and sub_X_gen: once */
            if (!j || s_gfunc[j - 1] != s_gfunc[i])
                s_gfunc[j++] = s_gfunc[i];
        s_ngfunc = j;
    }
    s_self = calloc((size_t)s_nsym, sizeof *s_self);
    s_xself = calloc((size_t)s_nsym, sizeof *s_xself);
    {
        int k;
        for (k = 0; k < OT_N; k++)
            if (!(s_ot[k].self = calloc((size_t)s_nsym, sizeof *s_ot[k].self)))
                return 0;
    }
    s_incl = calloc((size_t)(s_ngfunc ? s_ngfunc : 1), sizeof *s_incl);
    if (!s_self || !s_xself || !s_incl || !s_ngfunc)
        return 0;
    s_glo = s_gfunc[0];
    s_ghi = s_gfunc[s_ngfunc - 1] + 0x4000u;
    fprintf(stderr, "[GPROF] %d symbols, %d guest functions (%08X..%08X) from %s\n",
            s_nsym, s_ngfunc, s_glo, s_ghi, path);
    return 1;
}

static int sym_at(uint64_t off)                 /* nearest at or below, or -1 */
{
    int lo = 0, hi = s_nsym - 1;
    if (!s_nsym || off < s_sym[0].addr)
        return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s_sym[mid].addr <= off) lo = mid; else hi = mid - 1;
    }
    /* several names at one address: prefer a sub_ one (it names guest code) */
    {
        int k = lo;
        while (k > 0 && s_sym[k - 1].addr == s_sym[lo].addr) {
            k--;
            if (s_sym[k].gva) return k;
        }
    }
    return lo;
}

static int gfunc_at(uint32_t va)                /* the guest function holding va */
{
    int lo = 0, hi = s_ngfunc - 1;
    if (va < s_glo || va >= s_ghi)
        return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s_gfunc[mid] <= va) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int gfunc_index(uint32_t va)             /* exact entry */
{
    int i = gfunc_at(va);
    return i >= 0 && s_gfunc[i] == va ? i : -1;
}

static void take(uint64_t pc_off, uint32_t esp, int ph)
{
    int si = sym_at(pc_off), seen[24], ns = 0, k;
    uint32_t i;
    if (si >= 0)
        s_self[si][ph]++;
    /* inclusive: the pc's own guest function, then return addresses */
    if (si >= 0 && s_sym[si].gva) {
        int g = gfunc_index(s_sym[si].gva);
        if (g >= 0) seen[ns++] = g;
    }
    if (esp >= 0x00010000u && esp < 0x03FFF000u) {
        const uint32_t *w = (const uint32_t *)xbox_GuestToHost(esp);
        uint32_t lim = (0x04000000u - esp) / 4u;
        if (lim > 384u) lim = 384u;
        for (i = 0; w && i < lim && ns < 24; i++) {
            int g = gfunc_at(w[i]);
            if (g < 0 || w[i] == s_gfunc[g])    /* a function's entry is not a return address */
                continue;
            for (k = 0; k < ns && seen[k] != g; k++)
                ;
            if (k == ns)
                seen[ns++] = g;
        }
    }
    for (k = 0; k < ns; k++)
        s_incl[seen[k]][ph]++;
}

typedef struct { int idx; uint32_t n; } Top;

static int top_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Top *)a)->n, y = ((const Top *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(double secs, uint32_t total, const uint32_t ph_total[NPH])
{
    static Top top[25];
    int i, nt, pass;
    if (!total)
        return;
    fprintf(stderr, "[GPROF] %.1f s of race, %u samples: update %.0f%%, render %.0f%%, other %.0f%%\n",
            secs, total, 100.0 * ph_total[1] / total, 100.0 * ph_total[2] / total, 100.0 * ph_total[0] / total);
    for (pass = 0; pass < 2; pass++) {
        int n = pass ? s_ngfunc : s_nsym;
        uint32_t (*cnt)[NPH] = pass ? s_incl : s_self;
        nt = 0;
        for (i = 0; i < n; i++) {
            uint32_t c = cnt[i][0] + cnt[i][1] + cnt[i][2];
            if (!c) continue;
            if (nt < 25) {
                top[nt].idx = i; top[nt].n = c; nt++;
                if (nt == 25) qsort(top, 25, sizeof *top, top_cmp);
            } else if (c > top[24].n) {
                int j = 24;
                while (j > 0 && top[j - 1].n < c) { top[j] = top[j - 1]; j--; }
                top[j].idx = i; top[j].n = c;
            }
        }
        if (nt < 25)
            qsort(top, (size_t)nt, sizeof *top, top_cmp);
        fprintf(stderr, "[GPROF] %s (%% of game-thread samples; U / R = in update / render):\n",
                pass ? "inclusive guest functions (stack heuristic)" : "self (host function at the pc)");
        for (i = 0; i < nt; i++) {
            const uint32_t *c = cnt[top[i].idx];
            char nm[64];
            if (pass)
                snprintf(nm, sizeof nm, "sub_%08X", s_gfunc[top[i].idx]);
            else
                snprintf(nm, sizeof nm, "%.63s", s_sym[top[i].idx].name);
            fprintf(stderr, "[GPROF]  %5.1f%% %-34s U %4.1f%%  R %4.1f%%  (%.2f ms/s)\n",
                    100.0 * top[i].n / total, nm, 100.0 * c[1] / total, 100.0 * c[2] / total,
                    1000.0 * top[i].n / total);    /* ms of each second of game-thread time */
        }
    }
    /* the executor thread */
    if (s_xtotal) {
        nt = 0;
        for (i = 0; i < s_nsym; i++) {
            uint32_t c = s_xself[i];
            if (!c) continue;
            if (nt < 25) {
                top[nt].idx = i; top[nt].n = c; nt++;
                if (nt == 25) qsort(top, 25, sizeof *top, top_cmp);
            } else if (c > top[24].n) {
                int j = 24;
                while (j > 0 && top[j - 1].n < c) { top[j] = top[j - 1]; j--; }
                top[j].idx = i; top[j].n = c;
            }
        }
        if (nt < 25)
            qsort(top, (size_t)nt, sizeof *top, top_cmp);
        fprintf(stderr, "[GPROF] EXEC thread, %u samples, self (host function at the pc, %% of EXEC samples):\n",
                s_xtotal);
        for (i = 0; i < nt; i++)
            fprintf(stderr, "[GPROF]  X %5.1f%% %.63s\n", 100.0 * top[i].n / s_xtotal, s_sym[top[i].idx].name);
        /* the top call chains */
        nt = 0;
        for (i = 0; i < XCH_N; i++) {
            uint32_t c = s_xch[i].n;
            if (!c) continue;
            if (nt < 25) {
                top[nt].idx = i; top[nt].n = c; nt++;
                if (nt == 25) qsort(top, 25, sizeof *top, top_cmp);
            } else if (c > top[24].n) {
                int j = 24;
                while (j > 0 && top[j - 1].n < c) { top[j] = top[j - 1]; j--; }
                top[j].idx = i; top[j].n = c;
            }
        }
        if (nt < 25)
            qsort(top, (size_t)nt, sizeof *top, top_cmp);
        fprintf(stderr, "[GPROF] EXEC call chains (function <- caller <- its caller):\n");
        for (i = 0; i < nt; i++) {
            int s = s_xch[top[i].idx].s, c1 = s_xch[top[i].idx].c1, c2 = s_xch[top[i].idx].c2;
            fprintf(stderr, "[GPROF]  XC %5.1f%% %.40s <- %.40s <- %.40s\n", 100.0 * top[i].n / s_xtotal,
                    s >= 0 ? s_sym[s].name : "?", c1 >= 0 ? s_sym[c1].name : "?", c2 >= 0 ? s_sym[c2].name : "?");
        }
        memset(s_xch, 0, sizeof s_xch);
    } else
        fprintf(stderr, "[GPROF] EXEC thread: not found (no thread named EXEC), not sampled\n");
    {
        int k;
        for (k = 0; k < OT_N; k++) {
            if (!s_ot[k].total) {
                fprintf(stderr, "[GPROF] %s thread: not found, not sampled\n", s_ot[k].name);
                continue;
            }
            nt = 0;
            for (i = 0; i < s_nsym; i++) {
                uint32_t c = s_ot[k].self[i];
                if (!c) continue;
                if (nt < 15) {
                    top[nt].idx = i; top[nt].n = c; nt++;
                    if (nt == 15) qsort(top, 15, sizeof *top, top_cmp);
                } else if (c > top[14].n) {
                    int j = 14;
                    while (j > 0 && top[j - 1].n < c) { top[j] = top[j - 1]; j--; }
                    top[j].idx = i; top[j].n = c;
                }
            }
            if (nt < 15)
                qsort(top, (size_t)nt, sizeof *top, top_cmp);
            fprintf(stderr, "[GPROF] %s thread, %u samples, self (%% of its samples, idle waits included):\n",
                    s_ot[k].name, s_ot[k].total);
            for (i = 0; i < nt; i++)
                fprintf(stderr, "[GPROF]  %s %5.1f%% %.63s\n", s_ot[k].tag,
                        100.0 * top[i].n / s_ot[k].total, s_sym[top[i].idx].name);
            nt = 0;
            for (i = 0; i < OCH_N; i++) {
                uint32_t c = s_ot[k].ch[i].n;
                if (!c) continue;
                if (nt < 12) {
                    top[nt].idx = i; top[nt].n = c; nt++;
                    if (nt == 12) qsort(top, 12, sizeof *top, top_cmp);
                } else if (c > top[11].n) {
                    int j = 11;
                    while (j > 0 && top[j - 1].n < c) { top[j] = top[j - 1]; j--; }
                    top[j].idx = i; top[j].n = c;
                }
            }
            if (nt < 12)
                qsort(top, (size_t)nt, sizeof *top, top_cmp);
            for (i = 0; i < nt; i++) {
                int s = s_ot[k].ch[top[i].idx].s, c1 = s_ot[k].ch[top[i].idx].c1, c2 = s_ot[k].ch[top[i].idx].c2;
                fprintf(stderr, "[GPROF]  %sC %5.1f%% %.40s <- %.40s <- %.40s\n", s_ot[k].tag,
                        100.0 * top[i].n / s_ot[k].total, s >= 0 ? s_sym[s].name : "?",
                        c1 >= 0 ? s_sym[c1].name : "?", c2 >= 0 ? s_sym[c2].name : "?");
            }
            memset(s_ot[k].ch, 0, sizeof s_ot[k].ch);
            memset(s_ot[k].self, 0, sizeof *s_ot[k].self * (size_t)s_nsym);
            s_ot[k].total = 0;
        }
    }
    fflush(stderr);
    memset(s_xself, 0, sizeof *s_xself * (size_t)s_nsym);
    s_xtotal = 0;
    memset(s_self, 0, sizeof *s_self * (size_t)s_nsym);
    memset(s_incl, 0, sizeof *s_incl * (size_t)s_ngfunc);
}

static void *sampler(void *unused)
{
    int hz = 500, sec = 10;
    uint64_t base = (uint64_t)(uintptr_t)&_start, freq = armGetSystemTickFreq();
    uint64_t t0 = 0;
    uint32_t total = 0, ph_total[NPH] = {0, 0, 0};
    const char *e;
    (void)unused;
    if ((e = getenv("SSX3NX_GUEST_PROF_HZ")) && atoi(e) >= 50 && atoi(e) <= 4000) hz = atoi(e);
    if ((e = getenv("SSX3NX_GUEST_PROF_SEC")) && atoi(e) >= 2 && atoi(e) <= 120) sec = atoi(e);
    if (!load_syms())
        return NULL;
    fprintf(stderr, "[GPROF] sampling the game thread at %d Hz in races, a report every %d s\n", hz, sec);
    fflush(stderr);
    for (;;) {
        ThreadContext ctx;
        Result rc;
        int ph;
        if (!ssx3_InGame()) {
            if (total >= 100u)
                report((double)(armGetSystemTick() - t0) / (double)freq, total, ph_total);
            total = 0; ph_total[0] = ph_total[1] = ph_total[2] = 0; t0 = 0;
            svcSleepThread(100000000ull);
            continue;
        }
        if (!t0)
            t0 = armGetSystemTick();
        ph = g_ssx3_prof_phase;
        if (ph < 0 || ph >= NPH) ph = 0;
        rc = svcSetThreadActivity(s_game, ThreadActivity_Paused);
        if (R_FAILED(rc)) {
            fprintf(stderr, "[GPROF] cannot pause the game thread (0x%X): profiler off\n", (unsigned)rc);
            return NULL;
        }
        rc = svcGetThreadContext3(&ctx, s_game);
        svcSetThreadActivity(s_game, ThreadActivity_Runnable);
        if (R_SUCCEEDED(rc)) {
            take(ctx.pc.x - base, (uint32_t)ctx.cpu_gprs[25].x, ph);
            total++;
            ph_total[ph]++;
        }
        if (!s_exec)
            s_exec = (Handle)xbox_nx_thread_handle("EXEC");
        if (s_exec && R_SUCCEEDED(svcSetThreadActivity(s_exec, ThreadActivity_Paused))) {
            uint64_t r1 = 0, r2 = 0;
            rc = svcGetThreadContext3(&ctx, s_exec);
            if (R_SUCCEEDED(rc)) {
                /* callers: lr unless it points back into the same function
                 * (a non-leaf past its first call), then the frame records */
                uint64_t sp = ctx.sp, fp = ctx.fp;
                int s0 = sym_at(ctx.pc.x - base), sl = sym_at(ctx.lr - base);
                if (sl >= 0 && sl != s0) {
                    r1 = ctx.lr;
                    if (fp_ok(fp, sp)) r2 = ((const uint64_t *)fp)[1];
                    if (r2 && sym_at(r2 - base) == sl && fp_ok(((const uint64_t *)fp)[0], sp))
                        r2 = ((const uint64_t *)((const uint64_t *)fp)[0])[1];
                } else if (fp_ok(fp, sp)) {
                    uint64_t pfp = ((const uint64_t *)fp)[0];
                    r1 = ((const uint64_t *)fp)[1];
                    if (fp_ok(pfp, sp)) r2 = ((const uint64_t *)pfp)[1];
                }
            }
            svcSetThreadActivity(s_exec, ThreadActivity_Runnable);
            if (R_SUCCEEDED(rc)) {
                int si = sym_at(ctx.pc.x - base);
                if (si >= 0) s_xself[si]++;
                s_xtotal++;
                xch_add(si, r1 ? sym_at(r1 - 4 - base) : -1, r2 ? sym_at(r2 - 4 - base) : -1);
            }
        }
        {
            int k;
            for (k = 0; k < OT_N; k++) {
                if (!s_ot[k].h)
                    s_ot[k].h = (Handle)xbox_nx_thread_handle(s_ot[k].name);
                if (s_ot[k].h && R_SUCCEEDED(svcSetThreadActivity(s_ot[k].h, ThreadActivity_Paused))) {
                    uint64_t r1 = 0, r2 = 0;
                    rc = svcGetThreadContext3(&ctx, s_ot[k].h);
                    if (R_SUCCEEDED(rc)) {
                        uint64_t sp = ctx.sp, fp = ctx.fp;
                        int s0 = sym_at(ctx.pc.x - base), sl = sym_at(ctx.lr - base);
                        if (sl >= 0 && sl != s0) {
                            r1 = ctx.lr;
                            if (fp_ok(fp, sp)) r2 = ((const uint64_t *)fp)[1];
                        } else if (fp_ok(fp, sp)) {
                            uint64_t pfp = ((const uint64_t *)fp)[0];
                            r1 = ((const uint64_t *)fp)[1];
                            if (fp_ok(pfp, sp)) r2 = ((const uint64_t *)pfp)[1];
                        }
                    }
                    svcSetThreadActivity(s_ot[k].h, ThreadActivity_Runnable);
                    if (R_SUCCEEDED(rc)) {
                        int si = sym_at(ctx.pc.x - base);
                        if (si >= 0) s_ot[k].self[si]++;
                        s_ot[k].total++;
                        och_add(k, si, r1 ? sym_at(r1 - 4 - base) : -1, r2 ? sym_at(r2 - 4 - base) : -1);
                    }
                }
            }
        }
        if (armGetSystemTick() - t0 >= (uint64_t)sec * freq) {
            report((double)(armGetSystemTick() - t0) / (double)freq, total, ph_total);
            total = 0; ph_total[0] = ph_total[1] = ph_total[2] = 0;
            t0 = armGetSystemTick();
        }
        svcSleepThread(1000000000ull / (uint64_t)hz);
    }
    return NULL;
}

/* Called on the game thread (sub_000CC8F0, every in-game update). */
void ssx3_GuestProfGameThread(void)
{
    static int on = -1;
    pthread_t th;
    if (on == 0 || s_started)
        return;
    if (on < 0) {
        const char *e = getenv("SSX3NX_GUEST_PROF");
        on = e && *e && *e != '0';
        if (!on)
            return;
    }
    s_started = 1;
    s_game = threadGetCurHandle();
    if (pthread_create(&th, NULL, sampler, NULL) == 0)
        pthread_detach(th);
}
#else
void ssx3_GuestProfGameThread(void) {}
#endif
