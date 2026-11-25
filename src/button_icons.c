/*
 * button_icons.c -- Nintendo Switch button prompts (SSX3NX_BUTTON_ICONS).
 *
 * SSX 3 draws its button prompts from one 256x256 DXT5 atlas, the shape
 * "art_xbox" in the UI shape files (data\ui\fe_1.xsh, ov_1.xsh, gl_1.xsh,
 * su_1.xsh -- the same layout in each): the Xbox Y / X / A / B, black and
 * white buttons, the triggers ("L" / "R" in grey circles, the menus' page
 * switch), Start / Back (play / rewind arrows), and the trigger letter tiles
 * of the text prompts (L / R, and G / D / I in French and Spanish), and the
 * controller picture of Options: Controller (a Pro Controller instead;
 * SSX3NX_PRO_CONTROLLER=0 keeps the Xbox one). The D-pad icon stays.
 *
 * Each of those icons is redrawn as the Switch button that presses it, so a
 * prompt names the button to press:
 *   face buttons  SSX3NX_PS2_CONTROLS=1 reads them by label, so Xbox A / B /
 *                 X / Y are Switch A / B / X / Y; otherwise by position
 *                 (Xbox A = Switch B, B = A, X = Y, Y = X)
 *   black, white  L, R           triggers  ZL, ZR
 *   Start, Back   +, -
 * With SSX3NX_PS2_CONTROLS=1 the prompts that name a riding action (wipeout
 * recovery, uber tricks, the Ubertrick screen) are moved to the PS2 buttons
 * as well: see "Riding prompts" below. A hint the game writes out in words
 * ("press the jump button") stays as its text says.
 *
 * The icons are drawn here, from strokes (no image data from anywhere), in
 * the style of the Switch's own prompts: dark round face buttons, rounded
 * rectangles for the shoulder buttons, white letters. The new pixels are
 * encoded to DXT5 over exactly the 4x4 blocks those icons occupy, whose other
 * pixels are transparent.
 *
 * The disc is never written: the first time the game opens one of those
 * files, a patched copy is saved next to the save data (icons\<name>.xsh)
 * and the path layer serves that instead (g_xbox_path_redirect, via
 * ps2_controls.c). A file without the expected atlas is served unchanged.
 */
#include "log.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include "platform/win32_compat.h"
#include <sys/stat.h>
#define mkdir_one(p) mkdir(p, 0755)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>

#ifdef _WIN32
typedef WCHAR host_char;
#else
typedef char host_char;
#endif

extern int ssx3_Ps2Controls(void);

int ssx3_ButtonIcons(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SSX3NX_BUTTON_ICONS");
#if defined(__SWITCH__)
        on = !(e && e[0] == '0');
#else
        on = e && e[0] == '1';
#endif
    }
    return on;
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

#define ATLAS 256

typedef struct { float x0, y0, x1, y1; } Seg;

/* Letters as strokes in a unit box (y down). Arcs are polylines. */
#define MAXSEG 40
typedef struct { Seg s[MAXSEG]; int n; float aspect; } Glyph;

static void g_line(Glyph *g, float x0, float y0, float x1, float y1)
{
    if (g->n < MAXSEG) {
        Seg *s = &g->s[g->n++];
        s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1;
    }
}

/* A half-round bowl to the right: from (x, cy - r) round to (x, cy + r). */
static void g_bowl(Glyph *g, float x, float cy, float r, float xr)
{
    int i, n = 10;
    float px = x, py = cy - r;
    g_line(g, x, cy - r, xr, cy - r);
    px = xr;
    for (i = 1; i <= n; i++) {
        float a = -1.5707963f + 3.1415927f * i / n;
        float qx = xr + r * cosf(a), qy = cy + r * sinf(a);
        g_line(g, px, py, qx, qy);
        px = qx; py = qy;
    }
    g_line(g, px, py, x, cy + r);
}

static void glyph_make(Glyph *g, char c)
{
    memset(g, 0, sizeof *g);
    g->aspect = 0.78f;
    switch (c) {
    case 'A':
        g_line(g, 0.0f, 1.0f, 0.5f, 0.0f);
        g_line(g, 0.5f, 0.0f, 1.0f, 1.0f);
        g_line(g, 0.2f, 0.66f, 0.8f, 0.66f);
        g->aspect = 0.86f;
        break;
    case 'B':
        g_line(g, 0.0f, 0.0f, 0.0f, 1.0f);
        g_bowl(g, 0.0f, 0.235f, 0.235f, 0.58f);
        g_bowl(g, 0.0f, 0.74f, 0.26f, 0.64f);
        break;
    case 'X':
        g_line(g, 0.0f, 0.0f, 1.0f, 1.0f);
        g_line(g, 1.0f, 0.0f, 0.0f, 1.0f);
        g->aspect = 0.84f;
        break;
    case 'Y':
        g_line(g, 0.0f, 0.0f, 0.5f, 0.52f);
        g_line(g, 1.0f, 0.0f, 0.5f, 0.52f);
        g_line(g, 0.5f, 0.52f, 0.5f, 1.0f);
        g->aspect = 0.86f;
        break;
    case 'L':
        g_line(g, 0.0f, 0.0f, 0.0f, 1.0f);
        g_line(g, 0.0f, 1.0f, 0.95f, 1.0f);
        g->aspect = 0.66f;
        break;
    case 'R':
        g_line(g, 0.0f, 0.0f, 0.0f, 1.0f);
        g_bowl(g, 0.0f, 0.26f, 0.26f, 0.6f);
        g_line(g, 0.45f, 0.52f, 1.0f, 1.0f);
        break;
    case 'Z':
        g_line(g, 0.0f, 0.0f, 1.0f, 0.0f);
        g_line(g, 1.0f, 0.0f, 0.0f, 1.0f);
        g_line(g, 0.0f, 1.0f, 1.0f, 1.0f);
        break;
    case '+':
        g_line(g, 0.5f, 0.0f, 0.5f, 1.0f);
        g_line(g, 0.0f, 0.5f, 1.0f, 0.5f);
        g->aspect = 1.0f;
        break;
    case '-':
        g_line(g, 0.0f, 0.5f, 1.0f, 0.5f);
        g->aspect = 1.0f;
        break;
    }
}

static float seg_dist(float px, float py, const Seg *s)
{
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    float l2 = dx * dx + dy * dy, t = 0.0f;
    if (l2 > 0.0f) {
        t = ((px - s->x0) * dx + (py - s->y0) * dy) / l2;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    }
    dx = s->x0 + t * dx - px;
    dy = s->y0 + t * dy - py;
    return sqrtf(dx * dx + dy * dy);
}

/* One line of text placed in pixels: glyphs, their boxes and stroke width. */
typedef struct {
    Glyph g[2];
    float gx[2], gy, gw[2], gh;
    int n;
    float stroke;                       /* in pixels */
} Text;

static void text_make(Text *t, const char *s, float cx, float cy, float h, float stroke)
{
    float w = 0.0f, gap = h * 0.16f, x;
    int i;
    memset(t, 0, sizeof *t);
    t->n = (int)strlen(s) > 2 ? 2 : (int)strlen(s);
    t->gh = h;
    t->gy = cy - h * 0.5f;
    t->stroke = stroke;
    for (i = 0; i < t->n; i++) {
        glyph_make(&t->g[i], s[i]);
        t->gw[i] = h * t->g[i].aspect;
        w += t->gw[i] + (i ? gap : 0.0f);
    }
    x = cx - w * 0.5f;
    for (i = 0; i < t->n; i++) {
        t->gx[i] = x;
        x += t->gw[i] + gap;
    }
}

static int text_hit(const Text *t, float px, float py)
{
    int i, k;
    float half = t->stroke * 0.5f;
    for (i = 0; i < t->n; i++) {
        /* strokes sit inside the box: the box is the outline of the ink */
        float ix = t->gx[i] + half, iy = t->gy + half;
        float iw = t->gw[i] - t->stroke, ih = t->gh - t->stroke;
        if (iw < 0.01f) iw = 0.01f;
        if (ih < 0.01f) ih = 0.01f;
        for (k = 0; k < t->g[i].n; k++) {
            const Seg *s = &t->g[i].s[k];
            /* distance in pixels: scale the segment, not the point */
            Seg p = { ix + s->x0 * iw, iy + s->y0 * ih, ix + s->x1 * iw, iy + s->y1 * ih };
            if (seg_dist(px, py, &p) <= half)
                return 1;
        }
    }
    return 0;
}

enum { SHAPE_CIRCLE, SHAPE_PILL };

typedef struct {
    int x0, y0, x1, y1;                 /* the original icon's box, inclusive */
    int shape;
    const char *label;                  /* set at run time */
    int trim_r;                         /* drawn this many pixels narrower (right edge) */
} Icon;

/* Signed distance to the icon's outline (negative inside), in pixels. */
static float shape_sd(const Icon *ic, float px, float py)
{
    float cx = (ic->x0 + ic->x1 + 1) * 0.5f, cy = (ic->y0 + ic->y1 + 1) * 0.5f;
    float hw = (ic->x1 - ic->x0 + 1) * 0.5f, hh = (ic->y1 - ic->y0 + 1) * 0.5f;
    if (ic->shape == SHAPE_CIRCLE) {
        float r = hw < hh ? hw : hh;
        return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - r;
    } else {
        /* rounded rectangle, the Switch's shoulder-button prompt */
        float rr = (hw < hh ? hw : hh) * 0.55f;
        float qx = fabsf(px - cx) - (hw - rr), qy = fabsf(py - cy) - (hh * 0.86f - rr);
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float in = qx > qy ? qx : qy;
        return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rr;
    }
}

/* Draw one icon into the RGBA atlas region (rgba: ATLAS x ATLAS, R,G,B,A). */
static void icon_draw(uint8_t *rgba, const Icon *ic, int bx0, int by0, int bx1, int by1)
{
    const int SS = 4;
    float w = (float)(ic->x1 - ic->x0 + 1), h = (float)(ic->y1 - ic->y0 + 1);
    float m = w < h ? w : h;
    int two = strlen(ic->label) > 1;
    float th = ic->shape == SHAPE_PILL ? h * 0.86f * (two ? 0.56f : 0.66f)
                                       : m * (two ? 0.40f : 0.52f);
    float stroke = th * (m < 20.0f ? 0.25f : 0.21f);
    float rim = m < 20.0f ? 1.1f : 1.6f;
    Text t;
    int x, y;
    if (stroke < 1.6f) stroke = 1.6f;
    text_make(&t, ic->label, (ic->x0 + ic->x1 + 1) * 0.5f, (ic->y0 + ic->y1 + 1) * 0.5f, th, stroke);
    for (y = by0; y <= by1; y++)
        for (x = bx0; x <= bx1; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            int sx, sy;
            for (sy = 0; sy < SS; sy++)
                for (sx = 0; sx < SS; sx++) {
                    float px = x + (sx + 0.5f) / SS, py = y + (sy + 0.5f) / SS;
                    float d = shape_sd(ic, px, py);
                    float cr, cg, cb;
                    if (d > 0.0f)
                        continue;
                    if (text_hit(&t, px, py)) {
                        cr = cg = cb = 250.0f;
                    } else if (d > -rim) {
                        cr = cg = cb = 118.0f;          /* light rim */
                    } else {
                        /* body: dark grey, a little lighter at the top */
                        float k = (py - ic->y0) / h;
                        cr = cg = cb = 74.0f - 34.0f * k;
                    }
                    r += cr; g += cg; b += cb; a += 1.0f;
                }
            {
                uint8_t *o = rgba + ((size_t)y * ATLAS + x) * 4;
                float n = (float)(SS * SS);
                if (a > 0) {
                    o[0] = (uint8_t)(r / a + 0.5f);
                    o[1] = (uint8_t)(g / a + 0.5f);
                    o[2] = (uint8_t)(b / a + 0.5f);
                } else {
                    o[0] = o[1] = o[2] = 0;
                }
                o[3] = (uint8_t)(255.0f * a / n + 0.5f);
            }
        }
}

