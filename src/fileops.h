#ifndef FILEOPS_H
#define FILEOPS_H

#include "config.h"
#include <stddef.h>

/* ── FileEntry ───────────────────────────────────────────────────────────── */
typedef struct {
    char  local_path[MAX_PATH_LEN];   /* Full local Xbox path            */
    char  rel_path[MAX_PATH_LEN];     /* Relative to TDATA or UDATA root */
    int   is_dir;
    size_t size;
} FileEntry;

/*
 * fileops_scan  –  Recursively scan a local directory.
 * Fills entries[0..max-1]; returns count or <0 on error.
 */
int fileops_scan(const char *root, FileEntry entries[], int max);

/*
 * fileops_ensure_dir  –  Create a local directory (and parents) if missing.
 */
int fileops_ensure_dir(const char *path);

/*
 * fileops_copy  –  Copy a file from src to dst, creating dirs as needed.
 */
int fileops_copy(const char *src, const char *dst);

/*
 * fileops_total_size  –  Sum bytes of all non-directory entries.
 */
size_t fileops_total_size(const FileEntry entries[], int count);

#endif /* FILEOPS_H */
