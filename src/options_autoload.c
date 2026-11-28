/*
 * Saved options at boot (RECOMP_OPTIONS_AUTOLOAD, default on; 0 = off).
 *
 * SSX 3 keeps three kinds of save: game (0x9B61 bytes), replay and options
 * (0xA4C bytes, sub_00054C40), each a UDATA\<title>\<id>\Data.ssx with a
 * 20-byte signature after the data. Options are saved and loaded only from
 * Options > Save/Load. The boot autoload screen (vtable 0028B428, this =
 * [0x3684A8]) scans all three types and finds the options save, but loads the
 * game save only (sub_0008CC90): language, units and the rest come back as
 * the defaults every time the game starts, though "Save Options" worked.
 *
 * So as the frontend starts, and again once that screen has closed (see the
 * tick below), this loads the newest options save the way Load Options does
 * once its read is done
 * (sub_00078D50, type 2): context 5, sub_00054C60(buffer) -- which checks
 * the data's own CRC16 (seed 0xFBEA, its last dword) before taking anything
 * from it -- and the sound settings applied from [0x367FBC].
 */
#include "log.h"

#ifdef _WIN32
#include <windows.h>
typedef WCHAR host_char;
#else
#include "platform/win32_compat.h"
#include <dirent.h>
typedef char host_char;
#endif
#include "recomp/gen/recomp_types.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern BOOL xbox_translate_path(const char *xbox_path, host_char *host_path_buf, DWORD buf_size);

extern void sub_0004BD50(void);
extern void sub_0004BBA0(void);
extern void sub_00054C60(void);
extern void sub_000768D0(void);
extern void sub_000FD470(void);
extern void sub_0010B380(void);
extern void sub_0010B3D0(void);
extern void sub_0010B430(void);
extern void sub_0010B450(void);
extern void sub_0010B4A0(void);

#define OPT_SIZE   0xA4Cu                 /* sub_00054C40 */
#define OPT_FILE   (OPT_SIZE + 20u)       /* + the save signature */
#define TITLE_UDATA "\\Device\\Harddisk0\\Partition1\\UDATA\\4541003c"

/* The newest <id>\Data.ssx of options size in the title's save folder:
 * its first OPT_SIZE bytes into buf. 1 if found; name gets the folder id. */