/* ── The controller picture (Options: Controller) ─────────────────────────
 *
 * The Xbox controller at the top left of the atlas becomes a Switch Pro
 * Controller, drawn here from shapes. The screen's callout lines point at
 * fixed places in the picture; the Pro Controller has the Xbox pad's layout
 * (left stick high, D-pad and right stick low and inward, face buttons high
 * right), so each of its parts is drawn where the Xbox one was: the callouts
 * still land on a stick, the D-pad or the face buttons. The face letters are
 * the real controller's (X top, Y left, A right, B bottom) in either mode:
 * this is the physical pad, the bottom one (B) jumps with PS2 controls,
 * where the Xbox picture's A sat beside "Jump". */

#define PRO_X1 219                      /* the picture's region: 0..PRO_X1, 0..PRO_Y1 */
#define PRO_Y1 163                      /* the ring icon below starts at y 165 */

static float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static float sd_capsule(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    Seg s = { x0, y0, x1, y1 };
    return seg_dist(px, py, &s) - r;
}

static float smin(float a, float b, float k)
{
    float h = k - fabsf(a - b);
    h = h > 0 ? h / k : 0;
    return (a < b ? a : b) - h * h * k * 0.25f;
}

typedef struct { float x, y; const char *l; } ProBtn;
static const ProBtn k_face[4] = {
    { 162.0f, 32.5f, "X" }, { 146.5f, 48.0f, "Y" }, { 177.5f, 48.0f, "A" }, { 162.0f, 63.5f, "B" },
};
static Text s_pro_txt[8];               /* X Y A B, -, +, L, R */

static float pro_body(float px, float py)
{
    /* a tight outline around the controls (which stay where the callouts
     * point): smaller than the Xbox pad's, which filled the screen's box */
    float d = sd_rrect(px, py, 109.5f, 52.0f, 86.0f, 37.0f, 22.0f);
    d = smin(d, sd_capsule(px, py, 54.0f, 70.0f, 44.0f, 118.0f, 21.0f), 12.0f);
    d = smin(d, sd_capsule(px, py, 165.0f, 70.0f, 175.0f, 118.0f, 21.0f), 12.0f);
    return d;
}

/* Colour (grey level: a near-black pad with crisp white edges, high contrast against the screen's pale blue) at a
 * point; returns 0 where it is transparent. */
static int pro_sample(float px, float py, float *c)
{
    float d = pro_body(px, py), r, dx, dy;
    int i;
    for (i = 0; i < 8; i++)                         /* letters above everything */
        if (text_hit(&s_pro_txt[i], px, py)) {
            if (i >= 6 && d < -1.0f) break;        /* L / R: only over the bumpers */
            *c = 255.0f;
            return 1;
        }
    if (d > 0.0f) {
        /* the shoulder buttons, behind the top edge */
        float b = sd_capsule(px, py, 36.0f, 14.0f, 82.0f, 11.0f, 6.5f);
        float b2 = sd_capsule(px, py, 137.0f, 11.0f, 183.0f, 14.0f, 6.5f);
        if (b2 < b) b = b2;
        if (b > 0.0f)
            return 0;
        *c = b > -1.4f ? 225.0f : 46.0f - 1.0f * (py - 1.0f);   /* crisp white edge, dark button */
        return 1;
    }
    if (d > -1.9f) { *c = 236.0f; return 1; }       /* crisp white rim */
    *c = 34.0f - 12.0f * (py / (float)PRO_Y1);      /* near-black body, darker toward the grips */
    if (d > -4.0f && py < 40.0f) *c += 14.0f;       /* a lit top edge */

    /* D-pad: a cross on a round base */
    dx = px - 75.0f; dy = py - 83.0f; r = sqrtf(dx * dx + dy * dy);
    if (r < 16.6f) {
        float ax = fabsf(dx), ay = fabsf(dy);
        float arm = (ax < 5.2f && ay < 13.5f) || (ay < 5.2f && ax < 13.5f) ? 1.0f : 0.0f;
        *c = r > 15.3f ? 210.0f : 16.0f;
        if (arm) {
            float e = ax < 5.2f && ay < 13.5f ? fminf(5.2f - ax, 13.5f - ay) : 99.0f;
            float e2 = ay < 5.2f && ax < 13.5f ? fminf(5.2f - ay, 13.5f - ax) : 99.0f;
            *c = fminf(e, e2) < 1.1f && fmaxf(ax, ay) > 5.2f ? 200.0f : 8.0f;
            if (r < 2.7f) *c = 4.0f;
            /* arrows on the arms */
            if ((ay < 2.0f - (ax - 8.1f) * 0.7f && ax > 8.1f && ax < 11.0f && ay < 2.0f)
                || (ax < 2.0f - (ay - 8.1f) * 0.7f && ay > 8.1f && ay < 11.0f && ax < 2.0f))
                *c = 240.0f;
        }
        return 1;
    }
    /* the sticks: a dark well, a domed cap with a ridge */
    for (i = 0; i < 2; i++) {
        float sx = i ? 133.0f : 48.0f, sy = i ? 83.0f : 48.0f;
        dx = px - sx; dy = py - sy; r = sqrtf(dx * dx + dy * dy);
        if (r < 17.6f) {
            if (r > 16.2f)      *c = 228.0f;
            else if (r > 12.6f) *c = 6.0f;
            else if (r > 11.4f) *c = 196.0f;
            else {
                *c = 52.0f - 26.0f * (r / 11.7f) + 16.0f * (-(dx + dy) / 23.4f);
                if (fabsf(r - 8.5f) < 0.6f) *c = 150.0f;
            }
            return 1;
        }
    }
    /* face buttons */
    for (i = 0; i < 4; i++) {
        dx = px - k_face[i].x; dy = py - k_face[i].y; r = sqrtf(dx * dx + dy * dy);
        if (r < 7.9f) { *c = r > 6.7f ? 236.0f : 8.0f; return 1; }
    }
    /* minus, plus */
    for (i = 0; i < 2; i++) {
        dx = px - (i ? 130.0f : 89.0f); dy = py - 27.0f; r = sqrtf(dx * dx + dy * dy);
        if (r < 5.6f) { *c = r > 4.5f ? 220.0f : 8.0f; return 1; }
    }
    /* Home: a ring; Capture: a square with a dot */
    dx = px - 120.0f; dy = py - 52.0f; r = sqrtf(dx * dx + dy * dy);
    if (r < 6.2f) { *c = r > 5.0f ? 210.0f : r > 3.9f ? 8.0f : 236.0f; return 1; }
    dx = px - 99.0f; dy = py - 52.0f;
    if (fabsf(dx) < 5.0f && fabsf(dy) < 5.0f) {
        *c = fmaxf(fabsf(dx), fabsf(dy)) > 3.9f ? 210.0f : sqrtf(dx * dx + dy * dy) < 2.2f ? 236.0f : 8.0f;
        return 1;
    }
    return 1;
}

static int pro_on(void)
{
    const char *e = getenv("SSX3NX_PRO_CONTROLLER");
    return !(e && e[0] == '0');
}

#define PRO_W (PRO_X1 + 1)              /* 220 x 164 pixels */
#define PRO_H (PRO_Y1 + 1)

/* Render the picture at `scale` x its size into rgba (row stride `stride`
 * pixels): scale 1 is what the game shows; the export template is drawn
 * larger from the same shapes, so it is sharp to edit. */
static void pro_render(uint8_t *rgba, int stride, int scale)
{
    const int SS = 4;
    int i, x, y;
    for (i = 0; i < 4; i++)
        text_make(&s_pro_txt[i], k_face[i].l, k_face[i].x, k_face[i].y, 8.2f, 1.7f);
    text_make(&s_pro_txt[4], "-", 89.0f, 27.0f, 5.6f, 1.5f);
    text_make(&s_pro_txt[5], "+", 130.0f, 27.0f, 5.6f, 1.5f);
    text_make(&s_pro_txt[6], "L", 58.0f, 9.5f, 6.5f, 1.5f);
    text_make(&s_pro_txt[7], "R", 161.0f, 9.5f, 6.5f, 1.5f);
    for (y = 0; y < PRO_H * scale; y++)
        for (x = 0; x < PRO_W * scale; x++) {
            float sum = 0, a = 0;
            int sx, sy;
            uint8_t *o = rgba + ((size_t)y * stride + x) * 4;
            for (sy = 0; sy < SS; sy++)
                for (sx = 0; sx < SS; sx++) {
                    float c;
                    if (pro_sample((x + (sx + 0.5f) / SS) / scale, (y + (sy + 0.5f) / SS) / scale, &c)) {
                        sum += c;
                        a += 1.0f;
                    }
                }
            o[0] = o[1] = o[2] = a > 0 ? (uint8_t)(sum / a + 0.5f) : 0;
            o[3] = (uint8_t)(255.0f * a / (SS * SS) + 0.5f);
        }
}

/* Your own picture: icons/pro_controller.png next to the save data
 * (sdmc:/switch/ssx3nx/icons/pro_controller.png on the Switch), any PNG
 * with transparency. Shown at 220 x 164; a larger image (the 880 x 656
 * template, say) is averaged down to that, so draw at whatever size suits
 * as long as the shape is the same. The screen's callout lines point at
 * fixed places, so keep the sticks, D-pad and face buttons where they are
 * in the template. Read each boot; delete it to get the drawn one back. */
#if defined(__SWITCH__) || defined(SSX3_HAVE_PNG)
#include <png.h>
#define PRO_USER_PNG 1
#endif

/* A user picture icons/<name> next to the save data, area-averaged to w x h
 * into rgba (row stride `stride` pixels), colour weighted by alpha (no dark
 * fringes). 0 when there is none (or it cannot be read). */
static int user_png(const char *name, uint8_t *rgba, int stride, int W, int Hh)
{
#ifdef PRO_USER_PNG
    char path[512];
    png_image im;
    uint8_t *src;
    int x, y;
#if defined(__SWITCH__)
    snprintf(path, sizeof path, "sdmc:/switch/ssx3nx/icons/%s", name);
#else
    snprintf(path, sizeof path, "icons/%s", name);
#endif
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path))
        return 0;                       /* none there */
    im.format = PNG_FORMAT_RGBA;
    if (im.width < 8 || im.height < 8 || im.width > 8192 || im.height > 8192
        || !(src = (uint8_t *)malloc(PNG_IMAGE_SIZE(im)))) {
        png_image_free(&im);
        log_msg(LOG_WARN, "ICONS", "%s: unusable image size %ux%u", path, im.width, im.height);
        return 0;
    }
    if (!png_image_finish_read(&im, NULL, src, 0, NULL)) {
        log_msg(LOG_WARN, "ICONS", "%s: cannot read it (%s)", path, im.message);
        free(src);
        return 0;
    }
    for (y = 0; y < Hh; y++)
        for (x = 0; x < W; x++) {
            double fx0 = (double)x * im.width / W, fx1 = (double)(x + 1) * im.width / W;
            double fy0 = (double)y * im.height / Hh, fy1 = (double)(y + 1) * im.height / Hh;
            double r = 0, g = 0, b = 0, a = 0, wsum = 0;
            int sx, sy;
            uint8_t *o = rgba + ((size_t)y * stride + x) * 4;
            for (sy = (int)fy0; sy < (int)ceil(fy1) && sy < (int)im.height; sy++)
                for (sx = (int)fx0; sx < (int)ceil(fx1) && sx < (int)im.width; sx++) {
                    double wx = fmin(sx + 1.0, fx1) - fmax((double)sx, fx0);
                    double wy = fmin(sy + 1.0, fy1) - fmax((double)sy, fy0);
                    const uint8_t *s = src + ((size_t)sy * im.width + sx) * 4;
                    double w = wx * wy, wa = w * s[3];
                    r += wa * s[0]; g += wa * s[1]; b += wa * s[2]; a += wa; wsum += w;
                }
            o[0] = a > 0 ? (uint8_t)(r / a + 0.5) : 0;
            o[1] = a > 0 ? (uint8_t)(g / a + 0.5) : 0;
            o[2] = a > 0 ? (uint8_t)(b / a + 0.5) : 0;
            o[3] = wsum > 0 ? (uint8_t)(a / wsum + 0.5) : 0;
        }
    free(src);
    log_msg(LOG_INFO, "ICONS", "picture from %s (%ux%u, shown at %dx%d)", path, im.width, im.height, W, Hh);
    return 1;
