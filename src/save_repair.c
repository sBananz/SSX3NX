/*
 * An empty SaveMeta.xbx no longer locks a save out.
 *
 * Every Xbox save folder (UDATA\<title>\<id>) has a SaveMeta.xbx: UTF-16,
 * BOM, "Name=<save name>\r\n". The folder id is derived from the name, so a
 * name always maps to the same folder. XCreateSaveGame (sub_001802FC) creates
 * the folder; when it exists (overwriting a save) it checks the folder's
 * SaveMeta.xbx with sub_00180220: open, read the BOM, read Name=, compare with
 * the name asked for. Any failure is 0x491, which SSX 3 shows as "save data is
 * unusable" -- for saving, loading and deleting alike, since its delete also
 * goes through XCreateSaveGame first.
 *
 * Saves left with a 0-byte SaveMeta.xbx (by the kernel file layer's old delete
 * fallback, which emptied a file the SD card would not unlink while open)
 * failed that check for good. gen_guards.py puts a test at the check's failure
 * label: when the file is empty, the folder already proves it belongs to the
 * name asked for, so SaveMeta.xbx is written again with that name -- exactly
 * what XCreateSaveGame writes for a new save -- and the check passes. A
 * SaveMeta.xbx with content that does not match is still refused, as on the
 * Xbox.
 */
#include "log.h"

#ifdef _WIN32
#include <windows.h>
typedef WCHAR host_char;
#else
#include "platform/win32_compat.h"
typedef char host_char;
#endif
#include <sys/stat.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern BOOL xbox_translate_path(const char *xbox_path, host_char *host_path_buf, DWORD buf_size);
extern void *xbox_GuestToHost(uint32_t va);

/* The guest's ANSI path, copied out (NULL if unusable). */
static const char *guest_path(uint32_t va, char *buf, size_t n)
{
    const char *s = va ? (const char *)xbox_GuestToHost(va) : NULL;
    size_t k;
    if (!s)
        return NULL;
    for (k = 0; k + 1 < n && s[k]; k++)
        buf[k] = s[k];
    buf[k] = 0;
    return k ? buf : NULL;
}

static int host_path(uint32_t va, host_char *out, DWORD n, char *xp, size_t xn)
{
    return guest_path(va, xp, xn) && xbox_translate_path(xp, out, n);
}

static int host_size(const host_char *p, long long *size)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(p, &st) != 0) return 0;
#else
    struct stat st;
    if (stat(p, &st) != 0) return 0;
#endif
    *size = (long long)st.st_size;
    return 1;
}

/* 1 when the SaveMeta.xbx at the guest path exists and is empty. */
int ssx3_SaveMetaEmpty(uint32_t path_va)
{
    host_char p[600];
    char xp[300];
    long long size;
    return host_path(path_va, p, 600, xp, sizeof xp) && host_size(p, &size) && size == 0;
}

/* Write SaveMeta.xbx (BOM, Name=<name>\r\n) at the guest path, the name a
 * guest UTF-16 string. The handle the check opened is closed by then. */
void ssx3_SaveMetaWrite(uint32_t path_va, uint32_t name_va)
{
    host_char p[600];
    char xp[300], shown[64];
    uint16_t buf[160];
    const uint16_t *name = name_va ? (const uint16_t *)xbox_GuestToHost(name_va) : NULL;
    size_t n = 0, k;
    FILE *f;
    if (!host_path(path_va, p, 600, xp, sizeof xp) || !name)
        return;
    buf[n++] = 0xFEFF;
    for (k = 0; k < 4; k++) buf[n++] = (uint16_t)"Name"[k];
    buf[n++] = '=';
    for (k = 0; name[k] && n < 150; k++) {
        buf[n++] = name[k];
        if (k + 1 < sizeof shown) shown[k] = name[k] < 0x80 ? (char)name[k] : '?';
    }
    shown[k < sizeof shown ? k : sizeof shown - 1] = 0;
    buf[n++] = '\r';
    buf[n++] = '\n';
#ifdef _WIN32
    f = _wfopen(p, L"wb");
#else
    f = fopen(p, "wb");
#endif
    if (!f || fwrite(buf, 2, n, f) != n) {
        if (f) fclose(f);
        log_msg(LOG_WARN, "SAVE", "%s was empty and could not be rewritten", xp);
        return;
    }
    fclose(f);
    log_msg(LOG_WARN, "SAVE", "%s was empty (a damaged save): rewritten for \"%s\", the save is usable again",
            xp, shown);
}
