/*
 * ps2_controls.c -- SSX3NX_PS2_CONTROLS=1: PS2-style riding controls with
 * Nintendo menu buttons.
 *
 * SSX 3 reads its controls from data\config\input.map on the disc: a list of
 * named actions, each an expression of pad buttons (Button14/15 "ShoulderL1/
 * R1" are the left/right triggers). The riding actions (JumpPressed,
 * BoostHeld, Trick1..15, Tweak, Handplant, ResetPath...) and the menu actions
 * (UINext = A, UIPrevious = B, MenuKey...) are separate entries, so the game
 * itself knows which applies: rewriting only the riding entries changes the
 * controls on the snow and nowhere else -- every menu, the pause menu over a
 * race included, keeps A = confirm and B = back, with no guessing at what is
 * on screen and no lag at a transition.
 *
 * The Xbox defaults (input.map, "Controller config 1"): A jump, X boost (and
 * wipeout recovery), grabs on X / Y / right trigger / B (the 15 grabs are
 * their combinations), tweak on the left trigger, handplant on black or
 * white, reset on Back.
 *
 * With SSX3NX_PS2_CONTROLS=1 the Switch input layer reads the face buttons by
 * their Nintendo labels (xinput_device.c), so here
 *   Xbox A = Switch A, B = B, X = X, Y = Y, black = L, white = R,
 *   left trigger = ZL, right trigger = ZR, Back = minus, LStick = L3
 * and the riding entries become, as on the PS2:
 *   Switch B        jump (hold to crouch)                    PS2 X
 *   Switch Y        boost / tweak (tweak: while grabbing),   PS2 Square
 *                   wipeout recovery
 *   Switch A        handplant (black / white are grabs now)  PS2 Circle
 *   L / R / ZL / ZR the four grab buttons                    PS2 L1 / R1 / L2 / R2
 *                   (the Xbox Y / right trigger / X / B grab slots; the
 *                   uber grinds likewise)
 *   Switch X, minus reset to the course
 * ZL / ZR still punch on the ground (AttackLeft/Right, unchanged). The two
 * debug chords (frame pause, free camera: L3 + black / white, i.e. L3 + a
 * grab now) are switched off.
 *
 * The disc is never written: the first time the game opens input.map, a
 * rewritten copy is saved next to the save data (input_ps2.map) and the path
 * layer serves that instead (g_xbox_path_redirect). If the file does not have
 * the entries expected, the original is served unchanged. Controller config 2
 * (input2.map, chosen in Options) stays the game's own.
 */
#include "log.h"
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/win32_compat.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
typedef WCHAR host_char;
#else
typedef char host_char;
#endif
extern void (*g_xbox_path_redirect)(const char *xbox_path, host_char *host_path_buf, DWORD buf_size);

static int s_state;                     /* 0 not built yet, 1 serving s_out, -1 original */
static char s_out[512];

int ssx3_Ps2Controls(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SSX3NX_PS2_CONTROLS");
        on = e && e[0] == '1';
    }
    return on;
}

/* The four grab buttons for the Xbox slots X, Y, right trigger, B (the order
 * of input.map's trick table): ZL, L, R, ZR. */
static const char *const k_grab[4] = {"ShoulderL1", "Black", "White", "ShoulderR1"};

/* Trick n's combination over (X, Y, RT, B), as input.map has it. */
static const unsigned char k_trick[15] = {
    0x8, 0x4, 0x2, 0x1,                 /* method, mute, stalefish, indy */
    0xA, 0xC, 0x9, 0x6, 0x5, 0x3,       /* nose, melancholy, stiffy, lein, swiss cheese, tail */
    0xE, 0xD, 0xB, 0x7,                 /* stalemasky, seatbelt, chicken salad, spaghetti */
    0xF                                 /* shifty */
};

#define NKEYS (15 + 4 + 10)
static char s_key[NKEYS][16];
static char s_val[NKEYS][160];

static void table_build(void)
{
    static const struct { const char *k, *v; } fixed[] = {
        {"JumpPressed", "B.pressed"},
        {"JumpHeld", "B"},
        {"BoostPressed", "Y.pressed"},
        {"BoostHeld", "Y"},
        {"WipeoutRecover", "Y.pressed"},
        {"Tweak", "Y"},
        {"Handplant", "A"},
        {"ResetPath", "X.pressed || Back.pressed"},
        {"PauseMode", "0"},
        {"CameraActivate", "0"},
    };
    /* uber grinds: B, Y, RT, LT alone -> ZR, L, R, ZL alone */
    static const char *const uber[4] = {"ShoulderR1", "Black", "White", "ShoulderL1"};
    int n = 0, i, b;
    for (i = 0; i < 15; i++, n++) {
        char *v = s_val[n];
        v[0] = 0;
        snprintf(s_key[n], sizeof s_key[n], "Trick%d", i + 1);
        for (b = 0; b < 4; b++) {
            int want = (k_trick[i] >> (3 - b)) & 1;
            snprintf(v + strlen(v), sizeof s_val[n] - strlen(v), "%s%s%s",
                     b ? " && " : "", want ? "" : "!", k_grab[b]);
        }
    }
    for (i = 0; i < 4; i++, n++) {
        char *v = s_val[n];
        v[0] = 0;
        snprintf(s_key[n], sizeof s_key[n], "UberGrind%d", i + 1);
        for (b = 0; b < 4; b++)
            snprintf(v + strlen(v), sizeof s_val[n] - strlen(v), "%s%s%s",
                     b ? " && " : "", b == i ? "" : "!", uber[b]);
    }
    for (i = 0; i < (int)(sizeof fixed / sizeof fixed[0]); i++, n++) {
        snprintf(s_key[n], sizeof s_key[n], "%s", fixed[i].k);
        snprintf(s_val[n], sizeof s_val[n], "%s", fixed[i].v);
    }
}