#else
    (void)name; (void)rgba; (void)stride; (void)W; (void)Hh;
    return 0;
#endif
}

static int pro_user(uint8_t *rgba)
{
    return user_png("pro_controller.png", rgba, ATLAS, PRO_W, PRO_H);
}
/* Draw the picture into the RGBA atlas (region 0..PRO_X1 x 0..PRO_Y1). */
static void pro_draw(uint8_t *rgba)
{
    if (!pro_user(rgba))
        pro_render(rgba, ATLAS, 1);
}

/* ── DXT5 ──────────────────────────────────────────────────────────────── */

static uint16_t to565(const float c[3])
{
    int r = (int)(c[0] * 31.0f / 255.0f + 0.5f), g = (int)(c[1] * 63.0f / 255.0f + 0.5f),
        b = (int)(c[2] * 31.0f / 255.0f + 0.5f);
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 63 ? 63 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void from565(uint16_t c, int o[3])
{
    o[0] = ((c >> 11) & 31) * 255 / 31;
    o[1] = ((c >> 5) & 63) * 255 / 63;
    o[2] = (c & 31) * 255 / 31;
}

/* px: 16 pixels R,G,B,A. out: 16 bytes. */
static void dxt5_block(const uint8_t px[16][4], uint8_t out[16])
{
    int i, amin = 255, amax = 0;
    uint64_t abits = 0;
    float mean[3] = {0, 0, 0}, axis[3] = {1, 1, 1}, cov[6] = {0};
    float tmin = 1e9f, tmax = -1e9f, wsum = 0;
    float lo[3], hi[3];
    uint16_t c0, c1;
    int p0[3], p1[3], pal[4][3];
    uint32_t cbits = 0;

    /* alpha: 8 levels between the block's extremes */
    for (i = 0; i < 16; i++) {
        if (px[i][3] < amin) amin = px[i][3];
        if (px[i][3] > amax) amax = px[i][3];
    }
    out[0] = (uint8_t)amax;
    out[1] = (uint8_t)amin;
    for (i = 0; i < 16; i++) {
        int a = px[i][3], best = 0, bd = 1 << 30, k;
        for (k = 0; k < 8; k++) {
            int v = k == 0 ? amax : k == 1 ? amin
                  : amax > amin ? ((8 - k) * amax + (k - 1) * amin) / 7 : amax;
            int d = abs(v - a);
            if (d < bd) { bd = d; best = k; }
        }
        abits |= (uint64_t)best << (3 * i);
    }
    for (i = 0; i < 6; i++)
        out[2 + i] = (uint8_t)(abits >> (8 * i));

    /* colour: the principal axis of the visible pixels, weighted by alpha */
    for (i = 0; i < 16; i++) {
        float w = px[i][3] / 255.0f + 1e-3f;
        mean[0] += w * px[i][0]; mean[1] += w * px[i][1]; mean[2] += w * px[i][2];
        wsum += w;
    }
    mean[0] /= wsum; mean[1] /= wsum; mean[2] /= wsum;
    for (i = 0; i < 16; i++) {
        float w = px[i][3] / 255.0f + 1e-3f;
        float d0 = px[i][0] - mean[0], d1 = px[i][1] - mean[1], d2 = px[i][2] - mean[2];
        cov[0] += w * d0 * d0; cov[1] += w * d0 * d1; cov[2] += w * d0 * d2;
        cov[3] += w * d1 * d1; cov[4] += w * d1 * d2; cov[5] += w * d2 * d2;
    }
    for (i = 0; i < 8; i++) {
        float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        float l = sqrtf(x * x + y * y + z * z);
        if (l < 1e-6f) break;
        axis[0] = x / l; axis[1] = y / l; axis[2] = z / l;
    }
    for (i = 0; i < 16; i++) {
        float t;
        if (px[i][3] < 8) continue;
        t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
    }
    if (tmin > tmax) tmin = tmax = 0;
    for (i = 0; i < 3; i++) {
        lo[i] = mean[i] + tmin * axis[i];
        hi[i] = mean[i] + tmax * axis[i];
    }
    c0 = to565(hi);
    c1 = to565(lo);
    if (c0 < c1) { uint16_t s = c0; c0 = c1; c1 = s; }
    from565(c0, p0);
    from565(c1, p1);
    for (i = 0; i < 3; i++) {
        pal[0][i] = p0[i];
        pal[1][i] = p1[i];
        pal[2][i] = (2 * p0[i] + p1[i]) / 3;
        pal[3][i] = (p0[i] + 2 * p1[i]) / 3;
    }
    for (i = 0; i < 16; i++) {
        int k, best = 0, bd = 1 << 30;
        if (c0 != c1)
            for (k = 0; k < 4; k++) {
                int d0 = px[i][0] - pal[k][0], d1 = px[i][1] - pal[k][1], d2 = px[i][2] - pal[k][2];
                int d = d0 * d0 + d1 * d1 + d2 * d2;
                if (d < bd) { bd = d; best = k; }
            }
        cbits |= (uint32_t)best << (2 * i);
    }
    out[8] = (uint8_t)c0; out[9] = (uint8_t)(c0 >> 8);
    out[10] = (uint8_t)c1; out[11] = (uint8_t)(c1 >> 8);
    out[12] = (uint8_t)cbits; out[13] = (uint8_t)(cbits >> 8);
    out[14] = (uint8_t)(cbits >> 16); out[15] = (uint8_t)(cbits >> 24);
}

/* ── The atlas ─────────────────────────────────────────────────────────── */

/* The icons' boxes in art_xbox (measured from the PAL disc's atlas). */
static Icon s_icons[] = {
    {   2, 230,  21, 249, SHAPE_CIRCLE, 0 },    /* Y */
    {  23, 230,  42, 249, SHAPE_CIRCLE, 0 },    /* X */
    /* A: the game's sprite for this slot ends short of the measured box, so
     * a circle filling all 20 columns lost its right edge on screen; drawn
     * 2 px narrower (a 9 px radius, centred 1 px left), the whole ring shows. */
    {  44, 230,  63, 249, SHAPE_CIRCLE, 0, 2 }, /* A */
    {  64, 230,  83, 249, SHAPE_CIRCLE, 0 },    /* B */
    {  86, 231, 103, 247, SHAPE_PILL,   0 },    /* black */
    { 107, 230, 123, 246, SHAPE_PILL,   0 },    /* white */
    {  50, 185,  81, 216, SHAPE_PILL,   0 },    /* left trigger */
    { 141, 185, 172, 216, SHAPE_PILL,   0 },    /* right trigger */
    {  89, 189, 113, 213, SHAPE_CIRCLE, 0 },    /* Start */
    { 118, 192, 137, 211, SHAPE_CIRCLE, 0 },    /* Back */
    /* the trigger letter tiles the text prompts and some screens use, by
     * language: L / R (xbtr_L/R), G / D (French), I / D (Spanish) */
    { 127, 223, 156, 249, SHAPE_PILL,   0 },    /* L tile */
    { 158, 223, 187, 249, SHAPE_PILL,   0 },    /* R tile */
    { 190, 223, 219, 249, SHAPE_PILL,   0 },    /* D tile (right) */
    { 221, 223, 250, 249, SHAPE_PILL,   0 },    /* G tile (left) */
    { 224,   2, 253,  28, SHAPE_PILL,   0 },    /* I tile (left) */
    { 222,  31, 253,  62, SHAPE_PILL,   0 },    /* I circle (left) */
    { 222,  65, 253,  96, SHAPE_PILL,   0 },    /* G circle (left) */
    { 222, 100, 253, 131, SHAPE_PILL,   0 },    /* D circle (right) */
};

/* The block-aligned regions repainted (inclusive pixel boxes; each holds
 * only the icons above and transparent pixels in the original). */
static const struct { int x0, y0, x1, y1; } k_regions[] = {
    {   0, 228, 123, 251 },                     /* Y X A B black white */
    {  48, 184,  83, 219 },                     /* left trigger */
    { 140, 184, 175, 219 },                     /* right trigger */
    {  88, 188, 115, 215 },                     /* Start */
    { 116, 192, 139, 211 },                     /* Back */
    { 124, 220, 251, 251 },                     /* L R D G tiles */
    { 220,   0, 255, 135 },                     /* I tile, I G D circles */
};

static void labels_set(void)
{
    int ps2 = ssx3_Ps2Controls();
    s_icons[0].label = ps2 ? "Y" : "X";
    s_icons[1].label = ps2 ? "X" : "Y";
    s_icons[2].label = ps2 ? "A" : "B";
    s_icons[3].label = ps2 ? "B" : "A";
    s_icons[4].label = "L";
    s_icons[5].label = "R";
    s_icons[6].label = "ZL";
    s_icons[7].label = "ZR";
    s_icons[8].label = "+";
    s_icons[9].label = "-";
    s_icons[10].label = "ZL";
    s_icons[11].label = "ZR";
    s_icons[12].label = "ZR";
    s_icons[13].label = "ZL";
    s_icons[14].label = "ZL";
    s_icons[15].label = "ZL";
    s_icons[16].label = "ZL";
    s_icons[17].label = "ZR";
}

/* Repaint the icons in a 256x256 DXT5 image (65536 bytes, linear blocks). */
static void atlas_patch(uint8_t *dxt)
{
    static uint8_t rgba[ATLAS * ATLAS * 4];
    size_t r, i;
    labels_set();
    memset(rgba, 0, sizeof rgba);
    for (i = 0; i < sizeof s_icons / sizeof s_icons[0]; i++) {
        Icon trimmed = s_icons[i];
        const Icon *ic = &trimmed;
        int bx0, by0, bx1, by1;
        trimmed.x1 -= trimmed.trim_r;
        bx0 = ic->x0 - 2; by0 = ic->y0 - 2; bx1 = ic->x1 + 2; by1 = ic->y1 + 2;
        bx0 = bx0 < 0 ? 0 : bx0; by0 = by0 < 0 ? 0 : by0;
        bx1 = bx1 > ATLAS - 1 ? ATLAS - 1 : bx1; by1 = by1 > ATLAS - 1 ? ATLAS - 1 : by1;
        icon_draw(rgba, ic, bx0, by0, bx1, by1);
    }
    if (pro_on())
        pro_draw(rgba);
    for (r = 0; r <= sizeof k_regions / sizeof k_regions[0]; r++) {
        int bx, by, x0, y0, x1, y1;
        if (r < sizeof k_regions / sizeof k_regions[0]) {
            x0 = k_regions[r].x0; y0 = k_regions[r].y0; x1 = k_regions[r].x1; y1 = k_regions[r].y1;
        } else if (pro_on()) {
            x0 = 0; y0 = 0; x1 = PRO_X1; y1 = PRO_Y1;          /* the controller picture */
        } else
            break;
        for (by = y0; by <= y1; by += 4)
            for (bx = x0; bx <= x1; bx += 4) {
                uint8_t px[16][4];
                int k;
                for (k = 0; k < 16; k++)
                    memcpy(px[k], rgba + ((size_t)(by + (k >> 2)) * ATLAS + bx + (k & 3)) * 4, 4);
                dxt5_block(px, dxt + ((size_t)(by / 4) * (ATLAS / 4) + bx / 4) * 16);
            }
    }
}

/* The console picture of the memory / save screens ("Checking hard disk",
 * "Loading PLAYER 1. Please don't turn off ...", saving, deleting): the shape
 * "XBOXMemCard" in fe_1.xsh, 128 x 128 DXT5 (the orange "!" is another
 * sprite, drawn over it). Replaced by the Switch logo drawn below (the
 * renderer swaps in a 512 x 512 copy, so it stays sharp at any resolution),
 * framed like the original sprite. */
#define CONSOLE 128

/* The Switch logo, drawn here (no image needed): the outlined left Joy-Con
 * with its stick, the solid right one with its button hole, the logo's usual
 * proportions. The screen draws this sprite 2.12 times as wide
 * as it is tall on a 16:9 screen with the menus stretched (measured on the
 * Switch, 2026-11-25: the game's own placement for the Xbox console picture,
 * x 4:3 -> 16:9), 1.59 times without the stretch; the logo is drawn that
 * much narrower in the texture so it comes out square. Coordinates in logo
 * heights, the logo 1 x 1. */
#define LOGO_SQUEEZE_43 0.6278f             /* the sprite's own shape on a 4:3 screen */
static float rrect_sd(float x, float y, float x0, float y0, float x1, float y1, float rl, float rr)
{
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, r = x < cx ? rl : rr;
    float qx = fabsf(x - cx) - (x1 - x0) * 0.5f + r, qy = fabsf(y - cy) - (y1 - y0) * 0.5f + r;
    float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0.0f ? in : 0.0f) - r;
}
static float logo_sd(float x, float y)
{
    const float t = 0.083f;                                 /* the outline's stroke */
    float lo = rrect_sd(x, y, 0.0f, 0.0f, 0.491f, 1.0f, 0.22f, 0.03f);
    float li = rrect_sd(x, y, t, t, 0.491f - t, 1.0f - t, 0.22f - t, 0.0f);
    float c1 = hypotf(x - 0.255f, y - 0.296f) - 0.093f;     /* the left stick */
    float ro = rrect_sd(x, y, 0.593f, 0.0f, 1.0f, 1.0f, 0.03f, 0.22f);
    float c2 = hypotf(x - 0.787f, y - 0.540f) - 0.093f;     /* the button hole */
    float left = lo > -li ? lo : -li, right = ro > -c2 ? ro : -c2;
    left = left < c1 ? left : c1;
    return left < right ? left : right;
}
/* w x h RGBA bytes (rgba != NULL) or A8R8G8B8 dwords (argb): the logo
 * centred, 0.84 of the height tall, squeeze x as wide. */
