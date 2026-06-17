/*
 * titlescan.c  –  Scan UDATA and TDATA for Xbox title directories.
 *
 * Reads TitleMeta.xbx (UTF-16 LE) to extract the TitleName field.
 * Falls back to the raw TitleID if the file is missing or unreadable.
 */

#include "titlescan.h"
#include <string.h>
#include <windows.h>
#include <stdio.h>

/* ── UTF-16 LE → ASCII ────────────────────────────────────────────────────
 * Reads a UTF-16 LE file (with or without BOM) into an ASCII buffer.
 * Non-ASCII code points are replaced with '?'.
 */
static int utf16le_to_ascii(const unsigned char *src, int src_len,
                             char *dst, int dst_sz)
{
    int i = 0, j = 0;

    /* Skip BOM (FF FE) if present */
    if (src_len >= 2 && src[0] == 0xFF && src[1] == 0xFE)
        i = 2;

    while (i + 1 < src_len && j < dst_sz - 1) {
        unsigned int cp = src[i] | ((unsigned int)src[i + 1] << 8);
        i += 2;
        if (cp == '\r') continue;           /* strip CR */
        dst[j++] = (cp < 0x80) ? (char)cp : '?';
    }
    dst[j] = '\0';
    return j;
}

/* ── Read TitleName from TitleMeta.xbx ────────────────────────────────────
 * Returns 1 if found, 0 if not.
 */
static int read_title_name(const char *meta_path,
                            char *name_out, int name_sz)
{
    HANDLE hf = CreateFile(meta_path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return 0;

    unsigned char raw[1024];
    DWORD bytes_read = 0;
    ReadFile(hf, raw, sizeof(raw), &bytes_read, NULL);
    CloseHandle(hf);

    if (bytes_read < 2)
        return 0;

    /* Convert UTF-16 LE to ASCII */
    char ascii[512];
    utf16le_to_ascii(raw, (int)bytes_read, ascii, sizeof(ascii));

    /* Find TitleName= line */
    char *p = ascii;
    while (*p) {
        /* Find start of line */
        char *line = p;

        /* Find end of line */
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char saved = *eol;
        *eol = '\0';

        if (strncmp(line, "TitleName=", 10) == 0) {
            const char *val = line + 10;
            /* Trim trailing whitespace */
            int len = (int)strlen(val);
            while (len > 0 && (val[len-1] == ' ' || val[len-1] == '\t' ||
                                val[len-1] == '\r'))
                len--;
            if (len > 0) {
                int copy = len < name_sz - 1 ? len : name_sz - 1;
                memcpy(name_out, val, copy);
                name_out[copy] = '\0';
                return 1;
            }
        }

        *eol = saved;
        p = (*eol == '\n') ? eol + 1 : eol;
    }

    return 0;
}

/* ── Find or create entry for a TitleID ───────────────────────────────────*/
static TitleEntry *find_or_add(TitleEntry *entries, int *count, int max,
                                const char *title_id)
{
    for (int i = 0; i < *count; i++) {
        if (strcmp(entries[i].title_id, title_id) == 0)
            return &entries[i];
    }
    if (*count >= max)
        return NULL;

    TitleEntry *e = &entries[(*count)++];
    memset(e, 0, sizeof(*e));
    strncpy(e->title_id, title_id, TITLE_ID_LEN - 1);
    return e;
}

/* ── Scan one data directory (UDATA or TDATA) ────────────────────────────*/
static void scan_dir(const char *base_path, int is_udata,
                     TitleEntry *entries, int *count, int max)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", base_path);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        /* Skip . and .. and non-directories */
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0)
            continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        /* TitleID must be exactly 8 hex chars */
        if (strlen(fd.cFileName) != 8)
            continue;

        TitleEntry *e = find_or_add(entries, count, max, fd.cFileName);
        if (!e) break;

        if (is_udata) e->has_udata = 1;
        else          e->has_tdata = 1;

        /* Only read TitleMeta.xbx if we don't have a name yet */
        if (e->title_name[0] == '\0') {
            char meta[MAX_PATH];
            snprintf(meta, sizeof(meta), "%s\\%s\\TitleMeta.xbx",
                     base_path, fd.cFileName);
            if (!read_title_name(meta, e->title_name, TITLE_NAME_LEN)) {
                /* Fallback: use TitleID as name */
                strncpy(e->title_name, fd.cFileName, TITLE_NAME_LEN - 1);
            }
        }

    } while (FindNextFile(h, &fd));

    FindClose(h);
}

/* ── Public API ───────────────────────────────────────────────────────────*/
int titlescan_scan(TitleEntry *entries, int max)
{
    int count = 0;

    scan_dir("E:\\UDATA", 1, entries, &count, max);
    scan_dir("E:\\TDATA", 0, entries, &count, max);

    return count;
}
