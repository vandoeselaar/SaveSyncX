#include "../lib/ui.h"
#include "../lib/util.h"
#include "backup.h"
#include "config.h"
#include "webdav.h"
#include "fileops.h"
#include "titlescan.h"
#include "titledb.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ── Genereer timestamp string (YYYY-MM-DD_HH-MM-SS) ───────────── */
static void get_timestamp(char *buf, size_t bufsz)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, bufsz, "%04d-%02d-%02d_%02d-%02d-%02d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
}

/* ── Verwijder een lokale map recursief ─────────────────────────── */
static int delete_local_dir(const char *path)
{
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) continue;

        char fullpath[MAX_PATH_LEN];
        snprintf(fullpath, sizeof(fullpath), "%s\\%s", path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += delete_local_dir(fullpath);
            SetFileAttributes(fullpath, FILE_ATTRIBUTE_NORMAL);
            RemoveDirectory(fullpath);
        } else {
            SetFileAttributes(fullpath, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFile(fullpath)) count++;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
    SetFileAttributes(path, FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectory(path)) {
        SDL_Delay(100);
        RemoveDirectory(path);
    }
    return count;
}

/* ── Teken de backup lijstpagina ────────────────────────────────── */
static void draw_list(const TitleEntry *titles, int n,
                      int selected, const char *status)
{
    int page       = selected / LIST_PAGE_SIZE;
    int page_start = page * LIST_PAGE_SIZE;
    int page_end   = page_start + LIST_PAGE_SIZE;
    if (page_end > n) page_end = n;
    int total_pages = (n + LIST_PAGE_SIZE - 1) / LIST_PAGE_SIZE;

    ui_clear();
    ui_draw_text(MARGIN_X, MARGIN_Y, "  " APP_TITLE "  --  Backup", UI_COL_TITLE);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "page %d / %d   (%d titles)",
             page + 1, total_pages, n);
    ui_draw_text(MARGIN_X, MARGIN_Y + CHAR_H, hdr, UI_COL_DIM);

    ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H * 2 + 4,
                 SCREEN_W - MARGIN_X * 2, 1, UI_COL_DIM);

    for (int i = page_start; i < page_end; i++) {
        int row = i - page_start;
        int y   = MARGIN_Y + CHAR_H * (4 + row);
        int sel = (i == selected);

        if (sel)
            ui_fill_rect(MARGIN_X - 4, y - 2,
                         SCREEN_W - MARGIN_X * 2 + 8, CHAR_H + 4, UI_COL_SELBG);

        char line[80];
        uint32_t tid_num = (uint32_t)strtoul(titles[i].title_id, NULL, 16);
        const char *db_name = title_lookup(tid_num);
        const char *display_name = db_name ? db_name : titles[i].title_name;
        snprintf(line, sizeof(line), "%s [%s] %s",
                 sel ? ">" : " ",
                 titles[i].title_id,
                 display_name);
        ui_draw_text(MARGIN_X, y, line, sel ? UI_COL_SELECT : UI_COL_ITEM);
    }

    if (status && status[0])
        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H * 2,
                     status, UI_COL_OK);

    ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                 "up/down=scroll  A=backup  Y=delete save  B=back", UI_COL_DIM);
    ui_flip();
}