static void logo_draw(uint8_t *rgba, uint32_t *argb, int w, int h, float squeeze)
{
    float size = 0.84375f * (float)h;                       /* 108 of 128, as the image */
    float lw = size * squeeze, x0 = ((float)w - lw) * 0.5f, y0 = ((float)h - size) * 0.5f;
    float aa = 1.0f / (lw < size ? lw : size);              /* a texel, the coarser way */
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float u = ((float)x + 0.5f - x0) / lw, v = ((float)y + 0.5f - y0) / size;
            float d = logo_sd(u, v), a = 0.5f - d / aa;
            uint8_t al = a <= 0.0f ? 0 : a >= 1.0f ? 255 : (uint8_t)(a * 255.0f + 0.5f);
            if (rgba) {
                uint8_t *o = rgba + ((size_t)y * w + x) * 4;
                o[0] = o[1] = o[2] = 0; o[3] = al;
            } else
                argb[(size_t)y * w + x] = (uint32_t)al << 24;
        }
}
/* A transparent 4x4 block that marks the sprite as the logo: the renderer
 * (vkr_backend.c) puts the full-resolution logo in its place. */
static const uint8_t k_logo_mark[16] = {
    0, 0, 0, 0, 0, 0, 0, 0,  0x57, 0x53, 0x58, 0x4E, 0x4F, 0x47, 0x4F, 0x4C
};
int ssx3_ConsoleLogoIs(const uint8_t *block0)
{
    return !memcmp(block0, k_logo_mark, 16);
}
/* The logo at full resolution for the renderer: w x h A8R8G8B8. */
void ssx3_ConsoleLogoHd(uint32_t *argb, int w, int h, float squeeze)
{
    logo_draw(NULL, argb, w, h, squeeze);
}

static int console_patch(uint8_t *dxt)
{
    static uint8_t rgba[CONSOLE * CONSOLE * 4];
    float sq = LOGO_SQUEEZE_43;
    int bx, by;
#ifndef BUTTON_ICONS_TEST
    {
        extern int ssx3_StretchMenus(void);
        extern float ssx3_Aspect(void);
        if (ssx3_StretchMenus())
            sq /= ssx3_Aspect() * 0.75f;
    }
#endif
    logo_draw(rgba, NULL, CONSOLE, CONSOLE, sq);
    for (by = 0; by < CONSOLE; by += 4)
        for (bx = 0; bx < CONSOLE; bx += 4) {
            uint8_t px[16][4];
            int k;
            for (k = 0; k < 16; k++)
                memcpy(px[k], rgba + ((size_t)(by + (k >> 2)) * CONSOLE + bx + (k & 3)) * 4, 4);
            dxt5_block(px, dxt + ((size_t)(by / 4) * (CONSOLE / 4) + bx / 4) * 16);
        }
    memcpy(dxt, k_logo_mark, 16);           /* the top-left corner is empty anyway */
    return 1;
}

/* Patch art_xbox (and the console picture) in an SHPX file. 1 if patched. */
static int shpx_patch(uint8_t *b, size_t len)
{
    uint32_t n, i;
    int done = 0;
    if (len < 16 || memcmp(b, "SHPX", 4))
        return 0;
    memcpy(&n, b + 8, 4);
    if (n > 4096 || 16 + (size_t)n * 8 > len)
        return 0;
    for (i = 0; i < n; i++) {
        uint32_t off, next_rel, att, bytes;
        uint16_t w, h;
        memcpy(&off, b + 16 + i * 8 + 4, 4);
        if ((size_t)off + 16 > len || b[off] != 0x62)
            continue;
        memcpy(&w, b + off + 4, 2);
        memcpy(&h, b + off + 6, 2);
        bytes = (uint32_t)w * h;            /* DXT5: one byte a pixel */
        next_rel = (uint32_t)b[off + 1] | (uint32_t)b[off + 2] << 8 | (uint32_t)b[off + 3] << 16;
        if (next_rel < 16 + bytes || (size_t)off + next_rel + 4 + 12 > len)
            continue;
        /* the name attachment (0x70) after the pixels */
        att = off + next_rel;
        if (b[att] != 0x70)
            continue;
        if (w == ATLAS && h == ATLAS && !memcmp(b + att + 4, "art_xbox", 9)) {
            atlas_patch(b + off + 16);
            done = 1;
        } else if (w == CONSOLE && h == CONSOLE && !memcmp(b + att + 4, "XBOXMemCard", 12)) {
            done |= console_patch(b + off + 16);
        }
    }
    return done;
}
#ifndef BUTTON_ICONS_TEST
/* ── Riding prompts (SSX3NX_PS2_CONTROLS=1) ───────────────────────────────
 *
 * The icons above are per button, and a menu means them that way. A few
 * prompts mean an action instead, and with the PS2 layout that action moved
 * to another button. The game finds those prompts' sprites by name, through
 * strings in its .rdata, while the menu layouts (.lui) carry the sprite
 * hashes themselves; so renaming here moves the action prompts and leaves
 * every menu alone. Sprite names hash with the game's own function
 * (sub_001AAEA0) and resolve through the UI's symbol table:
 *   xbbt_a/b/x/y  the face buttons     Black, White  the shoulder buttons
 *   xbtr_L/R      the trigger tiles (English / German); Left Trigger / Right
 *                 Trigger / Left Trigger span the French and Spanish ones
 *
 * 1. Text prompts: "RECOVER = @x", "UBER TRICK = @l1 + @b" / "@l1 + @y"
 *    (guest 0x292F20..): each @token resolves through a table built at boot
 *    (sub_00222E00, 0x368960, 0x24 per token: token hash, sprite hash, ...).
 *    Its sprite hashes become the PS2 meaning: x (boost, recovery) -> Y,
 *    l1 / g / i (tweak, by language) -> Y, b / y (grabs) -> ZR / L.
 * 2. The HUD's wipeout-bar recovery icon: a hash table built at boot
 *    (sub_00222B80): 0x3688FC xbbt_x -> Y (and 0x368900, the trigger tile
 *    one, likewise).
 * 3. The Ubertrick screen (sub_0008A350) hashes the names each time it
 *    opens: the strings themselves are renamed -- grabs X / Y / B / right
 *    trigger -> ZL / L / ZR / R, tweak (the left trigger) -> Y.
 * Assumes controller config 1, the one SSX3NX_PS2_CONTROLS rewrites.
 * Every write checks the original first; a different XBE is left alone. */

/* Through the toolkit's translation, not base + va: on the Switch guest
 * memory is a 1 MB page map, and base + va is right only for the low 64 MB.
 * The frontend unpacks its layouts into the contiguous window (0x80xxxxxx),
 * so the Options: Controller patch read the wrong memory, matched nothing
 * and never logged. Valid within one mapped range (RAM, the window). */
static uint8_t *guest(uint32_t va)
{
    extern void *xbox_GuestToHost(uint32_t va);
    return (uint8_t *)xbox_GuestToHost(va);
}

static uint32_t ea_hash(const char *s)
{
    uint32_t h = 0;
    for (; *s; s++) {
        uint32_t g;
        h = (h << 4) + (uint32_t)(int32_t)(signed char)*s;
        g = h & 0xF0000000u;
        if (g)
            h ^= (g >> 23) ^ g;
    }
    return h;
}

static const struct { uint32_t va; const char *was, *now; } k_names[] = {
    { 0x0028AD78, "xbbt_x",            "xbtr_L" },   /* grab (X slot)  -> ZL */
    { 0x0028AD70, "xbbt_y",            "Black"  },   /* grab (Y slot)  -> L  */
    { 0x0028AD80, "xbbt_b",            "xbtr_R" },   /* grab (B slot)  -> ZR */
    { 0x0028AD60, "xbtr_R",            "White"  },   /* grab (RT slot) -> R  */
    { 0x0028AD2C, "Right Trigger",     "White"  },
    { 0x0028AD68, "xbtr_L",            "xbbt_y" },   /* tweak (LT)     -> Y  */
    { 0x0028AD50, "Left Trigger",      "xbbt_y" },
    { 0x0028AD3C, "Left Trigger span", "xbbt_y" },
};

static const struct { const char *token, *sprite; } k_tokens[] = {
    { "x", "xbbt_y" }, { "y", "Black" }, { "b", "xbtr_R" }, { "l1", "xbbt_y" },
    { "r1", "White" }, { "g", "xbbt_y" }, { "i", "xbbt_y" },
};

#define TOKEN_TABLE 0x00368960u
#define TOKEN_COUNT 9
#define TOKEN_STRIDE 0x24u

static int s_prompts_logged;

/* Apply 1 and 2 where the boot code has filled the tables (and again if it
 * ever rebuilds them). Returns how many entries were changed. */
