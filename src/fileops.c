/*
 * fileops.c  –  Filesystem helpers for the Xbox (nxdk / Win32-compat API)
 */

#include "fileops.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>  /* nxdk provides FindFirstFile / CreateDirectory etc. */
#include <hal/debug.h>
#include <stdint.h>

/* ── Recursive directory scan ────────────────────────────────────────────── */
static int scan_recursive(const char *base_root,
                           const char *cur_dir,
                           FileEntry entries[],
                           int max,
                           int *count)
{
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", cur_dir);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int base_len = (int)strlen(base_root);

    do {
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) continue;

        if (*count >= max) break;

        FileEntry *e = &entries[*count];

        /* Full local path */
        snprintf(e->local_path, MAX_PATH_LEN, "%s\\%s",
                 cur_dir, fd.cFileName);

        /* Relative path (strip root prefix + leading \) */
        const char *rel = e->local_path + base_len;
        while (*rel == '\\') rel++;
        strncpy(e->rel_path, rel, MAX_PATH_LEN - 1);
        /* Use forward slashes for remote paths */
        util_forward_slash(e->rel_path);

        e->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        e->size   = e->is_dir ? 0 :
                    ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;

        (*count)++;

        if (e->is_dir)
            scan_recursive(base_root, e->local_path, entries, max, count);

    } while (FindNextFile(h, &fd));

    FindClose(h);
    return 0;
}

int fileops_scan(const char *root, FileEntry entries[], int max)
{
    int count = 0;
    scan_recursive(root, root, entries, max, &count);
    return count;
}

/* ── Ensure local directory exists (creates parents) ─────────────────────── */
int fileops_ensure_dir(const char *path)
{
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, sizeof(tmp) - 1);

    /* Walk path segments */
    for (char *p = tmp + 3; *p; p++) {   /* skip drive letter e.g. "E:\" */
        if (*p == '\\' || *p == '/') {
            char save = *p;
            *p = '\0';
            CreateDirectory(tmp, NULL);  /* ignore errors – may already exist */
            *p = save;
        }
    }
    return CreateDirectory(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS
           ? 0 : -1;
}

/* ── Simple file copy ────────────────────────────────────────────────────── */
int fileops_copy(const char *src, const char *dst)
{
    /* Ensure destination directory */
    char dir[MAX_PATH_LEN];
    util_dirname(dst, dir, sizeof(dir));
    fileops_ensure_dir(dir);

    return CopyFile(src, dst, FALSE) ? 0 : -1;
}

/* ── Total byte count ────────────────────────────────────────────────────── */
size_t fileops_total_size(const FileEntry entries[], int count)
{
    size_t total = 0;
    for (int i = 0; i < count; i++)
        if (!entries[i].is_dir) total += entries[i].size;
    return total;
}