/* ── Backup menu hoofdfunctie ───────────────────────────────────── */
void do_backup(TitleEntry *titles, int *n,
               const char *creds64, const AppConfig *cfg)
{
    int selected = 0;
    char status_msg[80] = "";
    int redraw = 1;
    static ScrollState scroll = {0};   /* eigen state, niet gedeeld met restore-scherm */

    while (1) {
        ui_pump_events();

        int delta = scroll_update(&scroll, *n);
        if (delta != 0) {
            scroll_apply(&selected, delta, *n);
            status_msg[0] = '\0';
            redraw = 1;
        }

        if (btn_pressed(BTN_B)) return;

        /* ── A: upload save naar server ─────────────────────────── */
        if (btn_pressed(BTN_A)) {
            char tid_lower[16];
            strncpy(tid_lower, titles[selected].title_id, sizeof(tid_lower) - 1);
            tid_lower[sizeof(tid_lower) - 1] = '\0';
            for (int k = 0; tid_lower[k]; k++)
                if (tid_lower[k] >= 'A' && tid_lower[k] <= 'Z')
                    tid_lower[k] += 32;

            char local_root[MAX_PATH_LEN];
            char parent_root[MAX_PATH_LEN];
            char remote_root[MAX_PATH_LEN];
            char timestamp[32];
            get_timestamp(timestamp, sizeof(timestamp));

            snprintf(local_root,  sizeof(local_root),
                     "E:\\UDATA\\%s", titles[selected].title_id);
            snprintf(parent_root, sizeof(parent_root),
                     "%s/%s", cfg->remote_base, tid_lower);
            snprintf(remote_root, sizeof(remote_root),
                     "%s/%s", parent_root, timestamp);

            /* ── Vooraf scannen voor totaal aantal bestanden/bytes ──
               Dit geeft ui_progress een geldige noemer (X / TOTAAL)
               voordat er ook maar één byte geupload is. Zonder deze
               stap zou files_total pas na de volledige upload bekend
               zijn, en dus nooit zinnig te tonen tijdens het proces. */
            static FileEntry scan_entries[1024];
            int scan_count = fileops_scan(local_root, scan_entries, 1024);

            TransferState progress;
            memset(&progress, 0, sizeof(progress));
            progress.active      = 1;
            progress.cancellable = 1;   /* upload mag onderbroken worden met B */

            /* files_total moet alleen BESTANDEN tellen, niet mappen --
               fileops_scan levert beide in dezelfde lijst terug. */
            int file_count = 0;
            for (int k = 0; k < scan_count; k++)
                if (!scan_entries[k].is_dir) file_count++;
            progress.files_total = file_count;
            progress.bytes_total = fileops_total_size(scan_entries, scan_count);
            snprintf(progress.status_msg, sizeof(progress.status_msg),
                     "Uploading %s ...", titles[selected].title_name);

            webdav_mkcol(parent_root, creds64, cfg->host, cfg->port);
            int uploaded = upload_dir(local_root, remote_root,
                                      creds64, cfg->host, cfg->port,
                                      &progress);

            if (uploaded < 0)
                snprintf(status_msg, sizeof(status_msg), "Cancelled");
            else if (uploaded > 0)
                snprintf(status_msg, sizeof(status_msg),
                         "OK: %d files -> %s", uploaded, timestamp);
            else
                snprintf(status_msg, sizeof(status_msg),
                         "Failed / no files found");

            redraw = 1;
        }

        /* ── Y: verwijder lokale save van Xbox ──────────────────── */
        if (btn_pressed(BTN_Y)) {
            /* Bevestigingsscherm */
            ui_clear();
            ui_draw_text(MARGIN_X, MARGIN_Y, "-= Delete Save =-", UI_COL_TITLE);
            ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H * 2 + 4,
                         SCREEN_W - MARGIN_X * 2, 1, UI_COL_DIM);

            int dy = MARGIN_Y + CHAR_H * 4;
            char dline[80];
            snprintf(dline, sizeof(dline), "Game: %s", titles[selected].title_name);
            ui_draw_text(MARGIN_X, dy, dline, UI_COL_ITEM); dy += CHAR_H * 2;
            snprintf(dline, sizeof(dline), "ID  : %s", titles[selected].title_id);
            ui_draw_text(MARGIN_X, dy, dline, UI_COL_ITEM); dy += CHAR_H * 3;
            ui_draw_text(MARGIN_X, dy,
                         "This will PERMANENTLY delete the local save!",
                         UI_COL_ERROR);
            ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                         "A=confirm delete  B=cancel", UI_COL_DIM);
            ui_flip();

            int confirmed = 0;
            while (1) {
                ui_pump_events();
                if (btn_pressed(BTN_A)) { confirmed = 1; break; }
                if (btn_pressed(BTN_B)) break;
                SDL_Delay(16);
            }
            
            if (confirmed) {
                char local_path[MAX_PATH_LEN];
                snprintf(local_path, sizeof(local_path),
                         "E:\\UDATA\\%s", titles[selected].title_id);

                log_print("backup: delete local save %s\n", local_path);
                delete_local_dir(local_path);

                /* Verwijder entry direct uit array zonder rescan */
                for (int k = selected; k < *n - 1; k++)
                    titles[k] = titles[k + 1];
                (*n)--;

                if (selected >= *n) selected = *n - 1;
                if (selected < 0)   selected = 0;

                if (*n == 0) return;  /* geen titels meer over */

                snprintf(status_msg, sizeof(status_msg), "Deleted. %d titles remaining.", *n);
                log_print("backup: removed entry, %d titles remaining\n", *n);
            }

            redraw = 1;
        }

        if (redraw) {
            draw_list(titles, *n, selected, status_msg);
            redraw = 0;
        }

        SDL_Delay(16);
    }
}