static int prompts_tables(void)
{
    int changed = 0;
    unsigned k, t;
    for (k = 0; k < TOKEN_COUNT; k++) {
        volatile uint32_t *e = (volatile uint32_t *)guest(TOKEN_TABLE + k * TOKEN_STRIDE);
        for (t = 0; t < sizeof k_tokens / sizeof k_tokens[0]; t++)
            if (e[0] == ea_hash(k_tokens[t].token)) {
                uint32_t want = ea_hash(k_tokens[t].sprite);
                if (e[1] != want && e[1] != 0) { e[1] = want; changed++; }
            }
    }
    {
        static const uint32_t hud[2] = { 0x003688FCu, 0x00368900u };
        uint32_t y = ea_hash("xbbt_y");
        for (k = 0; k < 2; k++) {
            volatile uint32_t *e = (volatile uint32_t *)guest(hud[k]);
            if (*e == ea_hash("xbbt_x") || *e == ea_hash("xbtr_L")) { *e = y; changed++; }
        }
    }
    return changed;
}

/* 4. Options: Controller's callouts, laid out for the Pro Controller picture
 *    (the user's art, icons/pro_controller.png, or the drawn one: the same
 *    button positions). The screen is a layout in fe.lui (a RefPack chunk,
 *    24576 bytes unpacked): text elements {0x002C0017, id, ..., +0x24 string
 *    hash (cmnamer.loc)}, line elements {0x002C0018, id, ...}, groups
 *    {size << 16 | 0x0010, id, flags, 5 x -1, child count, child ids} and
 *    placements {size << 16 | 0x0021, id, n, n x (u16 property, s16 value)}:
 *    0 x, 1 y (relative to the parent group), 0xC text alignment (9 left edge
 *    at x, 17 centred, 33 right edge), a line's four corners in 0x15/16,
 *    0x1E/1F, 0x27/28, 0x30/31.
 *
 *    Three groups at (-90, 112): one shown with every config (07E003FE) and
 *    one per config (0920B2D4 Default, 0E1D440F Pro). The Xbox screen kept
 *    labels in the shared group that meant different buttons per config, so
 *    no one set of positions fitted both; here the groups' members are
 *    exchanged (each group keeps its count). A group adopts only elements
 *    recorded before it (a later one is left at the root -- seen on the
 *    Switch as Crouch/brake drawn over the title), so the shared group
 *    (recorded early) holds subgroup 06F239E3, whose 13 lines carry every
 *    line both configs use (ZL, L, ZR, R, left stick x2, D-pad, right
 *    stick), three shared labels (Turn/spin/flip on the D-pad, Board press,
 *    Crouch/brake) and four hidden fillers; each config its own labels:
 *      Default (SSX3NX_PS2_CONTROLS):  Grab board bracketing ZL + L and
 *        ZR + R, Y Boost/Tweak, X Reset, A Hand plant, B Jump, left stick
 *        Turn + Crouch/brake
 *      Pro (input2.map, unchanged):  ZL Boost/Tweak, L and R Hand plant,
 *        ZR Jump, X Y A B Grab board (one bracket), left stick
 *        Turn/spin/flip + Crouch/brake
 *    Every line not used is hidden. The positions are measured on a Switch
 *    screenshot of the screen (1280x720, menus stretched): for the groups
 *    at (-90, 112) screen x = 1.944 x - 157, y = 1.5 (y + 112); for
 *    Default's subgroup 06F239E3 x = 1.944 (x + 13) + 18, y = 1.5 y. The
 *    tables are generated (scratch ctlgen.ps1, 2026-11-25). Patched as the
 *    game unpacks the chunk (the RefPack decoder, ssx3_overrides.c
 *    sub_0017C9C0), when Options is opened, before the screen is built from
 *    it. All or nothing. */
/* group child lists (original, new) */
static const struct { uint32_t id; uint32_t was[15], now[15]; uint32_t n; } k_ctl_groups[] = {
    { 0x07E003FEu, { 0x0BC514A8u, 0x0BC514C8u, 0x0BC51708u, 0x0BC51388u, 0x0BC513A8u, 0x06F239E3u, 0x0BC513B8u, 0x0BC513C8u },
      { 0x06F239E3u, 0x0BC514A8u, 0x0BC513C8u, 0x0BC513B8u, 0x0B6EA401u, 0x0B6EA402u, 0x0B6EA403u, 0x0B6EA404u }, 8 },
    { 0x0920B2D4u, { 0x0BC51498u, 0x0BC514B8u, 0x0BC514D8u, 0x0BC514F8u, 0x0BC51398u, 0x07304850u, 0x00730488u, 0x0304B2A1u, 0x0304B2A4u, 0x00730484u, 0x0BC514E8u, 0x0BC51FE1u, 0x07304840u, 0x00730486u, 0x00730487u },
      { 0x0BC51FE1u, 0x0BC514D8u, 0x0BC51498u, 0x0BC514B8u, 0x0BC514E8u, 0x0BC514F8u, 0x0BC51398u, 0x07304850u, 0x00730488u, 0x0304B2A1u, 0x0304B2A4u, 0x00730484u, 0x07304840u, 0x00730486u, 0x00730487u }, 15 },
    { 0x0E1D440Fu, { 0x0BC51AF0u, 0x0C513990u, 0x03048810u, 0x03048910u, 0x03048A10u, 0x03048B10u, 0x03048C10u, 0x03048D10u, 0x03048E10u, 0x0BC51488u, 0x0B6EA476u, 0x0B6EA401u, 0x0B6EA402u, 0x0B6EA403u, 0x0B6EA404u },
      { 0x0BC51AF0u, 0x0C513990u, 0x0BC51488u, 0x0BC51708u, 0x0BC51388u, 0x0BC513A8u, 0x0BC514C8u, 0x03048810u, 0x03048910u, 0x03048A10u, 0x03048B10u, 0x03048C10u, 0x03048D10u, 0x03048E10u, 0x0B6EA476u }, 15 },
};
/* text elements: x, y relative to the parent, alignment, string (0 = keep); x -2000 = hidden */
static const struct { uint32_t id; int16_t x, y, align; uint32_t was, now; } k_ctl_texts[] = {
    /* both configs (common group) */
    { 0x0BC514A8u, 285, 68, 33, 0x00000000u, 0x00000000u },   /* Turn/spin/flip -> D-pad */
    { 0x0BC513C8u, 554, 68, 9, 0x00000000u, 0x00000000u },    /* Board press -> right stick */
    { 0x0BC513B8u, 285, 44, 33, 0x05F48058u, 0x06F28E15u },   /* Crouch/brake -> left stick (was Grab/Punch) */
    /* Default */
    { 0x0BC51FE1u, 285, -8, 33, 0x0F8CC8DBu, 0x0994D6A4u },   /* Grab board -> ZL + L */
    { 0x0BC514D8u, 554, -8, 9, 0x00000000u, 0x00000000u },    /* Grab board -> ZR + R */
    { 0x0BC51498u, 554, 12, 9, 0x0517D492u, 0x0AC2DF5Bu },    /* Boost/Tweak -> Y */
    { 0x0BC514B8u, 554, 26, 9, 0x06F28E15u, 0x0F8FDA64u },    /* Reset -> X (was Crouch/brake) */
    { 0x0BC514E8u, 554, 39, 9, 0x0994D6A4u, 0x07281774u },    /* Hand plant -> A */
    { 0x0BC514F8u, 554, 52, 9, 0x00000000u, 0x00000000u },    /* Jump -> B */
    { 0x0BC51398u, 285, 30, 33, 0x00000000u, 0x00000000u },   /* Turn -> left stick */
    /* Pro */
    { 0x0BC51AF0u, 554, -16, 9, 0x00000000u, 0x00000000u },   /* Jump -> ZR */
    { 0x0C513990u, 285, 30, 33, 0x00000000u, 0x00000000u },   /* Turn/spin/flip -> left stick */
    { 0x0BC51488u, 285, -16, 33, 0x00000000u, 0x00000000u },  /* Boost/Tweak -> ZL */
    { 0x0BC51708u, 285, 0, 33, 0x00000000u, 0x00000000u },    /* Hand plant -> L */
    { 0x0BC51388u, 554, 0, 9, 0x0F819F35u, 0x07281774u },     /* Hand plant -> R (was Pause) */
    { 0x0BC513A8u, 554, 39, 9, 0x00000000u, 0x00000000u },    /* Grab board -> X Y A B */
    { 0x0BC514C8u, -2000, 0, -1, 0x00000000u, 0x00000000u },  /* Reset: not used */
};
/* lines: position relative to the parent and the rectangle's size; w 0 = hidden */
static const struct { uint32_t id; int16_t x, y, w, h; } k_ctl_lines[] = {
    /* both configs: subgroup 06F239E3 in the common group (origin 13, 0 absolute) */
    { 0x07304846u, 186, 102, 55, 2 },      /* ZL */
    { 0x00730482u, 186, 118, 51, 2 },      /* L */
    { 0x00730485u, 393, 102, 53, 2 },      /* ZR */
    { 0x00730489u, 395, 118, 51, 2 },      /* R */
    { 0x0304B2A3u, 186, 147, 51, 2 },      /* left stick, upper row */
    { 0x07304845u, 186, 161, 53, 2 },      /* left stick, lower row */
    { 0x00730483u, 186, 186, 79, 2 },      /* D-pad */
    { 0x07304849u, 356, 186, 91, 2 },      /* right stick */
    { 0x07304851u, -2000, 0, 0, 0 },
    { 0x07304852u, -2000, 0, 0, 0 },
    { 0x07304842u, -2000, 0, 0, 0 },
    { 0x07304847u, -2000, 0, 0, 0 },
    { 0x07304871u, -2000, 0, 0, 0 },
    /* the common group's fillers (defined after it: they land at the root) */
    { 0x0B6EA401u, -2000, 0, 0, 0 },
    { 0x0B6EA402u, -2000, 0, 0, 0 },
    { 0x0B6EA403u, -2000, 0, 0, 0 },
    { 0x0B6EA404u, -2000, 0, 0, 0 },
    /* Default */
    { 0x07304850u, 289, -9, 2, 17 },       /* left bracket (ZL + L) */
    { 0x00730488u, 548, -9, 2, 17 },       /* right bracket (ZR + R) */
    { 0x0304B2A1u, 457, 18, 93, 2 },       /* Y: across the top... */
    { 0x0304B2A4u, 456, 19, 2, 19 },       /* ...down to Y */
    { 0x00730484u, 480, 31, 70, 2 },       /* X */
    { 0x07304840u, 494, 44, 56, 2 },       /* A */
    { 0x00730486u, 480, 58, 70, 2 },       /* B */
    { 0x00730487u, -2000, 0, 0, 0 },
    /* Pro */
    { 0x03048810u, 517, 19, 2, 41 },       /* face buttons: the spine */
    { 0x03048910u, 457, 18, 62, 2 },       /* over to Y... */
    { 0x03048A10u, 456, 19, 2, 19 },       /* ...down to Y */
    { 0x03048B10u, 480, 31, 39, 2 },       /* X */
    { 0x03048C10u, 494, 44, 56, 2 },       /* A, on to the label */
    { 0x03048D10u, 480, 58, 39, 2 },       /* B */
    { 0x03048E10u, -2000, 0, 0, 0 },
    { 0x0B6EA476u, -2000, 0, 0, 0 },
};

/* The fade-in the labels share (fe.lui animation 0AC8F1B2: alpha 0 -> 255
 * over 8 frames, nothing else). Each line has an animation of its own that
 * sets x, y, size and all four corners every frame -- the Xbox geometry --
 * so a patched placement never showed; the lines are given this one. */
#define CTL_FADE_ANIM 0x0AC8F1B2u

#define CTL_FIRST_ID 0x0BC51FE1u        /* the fallback scan's anchor: a text record */

