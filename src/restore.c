#include "../lib/ui.h"
#include "../lib/util.h"
#include "config.h"
#include "restore.h"
#include "resign.h"
#include "fileops.h"
#include "webdav.h"
#include "titledb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* Logt én toont een nowait bericht */
#define LOG_MSG(title, msg) \
    do { log_print("[UI] %s: %s\n", (title), (msg)); ui_message_nowait((title), (msg)); } while(0)

/*
 * restore.c  –  Restore een geselecteerde backup van de WebDAV server
 *
 * Remote structuur (aangemaakt door backup):
 *   <remote_base>/<TitleID>/<timestamp>/...save bestanden...
 *
 * Stappen:
 *   1. List <remote_base>/             → kies TitleID
 *   2. List <remote_base>/<TitleID>/   → kies timestamp (gesorteerd, nieuwste bovenaan)
 *   3. Download <remote_base>/<TitleID>/<timestamp>/ → E:\UDATA\<TitleID>\
 *   4. Indien non-roamable: resign met lokale HDDKey
 */

/* ── Timestamp sortering: nieuwste eerst ────────────────────────── */
/*
 * Timestamps zijn van de vorm "YYYYMMDD_HHMMSS" (lexicografisch sorteerbaar).
 * We sorteren descending zodat de meest recente backup bovenaan staat.
 */
static int cmp_ts_desc(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);
}

/* ── Teken een eenvoudige keuzelijst ────────────────────────────── */
static void draw_pick_list(const char *title,
                           char items[][MAX_PATH_LEN], int n,
                           int selected, const char *hint)
{
    ui_clear();
    ui_draw_text(MARGIN_X, MARGIN_Y, title, UI_COL_TITLE);
    ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H * 2 + 4,
                 SCREEN_W - MARGIN_X * 2, 1, UI_COL_DIM);

    int page       = selected / LIST_PAGE_SIZE;
    int page_start = page * LIST_PAGE_SIZE;
    int page_end   = page_start + LIST_PAGE_SIZE;
    if (page_end > n) page_end = n;

    for (int i = page_start; i < page_end; i++) {
        int row = i - page_start;
        int y   = MARGIN_Y + CHAR_H * (4 + row);
        int sel = (i == selected);

        if (sel)
            ui_fill_rect(MARGIN_X - 4, y - 2,
                         SCREEN_W - MARGIN_X * 2 + 8, CHAR_H + 4, UI_COL_SELBG);

        char line[MAX_PATH_LEN + 2];
        snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", items[i]);
        ui_draw_text(MARGIN_X, y, line, sel ? UI_COL_SELECT : UI_COL_ITEM);
    }

    if (hint)
        ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H, hint, UI_COL_DIM);
    ui_flip();
}

/* ── Recursief een map downloaden ───────────────────────────────── */
static int webdav_download_dir(const char *remote_dir, const char *local_dir,
                               const char *creds64, const char *host, int port)
{
    int success_count = 0;

    CreateDirectory(local_dir, NULL);

    typedef char PathBuf[MAX_PATH_LEN];
    PathBuf *items  = (PathBuf *)malloc(128 * sizeof(PathBuf));
    int     *is_dir = (int *)malloc(128 * sizeof(int));
    if (!items || !is_dir) {
        log_print("ERROR: malloc failed in download_dir (%s)\n", remote_dir);
        ui_message_nowait("Restore", "ERROR: malloc failed in download_dir");
        free(items); free(is_dir);
        return 0;
    }

    char status_msg[MAX_PATH_LEN + 64];
    snprintf(status_msg, sizeof(status_msg), "Listing: %s", remote_dir);
    LOG_MSG("Restore", status_msg);

    /* Gebruik _ex zodat we weten welke items mappen zijn zonder
     * een tweede PROPFIND per item te doen. Die tweede PROPFIND
     * gaf 207 terug voor BESTANDEN waardoor ze als map werden
     * behandeld en nooit gedownload werden. */
    int n = webdav_list_directory_ex(remote_dir, items, is_dir, 128,
                                     creds64, host, port);

    snprintf(status_msg, sizeof(status_msg), "Found %d items in %s", n, remote_dir);
    LOG_MSG("Restore", status_msg);

    if (n < 0) {
        snprintf(status_msg, sizeof(status_msg),
                 "ERROR: PROPFIND failed voor %s", remote_dir);
        LOG_MSG("Restore", status_msg);
        SDL_Delay(2000);
        free(items); free(is_dir);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        char remote_child[MAX_PATH_LEN];
        char local_child[MAX_PATH_LEN];
        snprintf(remote_child, sizeof(remote_child), "%s/%s", remote_dir, items[i]);
        snprintf(local_child,  sizeof(local_child),  "%s\\%s", local_dir,  items[i]);

        log_print("item[%d]: %s  is_dir=%d\n", i, items[i], is_dir[i]);

        if (is_dir[i]) {
            snprintf(status_msg, sizeof(status_msg), "Dir: %s", items[i]);
            LOG_MSG("Restore", status_msg);
            success_count += webdav_download_dir(remote_child, local_child,
                                                 creds64, host, port);
        } else {
            snprintf(status_msg, sizeof(status_msg), "GET %s", items[i]);
            LOG_MSG("Restore", status_msg);

            int r = webdav_get_file(remote_child, local_child, creds64, host, port);
            if (r == 0) {
                success_count++;
                snprintf(status_msg, sizeof(status_msg), "OK (%d) %s", success_count, items[i]);
            } else {
                snprintf(status_msg, sizeof(status_msg), "FAILED (GET=%d) %s", r, items[i]);
            }
            LOG_MSG("Restore", status_msg);
            SDL_Delay(300);
        }
    }

    free(items);
    free(is_dir);
    return success_count;
}