static FILE *open_host(const host_char *p, const char *mode)
{
#ifdef _WIN32
    wchar_t m[8];
    MultiByteToWideChar(CP_ACP, 0, mode, -1, m, 8);
    return _wfopen(p, m);
#else
    return fopen(p, mode);
#endif
}

/* Rewrite the riding entries of the original map (orig) into s_out. */
static int build(const host_char *orig)
{
    FILE *f = open_host(orig, "rb"), *o;
    char *src, *line, *next;
    long len;
    int found[NKEYS] = {0}, i, missing = 0;

    if (!f) {
        log_msg(LOG_WARN, "INPUT", "PS2 controls: input.map not readable, original controls");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 20) || !(src = (char *)malloc((size_t)len + 1))) {
        fclose(f);
        return 0;
    }
    len = (long)fread(src, 1, (size_t)len, f);
    fclose(f);
    src[len] = 0;
    table_build();

#if defined(__SWITCH__)
    snprintf(s_out, sizeof s_out, "sdmc:/switch/ssx3nx/input_ps2.map");
#else
    snprintf(s_out, sizeof s_out, "input_ps2.map");
#endif
    o = fopen(s_out, "wb");
    if (!o) {
        log_msg(LOG_WARN, "INPUT", "PS2 controls: cannot write %s, original controls", s_out);
        free(src);
        return 0;
    }
    for (line = src; line && *line; line = next) {
        char *eol = strchr(line, '\n'), *p = line, name[40];
        size_t k = 0;
        int crlf, hit = -1;
        next = eol ? eol + 1 : NULL;
        if (eol)
            *eol = 0;
        crlf = eol && eol > line && eol[-1] == '\r';
        while (*p == ' ' || *p == '\t')
            p++;
        while ((isalnum((unsigned char)*p) || *p == '_') && k + 1 < sizeof name)
            name[k++] = *p++;
        name[k] = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (k && *p == '=' && p[1] != '=')
            for (i = 0; i < NKEYS; i++)
                if (!strcmp(name, s_key[i])) {
                    hit = i;
                    break;
                }
        if (hit >= 0) {
            fprintf(o, "%s = %s; // SSX3NX_PS2_CONTROLS%s\n", s_key[hit], s_val[hit], crlf ? "\r" : "");
            found[hit] = 1;
        } else {
            fputs(line, o);
            if (eol)
                fputc('\n', o);
        }
    }
    fclose(o);
    free(src);
    for (i = 0; i < NKEYS; i++)
        if (!found[i]) {
            log_msg(LOG_WARN, "INPUT", "PS2 controls: input.map has no %s", s_key[i]);
            missing++;
        }
    if (missing) {
        log_msg(LOG_WARN, "INPUT", "PS2 controls: not the input.map expected, original controls");
        return 0;
    }
    log_msg(LOG_INFO, "INPUT", "PS2 controls on: jump B, boost/tweak Y, handplant A, grabs L/R/ZL/ZR, "
            "reset X (menus: A confirm, B back) -- %s", s_out);
    return 1;
}

static int ends_with_ci(const char *s, const char *tail)
{
    size_t n = strlen(s), m = strlen(tail), i;
    if (n < m)
        return 0;
    for (i = 0; i < m; i++) {
        char a = s[n - m + i], b = tail[i];
        if (a == '/') a = '\\';
        if (tolower((unsigned char)a) != tolower((unsigned char)b))
            return 0;
    }
    return 1;
}

extern int ssx3_ButtonIcons(void);
extern void ssx3_ButtonIconsRedirect(const char *xbox_path, host_char *host, DWORD n);

static void redirect(const char *xbox_path, host_char *host, DWORD n)
{
    ssx3_ButtonIconsRedirect(xbox_path, host, n);       /* button_icons.c: ui\*.xsh */
    if (!ssx3_Ps2Controls() || !xbox_path || !ends_with_ci(xbox_path, "\\config\\input.map"))
        return;
    if (s_state == 0)
        s_state = build(host) ? 1 : -1;
    if (s_state != 1)
        return;
#ifdef _WIN32
    MultiByteToWideChar(CP_ACP, 0, s_out, -1, host, (int)n);
#else
    snprintf(host, n, "%s", s_out);
#endif
}

void ssx3_Ps2ControlsInit(void)
{
    extern void ssx3_ButtonPromptsInit(void);
    if (ssx3_Ps2Controls() || ssx3_ButtonIcons())
        g_xbox_path_redirect = redirect;
    ssx3_ButtonPromptsInit();       /* button_icons.c: riding prompts, PS2 + icons only */
}