/* The element's text record / placement record in an unpacked layout. */
static uint8_t *ctl_find(uint8_t *b, uint32_t n, uint32_t id, int placement)
{
    uint32_t p, t, v;
    for (p = 0; p + 0x2C <= n; p += 4) {
        memcpy(&v, b + p + 4, 4);
        if (v != id)
            continue;
        memcpy(&t, b + p, 4);
        if (placement == 1 ? (t & 0xFFFFu) == 0x0021u
            : placement == 2 ? (t & 0xFFFFu) == 0x0010u && (t >> 16) >= 0x24u
            : placement == 3 ? t == 0x002C0018u
            : t == 0x002C0017u)
            return b + p;
    }
    return NULL;
}

/* The element's animation binding {0x00100020, animation key, id, 1}. */
static uint8_t *ctl_find_anim(uint8_t *b, uint32_t n, uint32_t id)
{
    uint32_t p, t, v;
    for (p = 0; p + 16 <= n; p += 4) {
        memcpy(&v, b + p + 8, 4);
        if (v != id)
            continue;
        memcpy(&t, b + p, 4);
        if (t == 0x00100020u)
            return b + p;
    }
    return NULL;
}

/* A placement's property: its value's address, NULL if the record lacks it. */
static uint8_t *ctl_prop(uint8_t *rec, unsigned pid)
{
    uint32_t n, k;
    memcpy(&n, rec + 8, 4);
    if (n == 0 || n > 64)
        return NULL;
    for (k = 0; k < n; k++) {
        uint16_t p;
        memcpy(&p, rec + 12 + 4 * k, 2);
        if (p == pid)
            return rec + 14 + 4 * k;
    }
    return NULL;
}

static void ctl_set(uint8_t *rec, unsigned pid, int v)
{
    int16_t s = (int16_t)v;
    uint8_t *q = ctl_prop(rec, pid);
    if (q) memcpy(q, &s, 2);
}

static volatile int s_ctl_done;         /* patched once (the hook or the scan) */
static volatile unsigned s_ctl_chunks;  /* layout-sized chunks the hook saw */

static int ctl_enabled(void)
{
    return ssx3_ButtonIcons() && ssx3_Ps2Controls() && pro_on();
}

#define CTL_NG (sizeof k_ctl_groups / sizeof k_ctl_groups[0])
#define CTL_NT (sizeof k_ctl_texts / sizeof k_ctl_texts[0])
#define CTL_NL (sizeof k_ctl_lines / sizeof k_ctl_lines[0])

/* Patch the layout in b[0..size) if it is Options: Controller's: 1 done (or
 * already done), 0 not that layout, -1 that layout but a record differs
 * (logged, nothing changed: all or nothing). */
static int ctl_patch_buf(uint8_t *b, uint32_t size, const char *where)
{
    static const unsigned k_line_props[] = { 0, 1, 6, 7, 0x15, 0x16, 0x1E, 0x1F, 0x27, 0x28, 0x30, 0x31,
                                             0x1A, 0x23, 0x2C, 0x35 };
    uint8_t *grp[CTL_NG], *txt[CTL_NT], *tpl[CTL_NT], *lpl[CTL_NL], *lan[CTL_NL], *lel[CTL_NL];
    size_t i, k;
    static int s_skips;
    int already = 1;
    if (!ctl_find(b, size, CTL_FIRST_ID, 0))
        return 0;                       /* cheap first test */
#define CTL_SKIP(why, id) do { if (s_skips++ < 4) log_msg(LOG_WARN, "ICONS", \
        "Options: Controller layout (%s, %u bytes) not patched: %s %08X", where, size, why, id); \
        return -1; } while (0)
    for (i = 0; i < CTL_NG; i++) {
        uint32_t n, c;
        int same_was = 1, same_now = 1;
        if (!(grp[i] = ctl_find(b, size, k_ctl_groups[i].id, 2)))
            CTL_SKIP("no group record for", k_ctl_groups[i].id);
        memcpy(&n, grp[i] + 0x20, 4);
        if (n != k_ctl_groups[i].n)
            CTL_SKIP("unexpected child count in group", k_ctl_groups[i].id);
        for (k = 0; k < n; k++) {
            memcpy(&c, grp[i] + 0x24 + 4 * k, 4);
            same_was &= c == k_ctl_groups[i].was[k];
            same_now &= c == k_ctl_groups[i].now[k];
        }
        if (same_now)
            continue;                   /* unpacked again into a patched copy */
        already = 0;
        if (!same_was)
            CTL_SKIP("unexpected children in group", k_ctl_groups[i].id);
    }
    for (i = 0; i < CTL_NT; i++) {
        txt[i] = NULL;
        if (!(tpl[i] = ctl_find(b, size, k_ctl_texts[i].id, 1)) || !ctl_prop(tpl[i], 0)
            || !ctl_prop(tpl[i], 1) || !ctl_prop(tpl[i], 0xC))
            CTL_SKIP("no usable placement for", k_ctl_texts[i].id);
        if (k_ctl_texts[i].now) {
            uint32_t s;
            if (!(txt[i] = ctl_find(b, size, k_ctl_texts[i].id, 0)))
                CTL_SKIP("no text record for", k_ctl_texts[i].id);
            memcpy(&s, txt[i] + 0x24, 4);
            if (s != k_ctl_texts[i].now && s != k_ctl_texts[i].was)
                CTL_SKIP("unexpected string on", k_ctl_texts[i].id);
        }
    }
    for (i = 0; i < CTL_NL; i++) {
        if (!(lpl[i] = ctl_find(b, size, k_ctl_lines[i].id, 1)))
            CTL_SKIP("no placement for line", k_ctl_lines[i].id);
        for (k = 0; k < sizeof k_line_props / sizeof k_line_props[0]; k++)
            if (!ctl_prop(lpl[i], k_line_props[k]))
                CTL_SKIP("line placement lacks a corner:", k_ctl_lines[i].id);
        if (!(lan[i] = ctl_find_anim(b, size, k_ctl_lines[i].id)))
            CTL_SKIP("no animation binding for line", k_ctl_lines[i].id);
        if (!(lel[i] = ctl_find(b, size, k_ctl_lines[i].id, 3)))
            CTL_SKIP("no element record for line", k_ctl_lines[i].id);
    }
#undef CTL_SKIP
    for (i = 0; i < CTL_NG; i++)
        memcpy(grp[i] + 0x24, k_ctl_groups[i].now, 4 * k_ctl_groups[i].n);
    for (i = 0; i < CTL_NT; i++) {
        ctl_set(tpl[i], 0, k_ctl_texts[i].x);
        if (k_ctl_texts[i].align >= 0) {
            ctl_set(tpl[i], 1, k_ctl_texts[i].y);
            ctl_set(tpl[i], 0xC, k_ctl_texts[i].align);
        }
        if (txt[i])
            memcpy(txt[i] + 0x24, &k_ctl_texts[i].now, 4);
    }
    for (i = 0; i < CTL_NL; i++) {
        int w = k_ctl_lines[i].w, h = k_ctl_lines[i].h;
        uint8_t *r = lpl[i];
        const uint32_t fade = CTL_FADE_ANIM;
        memcpy(lan[i] + 4, &fade, 4);   /* the placement, not the Xbox animation */
        ctl_set(r, 0, k_ctl_lines[i].x);
        if (!w)
            continue;                   /* hidden: off the screen */
        ctl_set(r, 1, k_ctl_lines[i].y);
        ctl_set(r, 6, w); ctl_set(r, 7, h);
        /* draw layer (flags' low byte) 0x0C, over the controller picture
         * (0x0A); the Xbox's D-pad, right-stick and one other line were 0x09,
         * under it -- they never crossed it there */
        lel[i][8] = 0x0C;
        /* opaque corners (the animation faded them in from 0) */
        ctl_set(r, 0x1A, 255); ctl_set(r, 0x23, 255); ctl_set(r, 0x2C, 255); ctl_set(r, 0x35, 255);
        /* corners in the order the disc's lines have them */
        ctl_set(r, 0x15, w); ctl_set(r, 0x16, h);
        ctl_set(r, 0x1E, 0); ctl_set(r, 0x1F, h);
        ctl_set(r, 0x27, 0); ctl_set(r, 0x28, 0);
        ctl_set(r, 0x30, w); ctl_set(r, 0x31, 0);
        /* white at every corner (one of the reused lines is stored black) */
        for (k = 0; k < 4; k++) {
            ctl_set(r, 0x1B + 9 * (unsigned)k, 255);
            ctl_set(r, 0x1C + 9 * (unsigned)k, 255);
            ctl_set(r, 0x1D + 9 * (unsigned)k, 255);
        }
    }
    if (!s_ctl_done || !already)
        log_msg(LOG_WARN, "ICONS", "Options: Controller callouts laid out for the Pro Controller picture "
                "(Default: Y boost/tweak, B jump, A hand plant, X reset, L R ZL ZR grab; Pro: input2.map; %s)",
                where);
    s_ctl_done = 1;
    return 1;
}
/* 5. The loading screen's controller page ("Basic Controls", gl.lui chunk at
 *    0x44B4, 31540 bytes): the same picture and element ids, groups at
 *    (-90, 130) -- 18 lower than Options, so the positions relative to them
 *    are Options' -- and no per-line animations. Too few elements for both
 *    configs at once (a group adopts only elements recorded before it, and
 *    the Default group 0920B2D4 is recorded early, with 10), but the page
 *    shows one config: it is unpacked for each load, and laid out then for
 *    player 1's config in Pro's 30-slot group, which takes Default's id when
 *    Default is the one shown (the 10-slot group, with the other id, stays
 *    hidden). The shared group keeps Turn/spin/flip on the D-pad and the
 *    lines to ZL, L, ZR and R (its subgroup, origin 13, 18 absolute). */
static const uint32_t k_gl_def_was[10] = {
    0x0BC514F8u, 0x0BC51398u, 0x00730488u, 0x0304B2A1u, 0x00730484u,
    0x0BC513C8u, 0x07304874u, 0x07304884u, 0x07304894u, 0x0BC51394u };
static const uint32_t k_gl_pro_was[30] = {
    0x0C513990u, 0x0BC51AF0u, 0x03048810u, 0x03048910u, 0x03048A10u, 0x03048B10u, 0x03048C10u,
    0x0BC51488u, 0x00730483u, 0x03048411u, 0x0BC514C8u, 0x0BC51388u, 0x00730489u, 0x0304B2A3u,
    0x0BC513D8u, 0x0BC51708u, 0x07304847u, 0x07304891u, 0x07304851u, 0x07304852u, 0x07304842u,
    0x0BC513B8u, 0x07304850u, 0x07304849u, 0x0BC513A8u, 0x0B6EA470u, 0x0B6EA471u, 0x0B6EA474u,
    0x0B6EA472u, 0x0B6EA476u };