/* ── Verwijder een remote map recursief via WebDAV DELETE ───────── */
static int webdav_delete_dir(const char *remote_path,
                              const char *creds64,
                              const char *host, int port)
{
    int status = 0;
    int r = webdav_request("DELETE", remote_path, host, port, creds64,
                            "Depth: infinity\r\n",
                            NULL, 0, NULL, 0, &status);
    log_print("DELETE %s -> HTTP %d\n", remote_path, status);
    return (r >= 0 && (status == 200 || status == 204 || status == 207)) ? 0 : -1;
}

/* ── Hoofdfunctie: restore flow ─────────────────────────────────── */
void do_restore(const char *creds64, const AppConfig *cfg)
{
    /* ── Stap 1: kies TitleID ───────────────────────────────────── */
    static char title_list[128][MAX_PATH_LEN];
    int title_count;

    log_print("[Stap 1] Ophalen titellijst van server: %s\n", cfg->remote_base);
    ui_message_nowait("Restore", "Loading title list from server...");
    title_count = webdav_list_directory(cfg->remote_base, title_list, 128,
                                        creds64, cfg->host, cfg->port, 1);
    log_print("[Stap 1] Gevonden titels: %d\n", title_count);
    if (title_count <= 0) {
        log_print("[Stap 1] FOUT: geen titels gevonden op server\n");
        ui_message("Restore", "No titles found on server.");
        return;
    }

    static char title_display[128][MAX_PATH_LEN];
    for (int i = 0; i < title_count; i++) {
        uint32_t title_id = (uint32_t)strtoul(title_list[i], NULL, 16);
        const char *name = title_lookup(title_id);
        if (name) {
            snprintf(title_display[i], sizeof(title_display[i]),
                     "[%s] %s", title_list[i], name);
        } else {
            snprintf(title_display[i], sizeof(title_display[i]),
                     "[%s]", title_list[i]);
        }
    }

    int title_sel = 0;
    int redraw    = 1;
    while (1) {
        ui_pump_events();
        if (btn_pressed(DPAD_DOWN)) { title_sel = (title_sel + 1) % title_count; redraw = 1; }
        if (btn_pressed(DPAD_UP))   { title_sel = (title_sel + title_count - 1) % title_count; redraw = 1; }
        if (btn_pressed(BTN_B))     return;
        if (btn_pressed(BTN_A))     break;
        if (redraw) {
            draw_pick_list("SaveSyncX v1.0 - Restore",
                           title_display, title_count, title_sel,
                           "up/down=scroll  A=select  B=back  Y=delete");
            redraw = 0;
        }
        SDL_Delay(16);
    }

    /* Bewaar de gekozen titel apart voor gebruik straks */
    char chosen_title[MAX_PATH_LEN];
    strncpy(chosen_title, title_list[title_sel], sizeof(chosen_title) - 1);
    chosen_title[sizeof(chosen_title) - 1] = '\0';
    log_print("[Stap 1] Gebruiker koos titel: %s\n", chosen_title);

    /* ── Stap 2: kies timestamp (nieuwste eerst) ────────────────── */
    static char backup_list[128][MAX_PATH_LEN];
    int backup_count;

    char title_remote_path[MAX_PATH_LEN];
    snprintf(title_remote_path, sizeof(title_remote_path),
             "%s/%s", cfg->remote_base, chosen_title);

    {
        char msg[MAX_PATH_LEN + 32];
        snprintf(msg, sizeof(msg), "Loading backups for %s...", chosen_title);
        log_print("[Stap 2] Ophalen backuplijst: %s\n", title_remote_path);
        ui_message_nowait("Restore", msg);
    }

    backup_count = webdav_list_directory(title_remote_path, backup_list, 128,
                                          creds64, cfg->host, cfg->port, 1);
    log_print("[Stap 2] Gevonden backups: %d\n", backup_count);
    if (backup_count <= 0) {
        log_print("[Stap 2] FOUT: geen backups gevonden voor %s\n", chosen_title);
        ui_message("Restore", "No backups found for this title.");
        return;
    }

    /* Sorteer: nieuwste timestamp bovenaan */
    qsort(backup_list, backup_count, MAX_PATH_LEN, cmp_ts_desc);

    int backup_sel = 0;
    redraw = 1;
    while (1) {
        ui_pump_events();
        if (btn_pressed(DPAD_DOWN)) { backup_sel = (backup_sel + 1) % backup_count; redraw = 1; }
        if (btn_pressed(DPAD_UP))   { backup_sel = (backup_sel + backup_count - 1) % backup_count; redraw = 1; }
        if (btn_pressed(BTN_B))     return;
        if (btn_pressed(BTN_A))     break;
        if (btn_pressed(BTN_Y)) {
            /* Bevestigingsscherm voor delete */
            ui_clear();
            ui_draw_text(MARGIN_X, MARGIN_Y, "-= Delete Backup =-", UI_COL_TITLE);
            ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H * 2 + 4,
                         SCREEN_W - MARGIN_X * 2, 1, UI_COL_DIM);

            int dy = MARGIN_Y + CHAR_H * 4;
            char dline[MAX_PATH_LEN + 32];
            snprintf(dline, sizeof(dline), "Title  : %s", title_display[title_sel]);
            ui_draw_text(MARGIN_X, dy, dline, UI_COL_ITEM); dy += CHAR_H * 2;
            snprintf(dline, sizeof(dline), "Backup : %s", backup_list[backup_sel]);
            ui_draw_text(MARGIN_X, dy, dline, UI_COL_ITEM); dy += CHAR_H * 3;
            ui_draw_text(MARGIN_X, dy, "This will PERMANENTLY delete this backup!",
                         UI_COL_ERROR);
            ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                         "A=confirm delete  B=cancel", UI_COL_DIM);
            ui_flip();

            /* Wacht op bevestiging */
            int confirmed = 0;
            while (1) {
                ui_pump_events();
                if (btn_pressed(BTN_A)) { confirmed = 1; break; }
                if (btn_pressed(BTN_B)) break;
                SDL_Delay(16);
            }

            if (confirmed) {
                char del_path[MAX_PATH_LEN];
                snprintf(del_path, sizeof(del_path),
                         "%s/%s/%s", cfg->remote_base,
                         chosen_title, backup_list[backup_sel]);

                ui_message_nowait("Delete", "Deleting backup...");
                int r = webdav_delete_dir(del_path, creds64,
                                          cfg->host, cfg->port);
                if (r == 0) {
                    log_print("Delete: backup %s verwijderd\n", del_path);

                    /* Verwijder uit de lijst en herlaad */
                    for (int k = backup_sel; k < backup_count - 1; k++)
                        memcpy(backup_list[k], backup_list[k+1], MAX_PATH_LEN);
                    backup_count--;

                    if (backup_count == 0) {
                        ui_message("Delete", "Last backup deleted. Returning to title list.");
                        return;
                    }
                    if (backup_sel >= backup_count)
                        backup_sel = backup_count - 1;

                    ui_message_nowait("Delete", "Backup deleted.");
                } else {
                    log_print("Delete: FAILED voor %s\n", del_path);
                    ui_message_nowait("Delete", "Delete failed! Check log for details.");
                }
            }
            redraw = 1;
        }
        if (redraw) {
            char list_title[MAX_PATH_LEN + 32];
            snprintf(list_title, sizeof(list_title),
                     "-= Restore: %s =-", chosen_title);
            draw_pick_list(list_title, backup_list, backup_count, backup_sel,
                           "up/down=scroll  A=restore  B=back");
            redraw = 0;
        }
        SDL_Delay(16);
    }

    char chosen_backup[MAX_PATH_LEN];
    strncpy(chosen_backup, backup_list[backup_sel], sizeof(chosen_backup) - 1);
    chosen_backup[sizeof(chosen_backup) - 1] = '\0';
    log_print("[Stap 2] Gebruiker koos backup: %s\n", chosen_backup);

    /* ── Stap 3: bevestiging ────────────────────────────────────── */

    /* Toon bevestigingsscherm */
    ui_clear();
    ui_draw_text(MARGIN_X, MARGIN_Y, "-= Confirm Restore =-", UI_COL_TITLE);
    ui_fill_rect(MARGIN_X, MARGIN_Y + CHAR_H * 2 + 4,
                 SCREEN_W - MARGIN_X * 2, 1, UI_COL_DIM);

    int y = MARGIN_Y + CHAR_H * 4;
    char line[MAX_PATH_LEN + 32];

    snprintf(line, sizeof(line), "Title  : %s", chosen_title);
    ui_draw_text(MARGIN_X, y, line, UI_COL_ITEM); y += CHAR_H * 2;

    snprintf(line, sizeof(line), "Backup : %s", chosen_backup);
    ui_draw_text(MARGIN_X, y, line, UI_COL_ITEM); y += CHAR_H * 3;

    ui_draw_text(MARGIN_X, y, "This will OVERWRITE the local save!", UI_COL_ERROR); y += CHAR_H * 2;

    /* Resign waarschuwing als van toepassing */
    const NonRoamableEntry *nr_check = resign_lookup(chosen_title);
    if (nr_check && nr_check->sign_type != SIGN_NONE) {
        if (nr_check->sign_type == SIGN_FORZA) {
            ui_draw_text(MARGIN_X, y,
                "WARNING: Forza save requires ForzaSign on PC after restore!",
                UI_COL_ERROR); y += CHAR_H * 2;
        } else {
            ui_draw_text(MARGIN_X, y,
                "Note: EEPROM-locked save will be re-signed for this console.",
                UI_COL_ITEM);
        }
    }

    ui_draw_text(MARGIN_X, SCREEN_H - MARGIN_Y - CHAR_H,
                 "A=confirm  B=cancel", UI_COL_DIM);
    ui_flip();

    log_print("[Stap 3] Bevestigingsscherm getoond: titel=%s backup=%s\n", chosen_title, chosen_backup);

    /* Wacht op A of B */
    while (1) {
        ui_pump_events();
        if (btn_pressed(BTN_B)) {
            log_print("[Stap 3] Gebruiker annuleerde restore\n");
            return;
        }
        if (btn_pressed(BTN_A)) break;
        SDL_Delay(16);
    }
    log_print("[Stap 3] Gebruiker bevestigde restore\n");

    /* ── Stap 4: download ───────────────────────────────────────── */
    char remote_backup_path[MAX_PATH_LEN];
    char local_title_path[MAX_PATH_LEN];

    /* chosen_title komt van server (lowercase) — uppercase voor lokaal pad */
    snprintf(remote_backup_path, sizeof(remote_backup_path),
             "%s/%s/%s", cfg->remote_base, chosen_title, chosen_backup);
    snprintf(local_title_path, sizeof(local_title_path),
             "E:\\UDATA\\%s", chosen_title);

    log_print("[Stap 4] Download gestart\n");
    log_print("[Stap 4] Remote pad : %s\n", remote_backup_path);
    log_print("[Stap 4] Lokaal pad : %s\n", local_title_path);
    ui_message_nowait("Restore", "Downloading, please wait...");
    int restored = webdav_download_dir(remote_backup_path, local_title_path,
                                       creds64, cfg->host, cfg->port);

    log_print("[Stap 4] Download klaar: %d bestanden ontvangen\n", restored);

    /* ── Stap 5: resign indien non-roamable ─────────────────────── */
    const NonRoamableEntry *nr = resign_lookup(chosen_title);
    int resigned = 0;
    log_print("[Stap 5] Resign check voor %s\n", chosen_title);
    if (nr != NULL && nr->sign_type != SIGN_NONE) {
        unsigned char local_key[HDDKEY_SIZE];
        if (resign_read_hddkey(local_key) == 0) {
            /*
             * We hebben de bronkey niet lokaal opgeslagen in deze versie.
             * Gebruik dezelfde key voor old en new: dit werkt als de backup
             * gemaakt is op deze zelfde console. Als de save van een andere
             * console komt, moet de bronkey uit een .key bestand komen.
             * TODO: upload/download hddkey per title.
             */
            unsigned char old_key[HDDKEY_SIZE];
            memcpy(old_key, local_key, HDDKEY_SIZE);
            resigned = resign_process_title(local_title_path, nr, old_key, local_key);
            log_print("resign: %d files re-signed for %s\n", resigned, chosen_title);
        } else {
            log_print("resign: failed to read HDDKey, skipping resign\n");
        }
    }

    /* ── Resultaat ──────────────────────────────────────────────── */
    char result[256];
    if (restored == 0) {
        snprintf(result, sizeof(result), "Download failed: 0 files received.\nCheck C:\\savesyncx.log for details.");
    } else if (nr && nr->sign_type == SIGN_FORZA) {
        snprintf(result, sizeof(result),
                 "Downloaded %d files.\n\nForza save: use ForzaSign\non PC to re-sign before playing.",
                 restored);
    } else if (nr && nr->sign_type != SIGN_NONE) {
        snprintf(result, sizeof(result),
                 "Downloaded %d files.\nRe-signed %d files for this console.",
                 restored, resigned);
    } else {
        snprintf(result, sizeof(result), "Downloaded %d files.", restored);
    }
    log_print("Restore klaar: %s\n", result);
    ui_message("Restore complete", result);
}