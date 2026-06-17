/*
 * util.c  –  Path and string helpers
 */

#include "util.h"
#include <string.h>
#include <stdarg.h>
#include <windows.h>

/*
 * util.c  –  Path and string helpers
 */

#include "util.h"
#include <string.h>
#include <stdarg.h>
#include <windows.h>

/* ── log_print ───────────────────────────────────────────────────── */
/*
 * Schrijft naar C:\savesyncx.log.
 *
 * Strategie: houd de bestandshandle open voor de hele levensduur van
 * de app. Eén WriteFile-aanroep per regel is atomair genoeg op FATX
 * voor buffers kleiner dan een sector (512 bytes). Geen CRITICAL_SECTION
 * nodig omdat nxdk die niet aanbiedt; we bouwen de volledige regel
 * (tijdstempel + bericht + \r\n) in één char-array zodat er maar één
 * enkele kernel-aanroep nodig is.
 */

static HANDLE s_log_handle = INVALID_HANDLE_VALUE;

static void open_log(const char *path)
{
    s_log_handle = CreateFile(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL);

    if (s_log_handle == INVALID_HANDLE_VALUE) return;

    const char *header = "=== SaveSyncX log start ===\r\n";
    DWORD w;
    WriteFile(s_log_handle, header, (DWORD)strlen(header), &w, NULL);
}

/* Initialiseer de logger met een expliciet pad */
void log_init_path(const char *path)
{
    open_log(path);
}

/*
 * Initialiseer de logger zonder expliciet pad.
 * Probeer E:\Apps\SaveSyncX, E:\Applications\SaveSyncX, dan C: als
 * laatste redmiddel (C: is altijd beschrijfbaar, ook zonder E-schijf).
 */
void log_init(void)
{
    static const char *candidates[] = {
        "E:\\Apps\\SaveSyncX\\savesyncx.log",
        "E:\\Applications\\SaveSyncX\\savesyncx.log",
        "C:\\savesyncx.log",
    };
    for (int i = 0; i < 3; i++) {
        open_log(candidates[i]);
        if (s_log_handle != INVALID_HANDLE_VALUE) return;
    }
}

void log_print(const char *fmt, ...)
{
    if (s_log_handle == INVALID_HANDLE_VALUE) return;

    char line[600];

    int prefix_len = snprintf(line, sizeof(line),
                              "[%lu] ", (unsigned long)GetTickCount());
    if (prefix_len < 0) prefix_len = 0;
    if (prefix_len >= (int)sizeof(line)) prefix_len = (int)sizeof(line) - 1;

    va_list ap;
    va_start(ap, fmt);
    int msg_len = vsnprintf(line + prefix_len,
                            (int)sizeof(line) - prefix_len, fmt, ap);
    va_end(ap);

    int total = prefix_len + (msg_len > 0 ? msg_len : 0);
    if (total >= (int)sizeof(line) - 3)
        total = (int)sizeof(line) - 3;

    /* Normaliseer naar \r\n */
    if (total > 0 && line[total - 1] == '\n') total--;
    if (total > 0 && line[total - 1] == '\r') total--;
    line[total++] = '\r';
    line[total++] = '\n';
    line[total]   = '\0';

    DWORD written;
    WriteFile(s_log_handle, line, (DWORD)total, &written, NULL);
}


void util_forward_slash(char *path)
{
    for (char *p = path; *p; p++)
        if (*p == '\\') *p = '/';
}

/* Convert forward slashes to backslashes in-place */
void util_backslash(char *path)
{
    for (char *p = path; *p; p++)
        if (*p == '/') *p = '\\';
}

/* Copy directory part of path into out */
void util_dirname(const char *path, char *out, int out_sz)
{
    strncpy(out, path, out_sz - 1);
    out[out_sz - 1] = '\0';
    char *last = NULL;
    for (char *p = out; *p; p++)
        if (*p == '\\' || *p == '/') last = p;
    if (last) *last = '\0';
    else      out[0] = '\0';
}

/* Case-insensitive string compare (POSIX strcasecmp niet beschikbaar in nxdk) */
int util_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int d = (unsigned char)(*a | 0x20) - (unsigned char)(*b | 0x20);
        if (d) return d;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int util_strncasecmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *b) {
        int d = (unsigned char)(*a | 0x20) - (unsigned char)(*b | 0x20);
        if (d) return d;
        a++; b++;
    }
    return n == (size_t)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}

/* Format byte count as human-readable string, e.g. "1.23 MB" */
void util_format_bytes(size_t bytes, char *out, int out_sz)
{
    if (bytes < 1024)
        snprintf(out, out_sz, "%u B", (unsigned)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, out_sz, "%.1f KB", bytes / 1024.0);
    else
        snprintf(out, out_sz, "%.2f MB", bytes / (1024.0 * 1024.0));
}