/* texts: per config (Default, Pro) x, y, alignment (-1 hidden), string (0 keep) */
static const struct { uint32_t id; int16_t x[2], y[2], al[2]; uint32_t s[2]; } k_gl_texts[] = {
    { 0x0BC514A8u, { 285, 285 }, { 68, 68 }, { 33, 33 }, { 0, 0 } },                  /* Turn/spin/flip -> D-pad */
    { 0x0BC513A8u, { 285, 554 }, { -8, 39 }, { 33, 9 }, { 0, 0 } },                   /* Grab board: ZL+L / XYAB */
    { 0x0BC513B8u, { 554, 285 }, { -8, 44 }, { 9, 33 }, { 0x0994D6A4u, 0x06F28E15u } }, /* Grab board ZR+R / Crouch/brake */
    { 0x0BC51488u, { 554, 285 }, { 12, -16 }, { 9, 33 }, { 0, 0 } },                  /* Boost/Tweak: Y / ZL */
    { 0x0BC514C8u, { 554, -2000 }, { 26, 0 }, { 9, -1 }, { 0, 0 } },                  /* Reset: X / - */
    { 0x0BC51708u, { 554, 285 }, { 39, 0 }, { 9, 33 }, { 0, 0 } },                    /* Hand plant: A / L */
    { 0x0BC51AF0u, { 554, 554 }, { 52, -16 }, { 9, 9 }, { 0, 0 } },                   /* Jump: B / ZR */
    { 0x0C513990u, { 285, 285 }, { 30, 30 }, { 33, 33 }, { 0x0DF8CDAEu, 0 } },        /* Turn / Turn/spin/flip: left stick */
    { 0x0BC51388u, { 285, 554 }, { 44, 0 }, { 33, 9 }, { 0x06F28E15u, 0x07281774u } }, /* Crouch/brake / Hand plant R (was Pause) */
    { 0x0BC513D8u, { 554, 554 }, { 68, 68 }, { 9, 9 }, { 0, 0 } },                    /* Board press -> right stick */
};
/* lines: per config x, y, w, h (w 0 = hidden) */
static const struct { uint32_t id; int16_t r[2][4]; } k_gl_lines[] = {
    /* shared: the subgroup */
    { 0x07304846u, { { 186, 102, 55, 2 }, { 186, 102, 55, 2 } } },   /* ZL */
    { 0x00730482u, { { 186, 118, 51, 2 }, { 186, 118, 51, 2 } } },   /* L */
    { 0x00730485u, { { 393, 102, 53, 2 }, { 393, 102, 53, 2 } } },   /* ZR */
    { 0x0304B2A4u, { { 395, 118, 51, 2 }, { 395, 118, 51, 2 } } },   /* R */
    /* the shown group: Default | Pro */
    { 0x03048810u, { { 289, -9, 2, 17 }, { 517, 19, 2, 41 } } },     /* left bracket | spine */
    { 0x03048910u, { { 548, -9, 2, 17 }, { 457, 18, 62, 2 } } },     /* right bracket | Y across */
    { 0x03048A10u, { { 457, 18, 93, 2 }, { 456, 19, 2, 19 } } },     /* Y across | Y down */
    { 0x03048B10u, { { 456, 19, 2, 19 }, { 480, 31, 39, 2 } } },     /* Y down | X */
    { 0x03048C10u, { { 480, 31, 70, 2 }, { 494, 44, 56, 2 } } },     /* X | A */
    { 0x00730483u, { { 494, 44, 56, 2 }, { 480, 58, 39, 2 } } },     /* A | B */
    { 0x03048411u, { { 480, 58, 70, 2 }, { 289, 35, 51, 2 } } },     /* B | left stick, upper */
    { 0x00730489u, { { 289, 35, 51, 2 }, { 289, 49, 53, 2 } } },     /* left stick upper | lower */
    { 0x0304B2A3u, { { 289, 49, 53, 2 }, { 289, 74, 79, 2 } } },     /* left stick lower | D-pad */
    { 0x07304847u, { { 289, 74, 79, 2 }, { 459, 74, 91, 2 } } },     /* D-pad | right stick */
    { 0x07304891u, { { 459, 74, 91, 2 }, { -2000, 0, 0, 0 } } },     /* right stick | - */
    { 0x07304851u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x07304852u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x07304842u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x07304850u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x07304849u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x0B6EA470u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x0B6EA471u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x0B6EA474u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x0B6EA472u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
    { 0x0B6EA476u, { { -2000, 0, 0, 0 }, { -2000, 0, 0, 0 } } },
};
#define GL_NT (sizeof k_gl_texts / sizeof k_gl_texts[0])
#define GL_NL (sizeof k_gl_lines / sizeof k_gl_lines[0])

/* Player 1's controller config, as the game reads it (sub_00051F10(0)):
 * 0 Default, else Pro; -1 unreadable. */
static int pad1_config(void)
{
    const uint8_t *p = guest(0x00346400u);
    uint32_t slot;
    if (!p)
        return -1;
    memcpy(&slot, p, 4);
    if (slot > 7 || !(p = guest(0x00367FE6u + 32u * slot)))
        return -1;
    return (int)(int8_t)p[0];
}

/* 1 patched, 0 not the page, -1 the page but not as expected. */
static int gl_patch_buf(uint8_t *b, uint32_t size)
{
    uint8_t *gd, *gp, *tpl[GL_NT], *txt[GL_NT], *lpl[GL_NL], *lel[GL_NL];
    uint32_t n, c, k, i;
    int cfg, ci;
    static int s_said;
    if (!(gp = ctl_find(b, size, 0x0E1D440Fu, 2)))
        return 0;
    memcpy(&n, gp + 0x20, 4);
    if (n != 30 || (gp[2] | gp[3] << 8) < 0x24u + 4u * 30u)
        return 0;                       /* Options' Pro group has 15 */
    for (k = 0; k < 30; k++) {
        memcpy(&c, gp + 0x24 + 4 * k, 4);
        if (c != k_gl_pro_was[k])
            return -1;
    }
    if (!(gd = ctl_find(b, size, 0x0920B2D4u, 2)))
        return -1;
    memcpy(&n, gd + 0x20, 4);
    if (n != 10)
        return -1;
    for (k = 0; k < 10; k++) {
        memcpy(&c, gd + 0x24 + 4 * k, 4);
        if (c != k_gl_def_was[k])
            return -1;
    }
    for (i = 0; i < GL_NT; i++)
        if (!(tpl[i] = ctl_find(b, size, k_gl_texts[i].id, 1)) || !ctl_prop(tpl[i], 0) || !ctl_prop(tpl[i], 1)
            || !ctl_prop(tpl[i], 0xC) || !(txt[i] = ctl_find(b, size, k_gl_texts[i].id, 0)))
            return -1;
    for (i = 0; i < GL_NL; i++) {
        static const unsigned need[] = { 0, 1, 6, 7, 0x15, 0x16, 0x1E, 0x1F, 0x27, 0x28, 0x30, 0x31 };
        if (!(lpl[i] = ctl_find(b, size, k_gl_lines[i].id, 1)) || !(lel[i] = ctl_find(b, size, k_gl_lines[i].id, 3)))
            return -1;
        for (k = 0; k < sizeof need / sizeof need[0]; k++)
            if (!ctl_prop(lpl[i], need[k]))
                return -1;
    }
    cfg = pad1_config();
    ci = cfg != 0;                      /* unreadable: Pro, the page as it is */
    if (cfg == 0) {                     /* Default shows the 30-slot group */
        const uint32_t idd = 0x0920B2D4u, idp = 0x0E1D440Fu;
        memcpy(gp + 4, &idd, 4);
        memcpy(gd + 4, &idp, 4);
    }
    for (i = 0; i < GL_NT; i++) {
        ctl_set(tpl[i], 0, k_gl_texts[i].x[ci]);
        if (k_gl_texts[i].al[ci] >= 0) {
            ctl_set(tpl[i], 1, k_gl_texts[i].y[ci]);
            ctl_set(tpl[i], 0xC, k_gl_texts[i].al[ci]);
        }
        if (k_gl_texts[i].s[ci])
            memcpy(txt[i] + 0x24, &k_gl_texts[i].s[ci], 4);
    }
    for (i = 0; i < GL_NL; i++) {
        const int16_t *q = k_gl_lines[i].r[ci];
        int w = q[2], h = q[3];
        uint8_t *r = lpl[i];
        ctl_set(r, 0, q[0]);
        if (!w)
            continue;
        ctl_set(r, 1, q[1]);
        ctl_set(r, 6, w); ctl_set(r, 7, h);
        ctl_set(r, 0x15, w); ctl_set(r, 0x16, h);
        ctl_set(r, 0x1E, 0); ctl_set(r, 0x1F, h);
        ctl_set(r, 0x27, 0); ctl_set(r, 0x28, 0);
        ctl_set(r, 0x30, w); ctl_set(r, 0x31, 0);
        for (k = 0; k < 4; k++) {
            ctl_set(r, 0x1A + 9 * k, 255);
            ctl_set(r, 0x1B + 9 * k, 255);
            ctl_set(r, 0x1C + 9 * k, 255);
            ctl_set(r, 0x1D + 9 * k, 255);
        }
        lel[i][8] = 0x0C;               /* over the picture */
    }
    if (s_said++ < 4)
        log_msg(LOG_WARN, "ICONS", "Loading screen: controller page laid out for player 1's %s config%s",
                ci ? "Pro" : "Default", cfg < 0 ? " (config unreadable: Pro assumed)" : "");
    return 1;
}

/* From the RefPack decoder (ssx3_overrides.c sub_0017C9C0), after each
 * chunk is unpacked. The chunk is 24576 bytes on the PAL disc; any
 * layout-sized chunk is looked at, the records decide. */
void ssx3_ControllerLayoutPatch(uint32_t dst, uint32_t size)
{
    /* any size: the layout may be unpacked inside a bigger block (the
     * 16-64 KB window missed it on the Switch, and only the late memory scan
     * patched it -- after the lines were built) */
    int r;
    uint8_t *h;
    uint32_t page, span;
    if (size < 0x1000u || size > 0x400000u || !ctl_enabled() || !(h = guest(dst)))
        return;
    /* guest RAM is mapped in 1 MB pages: read on only while the next page
     * follows this one in host memory too */
    span = 0x100000u - (dst & 0xFFFFFu);
    for (page = (dst & ~0xFFFFFu) + 0x100000u; span < size; page += 0x100000u, span += 0x100000u)
        if (guest(page) != h + span)
            break;
    if (span > size)
        span = size;
    s_ctl_chunks++;
    r = ctl_patch_buf(h, span, "as unpacked");
    if (r != 0) {
        static int said;
        if (said++ < 6)
            log_msg(LOG_WARN, "ICONS", "Options: Controller layout in an unpacked block (%08X, %u bytes, "
                    "chunk #%u): %s", dst, size, s_ctl_chunks, r > 0 ? "patched" : "did not match");
        return;
    }
    if (gl_patch_buf(h, span) < 0) {
        static int said;
        if (said++ < 2)
            log_msg(LOG_WARN, "ICONS", "Loading screen: controller page (%08X, %u bytes) not as expected: "
                    "left as it is", dst, size);
    }
}

/* Fallback, when Options (or Options: Controller) becomes the active screen
 * and the hook has not patched the layout yet: look for its records in
 * guest memory -- the first label's text record, then the layout around it
 * (+-32 KB) -- in main RAM and the contiguous window. Once per screen visit
 * at most; a few ms for 64 MB. Logs what it found either way. */
/* Options (or Options: Controller) became the active screen: only reports
 * when the layout has not been patched by then. There is no memory scan:
 * guest RAM on the Switch is a map of 1 MB pages, not one block, and a
 * linear walk from one host pointer ran into unmapped memory (a data abort
 * opening Controller Settings, 2026-11-25). The unpack hook is the patch. */
void ssx3_ControllerScreenSeen(int controller)
{
    static int s_warned;
    if (s_ctl_done || !ctl_enabled())
        return;
    if (s_warned++ < 2)
        log_msg(LOG_WARN, "ICONS", "Options%s opened: the Controller layout is not patched yet (%u "
                "blocks unpacked so far)", controller ? ": Controller" : "", s_ctl_chunks);
}
static DWORD WINAPI prompts_thread(LPVOID unused)
{
#if defined(__SWITCH__)
    { extern void xbox_nx_thread_name(const char *); xbox_nx_thread_name("PROMPTS"); }   /* SSX3NX_CPU_STATS */
#endif
    (void)unused;
    for (;;) {
        int n = prompts_tables();
        if (n && s_prompts_logged < 4) {
            s_prompts_logged++;
            log_msg(LOG_INFO, "ICONS", "riding prompts: %d table entr%s set to the PS2 buttons", n, n == 1 ? "y" : "ies");
        }
        Sleep(250);
    }
    return 0;
}

/* Called once guest memory holds the XBE (main.c, via ps2_controls.c). */
void ssx3_ButtonPromptsInit(void)
{
    size_t i;
    int renamed = 0;
    if (!ssx3_ButtonIcons() || !ssx3_Ps2Controls())
        return;
    for (i = 0; i < sizeof k_names / sizeof k_names[0]; i++)    /* all or nothing */
        if (!strcmp((const char *)guest(k_names[i].va), k_names[i].was))
            renamed++;
    if (renamed != (int)(sizeof k_names / sizeof k_names[0])) {
        log_msg(LOG_WARN, "ICONS", "riding prompts: %d of %d sprite names as expected; prompts unchanged",
                renamed, (int)(sizeof k_names / sizeof k_names[0]));
        return;
    }
    for (i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        memcpy(guest(k_names[i].va), k_names[i].now, strlen(k_names[i].now) + 1);
    CloseHandle(CreateThread(NULL, 0, prompts_thread, NULL, 0, NULL));
    log_msg(LOG_INFO, "ICONS", "riding prompts follow the PS2 layout: recovery Y, uber Y + grab, grabs ZL/L/R/ZR");
}
#endif /* BUTTON_ICONS_TEST */

/* ── Serving the patched files ─────────────────────────────────────────── */

typedef struct { char name[32]; int state; char out[512]; } Served;   /* 1 patched, -1 original */
static Served s_served[16];
static int s_nserved;

/* "Please don't turn off your Xbox console" (loading, saving and deleting a
 * profile; data\locale\cmn*.loc, four languages) names the Switch instead.
 * Every replacement is no longer than what it replaces and the rebuilt file
 * keeps the original size (padded at the end): the game formats these into
 * buffers of its own, and nothing may grow. SSX3NX_SWITCH_TEXT=0 keeps the
 * Xbox wording. */
static int switch_text(void)
{
    const char *e = getenv("SSX3NX_SWITCH_TEXT");
    return !(e && e[0] == '0');
}

static const struct { const char *was, *now; } k_loc_repl[] = {
    { "your Xbox console", "your Switch" },
    { "la console Xbox", "la Switch" },
    { "die Xbox-Konsole", "die Switch" },
    { "la consola Xbox", "la Switch" },
    /* #669, too few free blocks to save (all four languages) */
    { "Your Xbox doesn't have enough free blocks", "The Switch has too few free blocks" },
    { "La Xbox ne comporte pas", "La Switch n'a pas" },
    { "Die Xbox hat nicht gen\xfcgend", "Die Switch hat zu wenig" },
    { "Xbox no tiene suficientes bloques libres", "La Switch tiene pocos bloques libres" },
    /* #619 German, unusable save data (the others say just "hard disk") */
    { "auf der Xbox-Festplatte", "auf der Switch" },
};

/* UTF-16LE string at s (n units) with the replacements: into out (cap units), returns its length. */
static size_t loc_repl(const uint16_t *s, size_t n, uint16_t *out, size_t cap, int *hits)
{
    size_t i = 0, o = 0, r, k, wl;
    while (i < n) {
        int done = 0;
        for (r = 0; r < sizeof k_loc_repl / sizeof k_loc_repl[0] && !done; r++) {
            const char *w = k_loc_repl[r].was;
            wl = strlen(w);
            if (i + wl > n)
                continue;
            for (k = 0; k < wl && s[i + k] == (uint16_t)(unsigned char)w[k]; k++)
                ;
            if (k == wl) {
                const char *nw = k_loc_repl[r].now;
                for (k = 0; nw[k] && o < cap; k++)
                    out[o++] = (uint16_t)(unsigned char)nw[k];
                i += wl;
                (*hits)++;
                done = 1;
            }
        }
        if (!done && o < cap)
            out[o++] = s[i++];
        else if (!done)
            i++;
    }
    return o;
}

/* Rebuild a .loc (LOCH header, LOCT hash table, LOCL last: tag, size, 0,
 * count, offsets from the LOCL start, UTF-16 strings) with the replacements.
 * The new file in *out (malloc), 0 when nothing changed or it is not one. */
static size_t loc_patch(const uint8_t *b, size_t len, uint8_t **out)
{
    uint32_t locl, size, cnt, i;
    uint8_t *o;
    size_t cap, at;
    int hits = 0;
    if (len < 0x24 || memcmp(b, "LOCH", 4))
        return 0;
    memcpy(&locl, b + 0x10, 4);
    if ((size_t)locl + 16 > len || memcmp(b + locl, "LOCL", 4))
        return 0;
    memcpy(&size, b + locl + 4, 4);
    memcpy(&cnt, b + locl + 12, 4);
    if ((size_t)locl + size != len || cnt == 0 || cnt > 100000 || 16 + (size_t)cnt * 4 > size)
        return 0;
    cap = len + cnt * 64 + 64;
    if (!(o = (uint8_t *)malloc(cap)))
        return 0;
    memcpy(o, b, (size_t)locl + 16);    /* header, table, LOCL head */
    at = (size_t)locl + 16 + (size_t)cnt * 4;
    for (i = 0; i < cnt; i++) {
        uint32_t so, e;
        size_t n, m;
        uint16_t tmp[2048];
        memcpy(&so, b + locl + 16 + 4 * i, 4);
        if ((size_t)locl + so >= len) { free(o); return 0; }
        for (e = locl + so; e + 1 < len && (b[e] || b[e + 1]); e += 2)
            ;
        n = (e - (locl + so)) / 2;
        {
            uint32_t no = (uint32_t)(at - locl);
            memcpy(o + locl + 16 + 4 * i, &no, 4);
        }
        if (n < 1500) {
            uint16_t src[1500];
            memcpy(src, b + locl + so, n * 2);
            m = loc_repl(src, n, tmp, 2047, &hits);
            if (at + m * 2 + 4 > cap) { free(o); return 0; }
            memcpy(o + at, tmp, m * 2);
        } else {
            m = n;
            if (at + m * 2 + 4 > cap) { free(o); return 0; }
            memcpy(o + at, b + locl + so, m * 2);
        }
        at += m * 2;
        /* two terminators after each string, as the disc's files have */
        o[at++] = 0; o[at++] = 0; o[at++] = 0; o[at++] = 0;
    }
    if (!hits || at > len) { free(o); return 0; }
    while (at < len)                    /* the original size, exactly */
        o[at++] = 0;
    size = (uint32_t)(at - locl);
    memcpy(o + locl + 4, &size, 4);
    *out = o;
    return at;
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

static int build(const host_char *orig, const char *name, char *out, size_t outn, int is_loc)
{
    FILE *f = open_host(orig, "rb"), *o;
    uint8_t *buf;
    long len;
    char dir[400];
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (16 << 20) || !(buf = (uint8_t *)malloc((size_t)len))) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);
    if (is_loc) {
        uint8_t *nb = NULL;
        size_t nl = loc_patch(buf, (size_t)len, &nb);
        free(buf);
        if (!nl)
            return 0;
        buf = nb;
        len = (long)nl;
    } else if (!shpx_patch(buf, (size_t)len)) {
        free(buf);
        return 0;
    }
#if defined(__SWITCH__)
    snprintf(dir, sizeof dir, "sdmc:/switch/ssx3nx/icons");
#else
    snprintf(dir, sizeof dir, "icons");
#endif
    mkdir_one(dir);
    snprintf(out, outn, "%s/%s", dir, name);
    o = fopen(out, "wb");
    if (!o || fwrite(buf, 1, (size_t)len, o) != (size_t)len) {
        if (o) fclose(o);
        log_msg(LOG_WARN, "ICONS", "cannot write %s: the Xbox button prompts", out);
        free(buf);
        return 0;
    }
    fclose(o);
    free(buf);
    if (is_loc)
        log_msg(LOG_INFO, "ICONS", "%s: \"turn off your Xbox console\" names the Switch", out);
    else
        log_msg(LOG_INFO, "ICONS", "Switch button prompts in %s (%s face buttons)", out,
                ssx3_Ps2Controls() ? "by label" : "by position");
    return 1;
}

/* Called for every path the game opens (ps2_controls.c's redirect). */
void ssx3_ButtonIconsRedirect(const char *xbox_path, host_char *host, DWORD n)
{
    const char *base, *p;
    char name[32];
    size_t k = 0;
    int i;
    Served *s = NULL;
    if (!xbox_path || !ssx3_ButtonIcons())
        return;
    base = xbox_path;
    for (p = xbox_path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    int is_loc;
    for (p = base; *p && k + 1 < sizeof name; p++)
        name[k++] = (char)tolower((unsigned char)*p);
    name[k] = 0;
    /* the UI shape files, ui\<name>.xsh, and the common strings,
     * locale\cmn<language>.loc */
    is_loc = k > 7 && !strncmp(name, "cmn", 3) && !strcmp(name + k - 4, ".loc") && switch_text();
    if (!is_loc) {
        if (base - xbox_path < 4)
            return;
        if (tolower((unsigned char)base[-3]) != 'u' || tolower((unsigned char)base[-2]) != 'i'
            || (base[-4] != '\\' && base[-4] != '/'))
            return;
        if (k < 5 || strcmp(name + k - 4, ".xsh"))
            return;
    }
    for (i = 0; i < s_nserved; i++)
        if (!strcmp(s_served[i].name, name))
            s = &s_served[i];
    if (!s) {
        if (s_nserved == (int)(sizeof s_served / sizeof s_served[0]))
            return;
        s = &s_served[s_nserved++];
        snprintf(s->name, sizeof s->name, "%s", name);
        s->state = build(host, name, s->out, sizeof s->out, is_loc) ? 1 : -1;
    }
    if (s->state != 1)
        return;
#ifdef _WIN32
    MultiByteToWideChar(CP_ACP, 0, s->out, -1, host, (int)n);
#else
    snprintf(host, n, "%s", s->out);
#endif
}

#ifdef BUTTON_ICONS_TEST
/* Host test: patch a file and write the result. */
int ssx3_Ps2Controls(void) { return getenv("PS2") != NULL; }
void log_msg(int lvl, const char *tag, const char *fmt, ...) { (void)lvl; (void)tag; (void)fmt; }
int main(int argc, char **argv)
{
    FILE *f, *o;
    long len;
    uint8_t *b;
    if (argc == 4 && !strcmp(argv[1], "pro")) {
        /* the controller picture, raw RGBA at a scale: pro <scale> <out> */
        int s = atoi(argv[2]);
        size_t n = (size_t)PRO_W * s * PRO_H * s * 4;
        uint8_t *px = calloc(1, n);
        pro_render(px, PRO_W * s, s);
        o = fopen(argv[3], "wb"); fwrite(px, 1, n, o); fclose(o);
        printf("%d x %d\n", PRO_W * s, PRO_H * s);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "loc")) {
        /* the strings rewrite: loc <in.loc> <out.loc> */
        uint8_t *nb = NULL;
        size_t nl;
        f = fopen(argv[2], "rb");
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        b = malloc(len); fread(b, 1, len, f); fclose(f);
        nl = loc_patch(b, (size_t)len, &nb);
        printf("loc %ld -> %zu\n", len, nl);
        if (nl) { o = fopen(argv[3], "wb"); fwrite(nb, 1, nl, o); fclose(o); }
        return 0;
    }
    if (argc < 3)
        return 1;
    f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(len);
    fread(b, 1, len, f); fclose(f);
    printf("patched %d\n", shpx_patch(b, len));
    o = fopen(argv[2], "wb"); fwrite(b, 1, len, o); fclose(o);
    return 0;
}
#endif