static int read_options_save(unsigned char *buf, char *name, size_t name_n)
{
    host_char root[600];
    int found = 0;
    long long best = -1;
    if (!xbox_translate_path(TITLE_UDATA, root, 600))
        return 0;
#ifdef _WIN32
    {
        WCHAR pat[700], best_path[700] = L"", p[700];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        _snwprintf(pat, 700, L"%s\\*", root);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return 0;
        do {
            struct _stat64 st;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            _snwprintf(p, 700, L"%s\\%s\\Data.ssx", root, fd.cFileName);
            if (_wstat64(p, &st) == 0 && st.st_size == OPT_FILE && (long long)st.st_mtime > best) {
                best = (long long)st.st_mtime;
                wcscpy(best_path, p);
                _snprintf(name, name_n, "%S", fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (best >= 0) {
            FILE *f = _wfopen(best_path, L"rb");
            if (f) {
                found = fread(buf, 1, OPT_SIZE, f) == OPT_SIZE;
                fclose(f);
            }
        }
    }
#else
    {
        char best_path[700] = "", p[700];
        DIR *d = opendir(root);
        struct dirent *de;
        if (!d)
            return 0;
        while ((de = readdir(d)) != NULL) {
            struct stat st;
            if (de->d_name[0] == '.')
                continue;
            snprintf(p, sizeof p, "%s/%s/Data.ssx", root, de->d_name);
            if (stat(p, &st) == 0 && st.st_size == OPT_FILE && (long long)st.st_mtime > best) {
                best = (long long)st.st_mtime;
                snprintf(best_path, sizeof best_path, "%s", p);
                snprintf(name, name_n, "%s", de->d_name);
            }
        }
        closedir(d);
        if (best >= 0) {
            FILE *f = fopen(best_path, "rb");
            if (f) {
                found = fread(buf, 1, OPT_SIZE, f) == OPT_SIZE;
                fclose(f);
            }
        }
    }
#endif
    return found;
}

/* Context n, as sub_0004BD50 + sub_0004BBA0 (which pops the argument). */
static void set_context(uint32_t n)
{
    PUSH32(g_esp, n);
    PUSH32(g_esp, 0);
    sub_0004BD50();
    g_ecx = g_eax;
    PUSH32(g_esp, 0);
    sub_0004BBA0();
}

/* One sound setting: push v, [sub_000FD470] -> this, fn (ret 4). */
static void sound_set(void (*fn)(void), uint32_t v)
{
    PUSH32(g_esp, v);
    PUSH32(g_esp, 0);
    sub_000FD470();
    g_ecx = g_eax;
    PUSH32(g_esp, 0);
    fn();
}

static void load_options(int quiet)
{
    uint32_t sv_eax = g_eax, sv_ebx = g_ebx, sv_ecx = g_ecx, sv_edx = g_edx;
    uint32_t sv_esi = g_esi, sv_edi = g_edi, sv_ebp = g_ebp, sv_esp = g_esp;
    /* the buffer below the live stack, the calls' frames below it */
    uint32_t buf = (g_esp - 0x4000u) & ~0xFu;
    unsigned char data[OPT_SIZE];
    char name[64] = "";
    int ok;

    if (!read_options_save(data, name, sizeof name)) {
        log_msg(LOG_INFO, "OPTIONS", "no options save: the game's defaults stay");
        return;
    }
    {
        /* per dword, through the page map (the buffer may cross a page) */
        uint32_t k, w;
        for (k = 0; k < OPT_SIZE; k += 4) {
            memcpy(&w, data + k, 4);
            ST32(buf + k, w);
        }
    }

    g_esp = buf;
    set_context(5);
    g_esp = buf;
    PUSH32(g_esp, buf);
    PUSH32(g_esp, 0);
    sub_00054C60();
    ok = (g_eax & 0xFF) != 0;

    if (ok) {
        uint32_t fbc;
        g_esp = buf;
        PUSH32(g_esp, 0);
        sub_000FD470();
        if (g_eax) {
            set_context(4);
            fbc = MEM32(0x00367FBCu);
            g_esp = buf;
            PUSH32(g_esp, (fbc >> 12) & 3);
            PUSH32(g_esp, 0);
            sub_000768D0();
            g_esp = buf;
            sound_set(sub_0010B380, fbc & 0xF);
            sound_set(sub_0010B3D0, (fbc >> 8) & 0xF);
            sound_set(sub_0010B430, (fbc >> 4) & 0xF);
            sound_set(sub_0010B450, (fbc & 0x20000) != 0);
            sound_set(sub_0010B4A0, (fbc & 0x10000) != 0);
            g_esp = buf;
            set_context(5);                  /* the autoload's context, as it left it */
        }
    }

    g_eax = sv_eax; g_ebx = sv_ebx; g_ecx = sv_ecx; g_edx = sv_edx;
    g_esi = sv_esi; g_edi = sv_edi; g_ebp = sv_ebp; g_esp = sv_esp;
    if (ok) {
        if (!quiet)
            log_msg(LOG_WARN, "OPTIONS", "saved options loaded at boot (UDATA\\4541003c\\%s)", name);
    } else
        log_msg(LOG_WARN, "OPTIONS", "options save UDATA\\4541003c\\%s failed its check: the game's defaults stay", name);
}

/* Each app tick (sub_001AD720), before the tick's update. Twice:
 *  - on the first tick the frontend is the app's state (app [0x3A930C] +4,
 *    vtable 00289618), before its first update builds the "Press START"
 *    title screen: every screen's text, the title's too, comes from the
 *    locale loaded for the language set then ([0x367FBC] >> 22 & 7, which
 *    sub_00054C60 applies through sub_000E1C20);
 *  - once the boot autoload screen (vtable 0028B428, [0x3684A8]) has closed,
 *    after the game save it loaded has been applied, so the saved options
 *    win over anything that save carries. */
void ssx3_OptionsAutoloadTick(void)
{
    static int state = -1;      /* -1 unset, 0 waiting, 1 applied, 2 autoload up, 3 done */
    uint32_t app, cur, al;
    if (state < 0) {
        const char *e = getenv("RECOMP_OPTIONS_AUTOLOAD");
        state = e && e[0] == '0' ? 3 : 0;
    }
    if (state == 3)
        return;
    if (state == 0) {
        app = MEM32(0x003A930Cu);
        cur = app >= 0x10000u ? MEM32(app + 4) : 0;
        if (cur >= 0x10000u && MEM32(cur) == 0x00289618u && MEM32(0x00369534u) >= 0x10000u) {
            state = 1;
            load_options(0);
        }
        return;
    }
    al = MEM32(0x003684A8u);
    if (state == 1) {
        if (al >= 0x10000u && MEM32(al) == 0x0028B428u)
            state = 2;
        return;
    }
    if (al == 0) {
        state = 3;
        load_options(1);
    }
}
