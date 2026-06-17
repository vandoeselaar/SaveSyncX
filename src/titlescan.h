#ifndef TITLESCAN_H
#define TITLESCAN_H

#define TITLESCAN_MAX        512
#define TITLE_ID_LEN         9    /* 8 hex chars + null */
#define TITLE_NAME_LEN       64

typedef struct {
    char title_id[TITLE_ID_LEN];
    char title_name[TITLE_NAME_LEN];
    int  has_udata;
    int  has_tdata;
} TitleEntry;

/*
 * titlescan_scan  –  Scan UDATA and TDATA for title directories.
 * Fills entries[0..max-1]; returns number of entries found, or -1 on error.
 * Entries are deduplicated by TitleID (has_udata/has_tdata both set if both exist).
 */
int titlescan_scan(TitleEntry *entries, int max);

#endif /* TITLESCAN_H */
